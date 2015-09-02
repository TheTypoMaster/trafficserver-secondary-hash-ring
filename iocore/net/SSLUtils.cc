/** @file

  @section license License

  Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
 */

#include "ink_config.h"
#include "records/I_RecHttp.h"
#include "libts.h"
#include "I_Layout.h"
#include "P_Net.h"
#include "ink_cap.h"
#include "P_OCSPStapling.h"
#include "SSLSessionCache.h"

#include <string>
#include <openssl/err.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/asn1.h>
#include <openssl/rand.h>
#include <openssl/dh.h>
#include <openssl/bn.h>
#include <unistd.h>
#include <termios.h>

#if HAVE_OPENSSL_EVP_H
#include <openssl/evp.h>
#endif

#if HAVE_OPENSSL_HMAC_H
#include <openssl/hmac.h>
#endif

#if HAVE_OPENSSL_TS_H
#include <openssl/ts.h>
#endif

#if HAVE_OPENSSL_EC_H
#include <openssl/ec.h>
#endif

// ssl_multicert.config field names:
#define SSL_IP_TAG "dest_ip"
#define SSL_CERT_TAG "ssl_cert_name"
#define SSL_PRIVATE_KEY_TAG "ssl_key_name"
#define SSL_CA_TAG "ssl_ca_name"
#define SSL_ACTION_TAG "action"
#define SSL_ACTION_TUNNEL_TAG "tunnel"
#define SSL_SESSION_TICKET_ENABLED "ssl_ticket_enabled"
#define SSL_SESSION_TICKET_KEY_FILE_TAG "ticket_key_name"
#define SSL_KEY_DIALOG "ssl_key_dialog"
#define SSL_CERT_SEPARATE_DELIM ','

// openssl version must be 0.9.4 or greater
#if (OPENSSL_VERSION_NUMBER < 0x00090400L)
#error Traffic Server requires an OpenSSL library version 0.9.4 or greater
#endif

#ifndef evp_md_func
#ifdef OPENSSL_NO_SHA256
#define evp_md_func EVP_sha1()
#else
#define evp_md_func EVP_sha256()
#endif
#endif

#if (OPENSSL_VERSION_NUMBER >= 0x10000000L) // openssl returns a const SSL_METHOD
typedef const SSL_METHOD *ink_ssl_method_t;
#else
typedef SSL_METHOD *ink_ssl_method_t;
#endif

// gather user provided settings from ssl_multicert.config in to a single struct
struct ssl_user_config {
  ssl_user_config() : session_ticket_enabled(1), opt(SSLCertContext::OPT_NONE) {}

  int session_ticket_enabled; // ssl_ticket_enabled - session ticket enabled
  ats_scoped_str addr;        // dest_ip - IPv[64] address to match
  ats_scoped_str cert;        // ssl_cert_name - certificate
  ats_scoped_str first_cert;  // the first certificate name when multiple cert files are in 'ssl_cert_name'
  ats_scoped_str ca;          // ssl_ca_name - CA public certificate
  ats_scoped_str key;         // ssl_key_name - Private key
  ats_scoped_str
    ticket_key_filename; // ticket_key_name - session key file. [key_name (16Byte) + HMAC_secret (16Byte) + AES_key (16Byte)]
  ats_scoped_str dialog; // ssl_key_dialog - Private key dialog
  SSLCertContext::Option opt;
};

SSLSessionCache *session_cache; // declared extern in P_SSLConfig.h

// Check if the ticket_key callback #define is available, and if so, enable session tickets.
#ifdef SSL_CTX_set_tlsext_ticket_key_cb

#define HAVE_OPENSSL_SESSION_TICKETS 1

static void session_ticket_free(void *, void *, CRYPTO_EX_DATA *, int, long, void *);
static int ssl_callback_session_ticket(SSL *, unsigned char *, unsigned char *, EVP_CIPHER_CTX *, HMAC_CTX *, int);
#endif /* SSL_CTX_set_tlsext_ticket_key_cb */

#if HAVE_OPENSSL_SESSION_TICKETS
static int ssl_session_ticket_index = -1;
#endif


static pthread_mutex_t *mutex_buf = NULL;
static bool open_ssl_initialized = false;

RecRawStatBlock *ssl_rsb = NULL;
static InkHashTable *ssl_cipher_name_table = NULL;

/* Using pthread thread ID and mutex functions directly, instead of
 * ATS this_ethread / ProxyMutex, so that other linked libraries
 * may use pthreads and openssl without confusing us here. (TS-2271).
 */

static unsigned long
SSL_pthreads_thread_id()
{
  return (unsigned long)pthread_self();
}

static void
SSL_locking_callback(int mode, int type, const char * /* file ATS_UNUSED */, int /* line ATS_UNUSED */)
{
  ink_assert(type < CRYPTO_num_locks());

  if (mode & CRYPTO_LOCK) {
    pthread_mutex_lock(&mutex_buf[type]);
  } else if (mode & CRYPTO_UNLOCK) {
    pthread_mutex_unlock(&mutex_buf[type]);
  } else {
    Debug("ssl", "invalid SSL locking mode 0x%x", mode);
    ink_assert(0);
  }
}

static bool
SSL_CTX_add_extra_chain_cert_file(SSL_CTX *ctx, const char *chainfile)
{
  X509 *cert;
  scoped_BIO bio(BIO_new_file(chainfile, "r"));

  for (;;) {
    cert = PEM_read_bio_X509_AUX(bio.get(), NULL, NULL, NULL);

    if (!cert) {
      // No more the certificates in this file.
      break;
    }

    // This transfers ownership of the cert (X509) to the SSL context, if successful.
    if (!SSL_CTX_add_extra_chain_cert(ctx, cert)) {
      X509_free(cert);
      return false;
    }
  }

  return true;
}


static SSL_SESSION *
ssl_get_cached_session(SSL *ssl, unsigned char *id, int len, int *copy)
{
  SSLSessionID sid(id, len);

  *copy = 0;
  if (diags->tag_activated("ssl.session_cache")) {
    char printable_buf[(len * 2) + 1];
    sid.toString(printable_buf, sizeof(printable_buf));
    Debug("ssl.session_cache.get", "ssl_get_cached_session cached session '%s' context %p", printable_buf, SSL_get_SSL_CTX(ssl));
  }

  SSL_SESSION *session = NULL;

  if (session_cache->getSession(sid, &session)) {
    return session;
  }

  return NULL;
}

static int
ssl_new_cached_session(SSL *ssl, SSL_SESSION *sess)
{
  unsigned int len = 0;
  const unsigned char *id = SSL_SESSION_get_id(sess, &len);
  SSLSessionID sid(id, len);

  if (diags->tag_activated("ssl.session_cache")) {
    char printable_buf[(len * 2) + 1];

    sid.toString(printable_buf, sizeof(printable_buf));
    Debug("ssl.session_cache.insert", "ssl_new_cached_session session '%s' and context %p", printable_buf, SSL_get_SSL_CTX(ssl));
  }

  SSL_INCREMENT_DYN_STAT(ssl_session_cache_new_session);
  session_cache->insertSession(sid, sess);

  return 0;
}

static void
ssl_rm_cached_session(SSL_CTX *ctx, SSL_SESSION *sess)
{
  SSL_CTX_remove_session(ctx, sess);

  unsigned int len = 0;
  const unsigned char *id = SSL_SESSION_get_id(sess, &len);
  SSLSessionID sid(id, len);

  if (diags->tag_activated("ssl.session_cache")) {
    char printable_buf[(len * 2) + 1];
    sid.toString(printable_buf, sizeof(printable_buf));
    Debug("ssl.session_cache.remove", "ssl_rm_cached_session cached session '%s'", printable_buf);
  }

  session_cache->removeSession(sid);
}

#if TS_USE_TLS_SNI
int
set_context_cert(SSL *ssl)
{
  SSL_CTX *ctx = NULL;
  SSLCertContext *cc = NULL;
  SSLCertificateConfig::scoped_config lookup;
  const char *servername = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
  SSLNetVConnection *netvc = (SSLNetVConnection *)SSL_get_app_data(ssl);
  bool found = true;
  int retval = 1;

  Debug("ssl", "set_context_cert ssl=%p server=%s handshake_complete=%d", ssl, servername, netvc->getSSLHandShakeComplete());

  // catch the client renegotiation early on
  if (SSLConfigParams::ssl_allow_client_renegotiation == false && netvc->getSSLHandShakeComplete()) {
    Debug("ssl", "set_context_cert trying to renegotiate from the client");
    retval = 0; // Error
    goto done;
  }

  // The incoming SSL_CTX is either the one mapped from the inbound IP address or the default one. If we
  // don't find a name-based match at this point, we *do not* want to mess with the context because we've
  // already made a best effort to find the best match.
  if (likely(servername)) {
    cc = lookup->find((char *)servername);
    if (cc && cc->ctx)
      ctx = cc->ctx;
    if (cc && SSLCertContext::OPT_TUNNEL == cc->opt && netvc->get_is_transparent()) {
      netvc->attributes = HttpProxyPort::TRANSPORT_BLIND_TUNNEL;
      netvc->setSSLHandShakeComplete(true);
      retval = -1;
      goto done;
    }
  }

  // If there's no match on the server name, try to match on the peer address.
  if (ctx == NULL) {
    IpEndpoint ip;
    int namelen = sizeof(ip);

    safe_getsockname(netvc->get_socket(), &ip.sa, &namelen);
    cc = lookup->find(ip);
    if (cc && cc->ctx)
      ctx = cc->ctx;
  }

  if (ctx != NULL) {
    SSL_set_SSL_CTX(ssl, ctx);
#if HAVE_OPENSSL_SESSION_TICKETS
    // Reset the ticket callback if needed
    SSL_CTX_set_tlsext_ticket_key_cb(ctx, ssl_callback_session_ticket);
#endif
  } else {
    found = false;
  }

  ctx = SSL_get_SSL_CTX(ssl);
  Debug("ssl", "ssl_cert_callback %s SSL context %p for requested name '%s'", found ? "found" : "using", ctx, servername);

  if (ctx == NULL) {
    retval = 0;
    goto done;
  }
done:
  return retval;
}

// Use the certificate callback for openssl 1.0.2 and greater
// otherwise use the SNI callback
#if TS_USE_CERT_CB
/**
 * Called before either the server or the client certificate is used
 * Return 1 on success, 0 on error, or -1 to pause
 */
static int
ssl_cert_callback(SSL *ssl, void * /*arg*/)
{
  SSLNetVConnection *netvc = (SSLNetVConnection *)SSL_get_app_data(ssl);
  bool reenabled;
  int retval = 1;

  // Do the common certificate lookup only once.  If we pause
  // and restart processing, do not execute the common logic again
  if (!netvc->calledHooks(TS_SSL_CERT_HOOK)) {
    retval = set_context_cert(ssl);
    if (retval != 1) {
      return retval;
    }
  }

  // Call the plugin cert code
  reenabled = netvc->callHooks(TS_SSL_CERT_HOOK);
  // If it did not re-enable, return the code to
  // stop the accept processing
  if (!reenabled) {
    retval = -1; // Pause
  }

  // Return 1 for success, 0 for error, or -1 to pause
  return retval;
}
#else
static int
ssl_servername_callback(SSL *ssl, int * /* ad */, void * /*arg*/)
{
  SSLNetVConnection *netvc = (SSLNetVConnection *)SSL_get_app_data(ssl);
  bool reenabled;
  int retval = 1;

  // Do the common certificate lookup only once.  If we pause
  // and restart processing, do not execute the common logic again
  if (!netvc->calledHooks(TS_SSL_CERT_HOOK)) {
    retval = set_context_cert(ssl);
    if (retval != 1) {
      goto done;
    }
  }

  // Call the plugin SNI code
  reenabled = netvc->callHooks(TS_SSL_SNI_HOOK);
  // If it did not re-enable, return the code to
  // stop the accept processing
  if (!reenabled) {
    retval = -1;
  }

done:
  // Map 1 to SSL_TLSEXT_ERR_OK
  // Map 0 to SSL_TLSEXT_ERR_ALERT_FATAL
  // Map -1 to SSL_TLSEXT_ERR_READ_AGAIN, if present
  switch (retval) {
  case 1:
    retval = SSL_TLSEXT_ERR_OK;
    break;
  case -1:
#ifdef SSL_TLSEXT_ERR_READ_AGAIN
    retval = SSL_TLSEXT_ERR_READ_AGAIN;
#else
    Error("Cannot pause SNI processsing with this version of openssl");
    retval = SSL_TLSEXT_ERR_ALERT_FATAL;
#endif
    break;
  case 0:
  default:
    retval = SSL_TLSEXT_ERR_ALERT_FATAL;
    break;
  }
  return retval;
}
#endif
#endif /* TS_USE_TLS_SNI */

/* Build 2048-bit MODP Group with 256-bit Prime Order Subgroup from RFC 5114 */
static DH *
get_dh2048()
{
  static const unsigned char dh2048_p[] = {
    0x87, 0xA8, 0xE6, 0x1D, 0xB4, 0xB6, 0x66, 0x3C, 0xFF, 0xBB, 0xD1, 0x9C, 0x65, 0x19, 0x59, 0x99, 0x8C, 0xEE, 0xF6, 0x08,
    0x66, 0x0D, 0xD0, 0xF2, 0x5D, 0x2C, 0xEE, 0xD4, 0x43, 0x5E, 0x3B, 0x00, 0xE0, 0x0D, 0xF8, 0xF1, 0xD6, 0x19, 0x57, 0xD4,
    0xFA, 0xF7, 0xDF, 0x45, 0x61, 0xB2, 0xAA, 0x30, 0x16, 0xC3, 0xD9, 0x11, 0x34, 0x09, 0x6F, 0xAA, 0x3B, 0xF4, 0x29, 0x6D,
    0x83, 0x0E, 0x9A, 0x7C, 0x20, 0x9E, 0x0C, 0x64, 0x97, 0x51, 0x7A, 0xBD, 0x5A, 0x8A, 0x9D, 0x30, 0x6B, 0xCF, 0x67, 0xED,
    0x91, 0xF9, 0xE6, 0x72, 0x5B, 0x47, 0x58, 0xC0, 0x22, 0xE0, 0xB1, 0xEF, 0x42, 0x75, 0xBF, 0x7B, 0x6C, 0x5B, 0xFC, 0x11,
    0xD4, 0x5F, 0x90, 0x88, 0xB9, 0x41, 0xF5, 0x4E, 0xB1, 0xE5, 0x9B, 0xB8, 0xBC, 0x39, 0xA0, 0xBF, 0x12, 0x30, 0x7F, 0x5C,
    0x4F, 0xDB, 0x70, 0xC5, 0x81, 0xB2, 0x3F, 0x76, 0xB6, 0x3A, 0xCA, 0xE1, 0xCA, 0xA6, 0xB7, 0x90, 0x2D, 0x52, 0x52, 0x67,
    0x35, 0x48, 0x8A, 0x0E, 0xF1, 0x3C, 0x6D, 0x9A, 0x51, 0xBF, 0xA4, 0xAB, 0x3A, 0xD8, 0x34, 0x77, 0x96, 0x52, 0x4D, 0x8E,
    0xF6, 0xA1, 0x67, 0xB5, 0xA4, 0x18, 0x25, 0xD9, 0x67, 0xE1, 0x44, 0xE5, 0x14, 0x05, 0x64, 0x25, 0x1C, 0xCA, 0xCB, 0x83,
    0xE6, 0xB4, 0x86, 0xF6, 0xB3, 0xCA, 0x3F, 0x79, 0x71, 0x50, 0x60, 0x26, 0xC0, 0xB8, 0x57, 0xF6, 0x89, 0x96, 0x28, 0x56,
    0xDE, 0xD4, 0x01, 0x0A, 0xBD, 0x0B, 0xE6, 0x21, 0xC3, 0xA3, 0x96, 0x0A, 0x54, 0xE7, 0x10, 0xC3, 0x75, 0xF2, 0x63, 0x75,
    0xD7, 0x01, 0x41, 0x03, 0xA4, 0xB5, 0x43, 0x30, 0xC1, 0x98, 0xAF, 0x12, 0x61, 0x16, 0xD2, 0x27, 0x6E, 0x11, 0x71, 0x5F,
    0x69, 0x38, 0x77, 0xFA, 0xD7, 0xEF, 0x09, 0xCA, 0xDB, 0x09, 0x4A, 0xE9, 0x1E, 0x1A, 0x15, 0x97};
  static const unsigned char dh2048_g[] = {
    0x3F, 0xB3, 0x2C, 0x9B, 0x73, 0x13, 0x4D, 0x0B, 0x2E, 0x77, 0x50, 0x66, 0x60, 0xED, 0xBD, 0x48, 0x4C, 0xA7, 0xB1, 0x8F,
    0x21, 0xEF, 0x20, 0x54, 0x07, 0xF4, 0x79, 0x3A, 0x1A, 0x0B, 0xA1, 0x25, 0x10, 0xDB, 0xC1, 0x50, 0x77, 0xBE, 0x46, 0x3F,
    0xFF, 0x4F, 0xED, 0x4A, 0xAC, 0x0B, 0xB5, 0x55, 0xBE, 0x3A, 0x6C, 0x1B, 0x0C, 0x6B, 0x47, 0xB1, 0xBC, 0x37, 0x73, 0xBF,
    0x7E, 0x8C, 0x6F, 0x62, 0x90, 0x12, 0x28, 0xF8, 0xC2, 0x8C, 0xBB, 0x18, 0xA5, 0x5A, 0xE3, 0x13, 0x41, 0x00, 0x0A, 0x65,
    0x01, 0x96, 0xF9, 0x31, 0xC7, 0x7A, 0x57, 0xF2, 0xDD, 0xF4, 0x63, 0xE5, 0xE9, 0xEC, 0x14, 0x4B, 0x77, 0x7D, 0xE6, 0x2A,
    0xAA, 0xB8, 0xA8, 0x62, 0x8A, 0xC3, 0x76, 0xD2, 0x82, 0xD6, 0xED, 0x38, 0x64, 0xE6, 0x79, 0x82, 0x42, 0x8E, 0xBC, 0x83,
    0x1D, 0x14, 0x34, 0x8F, 0x6F, 0x2F, 0x91, 0x93, 0xB5, 0x04, 0x5A, 0xF2, 0x76, 0x71, 0x64, 0xE1, 0xDF, 0xC9, 0x67, 0xC1,
    0xFB, 0x3F, 0x2E, 0x55, 0xA4, 0xBD, 0x1B, 0xFF, 0xE8, 0x3B, 0x9C, 0x80, 0xD0, 0x52, 0xB9, 0x85, 0xD1, 0x82, 0xEA, 0x0A,
    0xDB, 0x2A, 0x3B, 0x73, 0x13, 0xD3, 0xFE, 0x14, 0xC8, 0x48, 0x4B, 0x1E, 0x05, 0x25, 0x88, 0xB9, 0xB7, 0xD2, 0xBB, 0xD2,
    0xDF, 0x01, 0x61, 0x99, 0xEC, 0xD0, 0x6E, 0x15, 0x57, 0xCD, 0x09, 0x15, 0xB3, 0x35, 0x3B, 0xBB, 0x64, 0xE0, 0xEC, 0x37,
    0x7F, 0xD0, 0x28, 0x37, 0x0D, 0xF9, 0x2B, 0x52, 0xC7, 0x89, 0x14, 0x28, 0xCD, 0xC6, 0x7E, 0xB6, 0x18, 0x4B, 0x52, 0x3D,
    0x1D, 0xB2, 0x46, 0xC3, 0x2F, 0x63, 0x07, 0x84, 0x90, 0xF0, 0x0E, 0xF8, 0xD6, 0x47, 0xD1, 0x48, 0xD4, 0x79, 0x54, 0x51,
    0x5E, 0x23, 0x27, 0xCF, 0xEF, 0x98, 0xC5, 0x82, 0x66, 0x4B, 0x4C, 0x0F, 0x6C, 0xC4, 0x16, 0x59};
  DH *dh;

  if ((dh = DH_new()) == NULL)
    return (NULL);
  dh->p = BN_bin2bn(dh2048_p, sizeof(dh2048_p), NULL);
  dh->g = BN_bin2bn(dh2048_g, sizeof(dh2048_g), NULL);
  if ((dh->p == NULL) || (dh->g == NULL)) {
    DH_free(dh);
    return (NULL);
  }
  return (dh);
}

static SSL_CTX *
ssl_context_enable_dhe(const char *dhparams_file, SSL_CTX *ctx)
{
  DH *server_dh;

  if (dhparams_file) {
    scoped_BIO bio(BIO_new_file(dhparams_file, "r"));
    server_dh = PEM_read_bio_DHparams(bio.get(), NULL, NULL, NULL);
  } else {
    server_dh = get_dh2048();
  }

  if (!server_dh) {
    Error("SSL dhparams source returned invalid parameters");
    return NULL;
  }

  if (!SSL_CTX_set_options(ctx, SSL_OP_SINGLE_DH_USE) || !SSL_CTX_set_tmp_dh(ctx, server_dh)) {
    DH_free(server_dh);
    Error("failed to configure SSL DH");
    return NULL;
  }

  DH_free(server_dh);

  return ctx;
}

static SSL_CTX *
ssl_context_enable_ecdh(SSL_CTX *ctx)
{
#if TS_USE_TLS_ECKEY

#if defined(SSL_CTRL_SET_ECDH_AUTO)
  SSL_CTX_set_ecdh_auto(ctx, 1);
#elif defined(HAVE_EC_KEY_NEW_BY_CURVE_NAME) && defined(NID_X9_62_prime256v1)
  EC_KEY *ecdh = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);

  if (ecdh) {
    SSL_CTX_set_tmp_ecdh(ctx, ecdh);
    EC_KEY_free(ecdh);
  }
#endif
#endif

  return ctx;
}

static ssl_ticket_key_block *
ssl_context_enable_tickets(SSL_CTX *ctx, const char *ticket_key_path)
{
#if HAVE_OPENSSL_SESSION_TICKETS
  ats_scoped_str ticket_key_data;
  int ticket_key_len;
  unsigned num_ticket_keys;
  ssl_ticket_key_block *keyblock = NULL;

  if (ticket_key_path != NULL) {
    ticket_key_data = readIntoBuffer(ticket_key_path, __func__, &ticket_key_len);
    if (!ticket_key_data) {
      Error("failed to read SSL session ticket key from %s", (const char *)ticket_key_path);
      goto fail;
    }
  } else {
    // Generate a random ticket key
    ticket_key_len = 48;
    ticket_key_data = (char *)ats_malloc(ticket_key_len);
    char *tmp_ptr = ticket_key_data;
    RAND_bytes(reinterpret_cast<unsigned char *>(tmp_ptr), ticket_key_len);
  }

  num_ticket_keys = ticket_key_len / sizeof(ssl_ticket_key_t);
  if (num_ticket_keys == 0) {
    Error("SSL session ticket key from %s is too short (>= 48 bytes are required)", (const char *)ticket_key_path);
    goto fail;
  }

  // Increase the stats.
  if (ssl_rsb != NULL) { // ssl_rsb is not initialized during the first run.
    SSL_INCREMENT_DYN_STAT(ssl_total_ticket_keys_renewed_stat);
  }

  keyblock = ticket_block_alloc(num_ticket_keys);

  // Slurp all the keys in the ticket key file. We will encrypt with the first key, and decrypt
  // with any key (for rotation purposes).
  for (unsigned i = 0; i < num_ticket_keys; ++i) {
    const char *data = (const char *)ticket_key_data + (i * sizeof(ssl_ticket_key_t));
    memcpy(keyblock->keys[i].key_name, data, sizeof(ssl_ticket_key_t::key_name));
    memcpy(keyblock->keys[i].hmac_secret, data + sizeof(ssl_ticket_key_t::key_name), sizeof(ssl_ticket_key_t::hmac_secret));
    memcpy(keyblock->keys[i].aes_key, data + sizeof(ssl_ticket_key_t::key_name) + sizeof(ssl_ticket_key_t::hmac_secret),
           sizeof(ssl_ticket_key_t::aes_key));
  }

  // Setting the callback can only fail if OpenSSL does not recognize the
  // SSL_CTRL_SET_TLSEXT_TICKET_KEY_CB constant. we set the callback first
  // so that we don't leave a ticket_key pointer attached if it fails.
  if (SSL_CTX_set_tlsext_ticket_key_cb(ctx, ssl_callback_session_ticket) == 0) {
    Error("failed to set session ticket callback");
    goto fail;
  }

  SSL_CTX_clear_options(ctx, SSL_OP_NO_TICKET);
  return keyblock;

fail:
  ticket_block_free(keyblock);
  return NULL;

#else  /* !HAVE_OPENSSL_SESSION_TICKETS */
  (void)ticket_key_path;
  return NULL;
#endif /* HAVE_OPENSSL_SESSION_TICKETS */
}

struct passphrase_cb_userdata {
  const SSLConfigParams *_configParams;
  const char *_serverDialog;
  const char *_serverCert;
  const char *_serverKey;

  passphrase_cb_userdata(const SSLConfigParams *params, const char *dialog, const char *cert, const char *key)
    : _configParams(params), _serverDialog(dialog), _serverCert(cert), _serverKey(key)
  {
  }
};

// RAII implementation for struct termios
struct ssl_termios : public termios {
  ssl_termios(int fd)
  {
    _fd = -1;
    // populate base class data
    if (tcgetattr(fd, this) == 0) { // success
      _fd = fd;
    }
    // save our copy
    _initialAttr = *this;
  }

  ~ssl_termios()
  {
    if (_fd != -1) {
      tcsetattr(_fd, 0, &_initialAttr);
    }
  }

  bool
  ok() const
  {
    return (_fd != -1);
  }

private:
  int _fd;
  struct termios _initialAttr;
};

static int
ssl_getpassword(const char *prompt, char *buffer, int size)
{
  fprintf(stdout, "%s", prompt);

  // disable echo and line buffering
  ssl_termios tty_attr(STDIN_FILENO);

  if (!tty_attr.ok()) {
    return -1;
  }

  tty_attr.c_lflag &= ~ICANON; // no buffer, no backspace
  tty_attr.c_lflag &= ~ECHO;   // no echo
  tty_attr.c_lflag &= ~ISIG;   // no signal for ctrl-c

  if (tcsetattr(STDIN_FILENO, 0, &tty_attr) < 0) {
    return -1;
  }

  int i = 0;
  int ch = 0;

  *buffer = 0;
  while ((ch = getchar()) != '\n' && ch != EOF) {
    // make sure room in buffer
    if (i >= size - 1) {
      return -1;
    }

    buffer[i] = ch;
    buffer[++i] = 0;
  }

  return i;
}

static int
ssl_private_key_passphrase_callback_exec(char *buf, int size, int rwflag, void *userdata)
{
  if (0 == size) {
    return 0;
  }

  *buf = 0;
  passphrase_cb_userdata *ud = static_cast<passphrase_cb_userdata *>(userdata);

  Debug("ssl", "ssl_private_key_passphrase_callback_exec rwflag=%d serverDialog=%s", rwflag, ud->_serverDialog);

  // only respond to reading private keys, not writing them (does ats even do that?)
  if (0 == rwflag) {
    // execute the dialog program and use the first line output as the passphrase
    FILE *f = popen(ud->_serverDialog, "r");
    if (f) {
      if (fgets(buf, size, f)) {
        // remove any ending CR or LF
        for (char *pass = buf; *pass; pass++) {
          if (*pass == '\n' || *pass == '\r') {
            *pass = 0;
            break;
          }
        }
      }
      pclose(f);
    } else { // popen failed
      Error("could not open dialog '%s' - %s", ud->_serverDialog, strerror(errno));
    }
  }
  return strlen(buf);
}

static int
ssl_private_key_passphrase_callback_builtin(char *buf, int size, int rwflag, void *userdata)
{
  if (0 == size) {
    return 0;
  }

  *buf = 0;
  passphrase_cb_userdata *ud = static_cast<passphrase_cb_userdata *>(userdata);

  Debug("ssl", "ssl_private_key_passphrase_callback rwflag=%d serverDialog=%s", rwflag, ud->_serverDialog);

  // only respond to reading private keys, not writing them (does ats even do that?)
  if (0 == rwflag) {
    // output request
    fprintf(stdout, "Some of your private key files are encrypted for security reasons.\n");
    fprintf(stdout, "In order to read them you have to provide the pass phrases.\n");
    fprintf(stdout, "ssl_cert_name=%s", ud->_serverCert);
    if (ud->_serverKey) { // output ssl_key_name if provided
      fprintf(stdout, " ssl_key_name=%s", ud->_serverKey);
    }
    fprintf(stdout, "\n");
    // get passphrase
    // if error, then no passphrase
    if (ssl_getpassword("Enter passphrase:", buf, size) <= 0) {
      *buf = 0;
    }
    fprintf(stdout, "\n");
  }
  return strlen(buf);
}

static bool
ssl_private_key_validate_exec(const char *cmdLine)
{
  if (NULL == cmdLine) {
    errno = EINVAL;
    return false;
  }

  bool bReturn = false;
  char *cmdLineCopy = ats_strdup(cmdLine);
  char *ptr = cmdLineCopy;

  while (*ptr && !isspace(*ptr))
    ++ptr;
  *ptr = 0;
  if (access(cmdLineCopy, X_OK) != -1) {
    bReturn = true;
  }
  ats_free(cmdLineCopy);
  return bReturn;
}

static int
SSLRecRawStatSyncCount(const char *name, RecDataT data_type, RecData *data, RecRawStatBlock *rsb, int id)
{
  // Grab all the stats we want from OpenSSL and set the stats. This function only needs to be called by one of the
  // involved stats, all others *must* call RecRawStatSyncSum.
  SSLCertificateConfig::scoped_config certLookup;

  int64_t sessions = 0;
  int64_t hits = 0;
  int64_t misses = 0;
  int64_t timeouts = 0;

  if (certLookup) {
    const unsigned ctxCount = certLookup->count();
    for (size_t i = 0; i < ctxCount; i++) {
      SSLCertContext *cc = certLookup->get(i);
      if (cc && cc->ctx) {
        sessions += SSL_CTX_sess_accept_good(cc->ctx);
        hits += SSL_CTX_sess_hits(cc->ctx);
        misses += SSL_CTX_sess_misses(cc->ctx);
        timeouts += SSL_CTX_sess_timeouts(cc->ctx);
      }
    }
  }

  SSL_SET_COUNT_DYN_STAT(ssl_user_agent_sessions_stat, sessions);
  SSL_SET_COUNT_DYN_STAT(ssl_user_agent_session_hit_stat, hits);
  SSL_SET_COUNT_DYN_STAT(ssl_user_agent_session_miss_stat, misses);
  SSL_SET_COUNT_DYN_STAT(ssl_user_agent_session_timeout_stat, timeouts);
  return RecRawStatSyncCount(name, data_type, data, rsb, id);
}

void
SSLInitializeLibrary()
{
  if (!open_ssl_initialized) {
    CRYPTO_set_mem_functions(ats_malloc, ats_realloc, ats_free);

    SSL_load_error_strings();
    SSL_library_init();

    mutex_buf = (pthread_mutex_t *)OPENSSL_malloc(CRYPTO_num_locks() * sizeof(pthread_mutex_t));

    for (int i = 0; i < CRYPTO_num_locks(); i++) {
      pthread_mutex_init(&mutex_buf[i], NULL);
    }

    CRYPTO_set_locking_callback(SSL_locking_callback);
    CRYPTO_set_id_callback(SSL_pthreads_thread_id);
  }

#ifdef SSL_CTX_set_tlsext_ticket_key_cb
  ssl_session_ticket_index = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, session_ticket_free);
  if (ssl_session_ticket_index == -1) {
    SSLError("failed to create session ticket index");
  }
#endif

#ifdef HAVE_OPENSSL_OCSP_STAPLING
  ssl_stapling_ex_init();
#endif /* HAVE_OPENSSL_OCSP_STAPLING */

  open_ssl_initialized = true;
}

void
SSLInitializeStatistics()
{
  SSL_CTX *ctx;
  SSL *ssl;
  STACK_OF(SSL_CIPHER) * ciphers;

  // Allocate SSL statistics block.
  ssl_rsb = RecAllocateRawStatBlock((int)Ssl_Stat_Count);
  ink_assert(ssl_rsb != NULL);

  // SSL client errors.
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.user_agent_other_errors", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_user_agent_other_errors_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.user_agent_expired_cert", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_user_agent_expired_cert_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.user_agent_revoked_cert", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_user_agent_revoked_cert_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.user_agent_unknown_cert", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_user_agent_unknown_cert_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.user_agent_cert_verify_failed", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_user_agent_cert_verify_failed_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.user_agent_bad_cert", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_user_agent_bad_cert_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.user_agent_decryption_failed", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_user_agent_decryption_failed_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.user_agent_wrong_version", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_user_agent_wrong_version_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.user_agent_unknown_ca", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_user_agent_unknown_ca_stat, RecRawStatSyncSum);

  // Polled SSL context statistics.
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.user_agent_sessions", RECD_INT, RECP_NON_PERSISTENT,
                     (int)ssl_user_agent_sessions_stat,
                     SSLRecRawStatSyncCount); //<- only use this fn once
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.user_agent_session_hit", RECD_INT, RECP_NON_PERSISTENT,
                     (int)ssl_user_agent_session_hit_stat, RecRawStatSyncCount);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.user_agent_session_miss", RECD_INT, RECP_NON_PERSISTENT,
                     (int)ssl_user_agent_session_miss_stat, RecRawStatSyncCount);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.user_agent_session_timeout", RECD_INT, RECP_NON_PERSISTENT,
                     (int)ssl_user_agent_session_timeout_stat, RecRawStatSyncCount);

  // SSL server errors.
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.origin_server_other_errors", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_origin_server_other_errors_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.origin_server_expired_cert", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_origin_server_expired_cert_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.origin_server_revoked_cert", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_origin_server_revoked_cert_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.origin_server_unknown_cert", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_origin_server_unknown_cert_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.origin_server_cert_verify_failed", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_origin_server_cert_verify_failed_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.origin_server_bad_cert", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_origin_server_bad_cert_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.origin_server_decryption_failed", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_origin_server_decryption_failed_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.origin_server_wrong_version", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_origin_server_wrong_version_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.origin_server_unknown_ca", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_origin_server_unknown_ca_stat, RecRawStatSyncSum);

  // SSL handshake time
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.total_handshake_time", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_total_handshake_time_stat, RecRawStatSyncSum);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.total_success_handshake_count", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_total_success_handshake_count_in_stat, RecRawStatSyncCount);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.total_success_handshake_count_out", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_total_success_handshake_count_out_stat, RecRawStatSyncCount);

  // TLS tickets
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.total_tickets_created", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_total_tickets_created_stat, RecRawStatSyncCount);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.total_tickets_verified", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_total_tickets_verified_stat, RecRawStatSyncCount);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.total_tickets_not_found", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_total_tickets_not_found_stat, RecRawStatSyncCount);
  // TODO: ticket renewal is not used right now.
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.total_tickets_renewed", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_total_tickets_renewed_stat, RecRawStatSyncCount);
  // The number of session tickets verified with an "old" key.
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.total_tickets_verified_old_key", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_total_tickets_verified_old_key_stat, RecRawStatSyncCount);
  // The number of ticket keys renewed.
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.total_ticket_keys_renewed", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_total_ticket_keys_renewed_stat, RecRawStatSyncCount);

  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.ssl_session_cache_hit", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_session_cache_hit, RecRawStatSyncCount);

  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.ssl_session_cache_new_session", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_session_cache_new_session, RecRawStatSyncCount);

  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.ssl_session_cache_miss", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_session_cache_miss, RecRawStatSyncCount);

  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.ssl_session_cache_eviction", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_session_cache_eviction, RecRawStatSyncCount);

  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.ssl_session_cache_lock_contention", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_session_cache_lock_contention, RecRawStatSyncCount);

  /* error stats */
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.ssl_error_want_write", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_error_want_write, RecRawStatSyncCount);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.ssl_error_want_read", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_error_want_read, RecRawStatSyncCount);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.ssl_error_want_x509_lookup", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_error_want_x509_lookup, RecRawStatSyncCount);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.ssl_error_syscall", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_error_syscall, RecRawStatSyncCount);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.ssl_error_read_eos", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_error_read_eos, RecRawStatSyncCount);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.ssl_error_zero_return", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_error_zero_return, RecRawStatSyncCount);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.ssl_error_ssl", RECD_INT, RECP_PERSISTENT, (int)ssl_error_ssl,
                     RecRawStatSyncCount);
  RecRegisterRawStat(ssl_rsb, RECT_PROCESS, "proxy.process.ssl.ssl_sni_name_set_failure", RECD_INT, RECP_PERSISTENT,
                     (int)ssl_sni_name_set_failure, RecRawStatSyncCount);

  // Get and register the SSL cipher stats. Note that we are using the default SSL context to obtain
  // the cipher list. This means that the set of ciphers is fixed by the build configuration and not
  // filtered by proxy.config.ssl.server.cipher_suite. This keeps the set of cipher suites stable across
  // configuration reloads and works for the case where we honor the client cipher preference.

  // initialize stat name->index hash table
  ssl_cipher_name_table = ink_hash_table_create(InkHashTableKeyType_Word);

  ctx = SSLDefaultServerContext();
  ssl = SSL_new(ctx);
  ciphers = SSL_get_ciphers(ssl);

  for (int index = 0; index < sk_SSL_CIPHER_num(ciphers); index++) {
    SSL_CIPHER *cipher = sk_SSL_CIPHER_value(ciphers, index);
    const char *cipherName = SSL_CIPHER_get_name(cipher);
    std::string statName = "proxy.process.ssl.cipher.user_agent." + std::string(cipherName);

    // If room in allocated space ...
    if ((ssl_cipher_stats_start + index) > ssl_cipher_stats_end) {
      // Too many ciphers, increase ssl_cipher_stats_end.
      SSLError("too many ciphers to register metric '%s', increase SSL_Stats::ssl_cipher_stats_end", statName.c_str());
      continue;
    }

    // If not already registered ...
    if (!ink_hash_table_isbound(ssl_cipher_name_table, cipherName)) {
      ink_hash_table_insert(ssl_cipher_name_table, cipherName, (void *)(intptr_t)(ssl_cipher_stats_start + index));
      // Register as non-persistent since the order/index is dependent upon configuration.
      RecRegisterRawStat(ssl_rsb, RECT_PROCESS, statName.c_str(), RECD_INT, RECP_NON_PERSISTENT,
                         (int)ssl_cipher_stats_start + index, RecRawStatSyncSum);
      SSL_CLEAR_DYN_STAT((int)ssl_cipher_stats_start + index);
      Debug("ssl", "registering SSL cipher metric '%s'", statName.c_str());
    }
  }

  SSL_free(ssl);
  SSL_CTX_free(ctx);
}

// return true if we have a stat for the error
static bool
increment_ssl_client_error(unsigned long err)
{
  // we only look for LIB_SSL errors atm
  if (ERR_LIB_SSL != ERR_GET_LIB(err)) {
    SSL_INCREMENT_DYN_STAT(ssl_user_agent_other_errors_stat);
    return false;
  }

  // error was in LIB_SSL, now just switch on REASON
  // (we ignore FUNCTION with the prejudice that we don't care what function
  // the error came from, hope that's ok?)
  switch (ERR_GET_REASON(err)) {
  case SSL_R_SSLV3_ALERT_CERTIFICATE_EXPIRED:
    SSL_INCREMENT_DYN_STAT(ssl_user_agent_expired_cert_stat);
    break;
  case SSL_R_SSLV3_ALERT_CERTIFICATE_REVOKED:
    SSL_INCREMENT_DYN_STAT(ssl_user_agent_revoked_cert_stat);
    break;
  case SSL_R_SSLV3_ALERT_CERTIFICATE_UNKNOWN:
    SSL_INCREMENT_DYN_STAT(ssl_user_agent_unknown_cert_stat);
    break;
  case SSL_R_CERTIFICATE_VERIFY_FAILED:
    SSL_INCREMENT_DYN_STAT(ssl_user_agent_cert_verify_failed_stat);
    break;
  case SSL_R_SSLV3_ALERT_BAD_CERTIFICATE:
    SSL_INCREMENT_DYN_STAT(ssl_user_agent_bad_cert_stat);
    break;
  case SSL_R_TLSV1_ALERT_DECRYPTION_FAILED:
    SSL_INCREMENT_DYN_STAT(ssl_user_agent_decryption_failed_stat);
    break;
  case SSL_R_WRONG_VERSION_NUMBER:
    SSL_INCREMENT_DYN_STAT(ssl_user_agent_wrong_version_stat);
    break;
  case SSL_R_TLSV1_ALERT_UNKNOWN_CA:
    SSL_INCREMENT_DYN_STAT(ssl_user_agent_unknown_ca_stat);
    break;
  default:
    SSL_INCREMENT_DYN_STAT(ssl_user_agent_other_errors_stat);
    return false;
  }

  return true;
}

// return true if we have a stat for the error

static bool
increment_ssl_server_error(unsigned long err)
{
  // we only look for LIB_SSL errors atm
  if (ERR_LIB_SSL != ERR_GET_LIB(err)) {
    SSL_INCREMENT_DYN_STAT(ssl_origin_server_other_errors_stat);
    return false;
  }

  // error was in LIB_SSL, now just switch on REASON
  // (we ignore FUNCTION with the prejudice that we don't care what function
  // the error came from, hope that's ok?)
  switch (ERR_GET_REASON(err)) {
  case SSL_R_SSLV3_ALERT_CERTIFICATE_EXPIRED:
    SSL_INCREMENT_DYN_STAT(ssl_origin_server_expired_cert_stat);
    break;
  case SSL_R_SSLV3_ALERT_CERTIFICATE_REVOKED:
    SSL_INCREMENT_DYN_STAT(ssl_origin_server_revoked_cert_stat);
    break;
  case SSL_R_SSLV3_ALERT_CERTIFICATE_UNKNOWN:
    SSL_INCREMENT_DYN_STAT(ssl_origin_server_unknown_cert_stat);
    break;
  case SSL_R_CERTIFICATE_VERIFY_FAILED:
    SSL_INCREMENT_DYN_STAT(ssl_origin_server_cert_verify_failed_stat);
    break;
  case SSL_R_SSLV3_ALERT_BAD_CERTIFICATE:
    SSL_INCREMENT_DYN_STAT(ssl_origin_server_bad_cert_stat);
    break;
  case SSL_R_TLSV1_ALERT_DECRYPTION_FAILED:
    SSL_INCREMENT_DYN_STAT(ssl_origin_server_decryption_failed_stat);
    break;
  case SSL_R_WRONG_VERSION_NUMBER:
    SSL_INCREMENT_DYN_STAT(ssl_origin_server_wrong_version_stat);
    break;
  case SSL_R_TLSV1_ALERT_UNKNOWN_CA:
    SSL_INCREMENT_DYN_STAT(ssl_origin_server_unknown_ca_stat);
    break;
  default:
    SSL_INCREMENT_DYN_STAT(ssl_origin_server_other_errors_stat);
    return false;
  }

  return true;
}

void
SSLDiagnostic(const SrcLoc &loc, bool debug, SSLNetVConnection *vc, const char *fmt, ...)
{
  unsigned long l;
  char buf[256];
  const char *file, *data;
  int line, flags;
  unsigned long es;
  va_list ap;
  ip_text_buffer ip_buf = {'\0'};

  if (vc) {
    ats_ip_ntop(vc->get_remote_addr(), ip_buf, sizeof(ip_buf));
  }

  es = CRYPTO_thread_id();
  while ((l = ERR_get_error_line_data(&file, &line, &data, &flags)) != 0) {
    if (debug) {
      if (unlikely(diags->on())) {
        diags->log("ssl", DL_Debug, loc.file, loc.func, loc.line, "SSL::%lu:%s:%s:%d%s%s%s%s", es, ERR_error_string(l, buf), file,
                   line, (flags & ERR_TXT_STRING) ? ":" : "", (flags & ERR_TXT_STRING) ? data : "", vc ? ": peer address is " : "",
                   ip_buf);
      }
    } else {
      diags->error(DL_Error, loc.file, loc.func, loc.line, "SSL::%lu:%s:%s:%d%s%s%s%s", es, ERR_error_string(l, buf), file, line,
                   (flags & ERR_TXT_STRING) ? ":" : "", (flags & ERR_TXT_STRING) ? data : "", vc ? ": peer address is " : "",
                   ip_buf);
    }

    // Tally desired stats (only client/server connection stats, not init
    // issues where vc is NULL)
    if (vc) {
      // getSSLClientConnection - true if ats is client (we update server stats)
      if (vc->getSSLClientConnection()) {
        increment_ssl_server_error(l); // update server error stats
      } else {
        increment_ssl_client_error(l); // update client error stat
      }
    }
  }

  va_start(ap, fmt);
  if (debug) {
    diags->log_va("ssl", DL_Debug, &loc, fmt, ap);
  } else {
    diags->error_va(DL_Error, loc.file, loc.func, loc.line, fmt, ap);
  }
  va_end(ap);
}

const char *
SSLErrorName(int ssl_error)
{
  static const char *names[] = {"SSL_ERROR_NONE", "SSL_ERROR_SSL", "SSL_ERROR_WANT_READ", "SSL_ERROR_WANT_WRITE",
                                "SSL_ERROR_WANT_X509_LOOKUP", "SSL_ERROR_SYSCALL", "SSL_ERROR_ZERO_RETURN",
                                "SSL_ERROR_WANT_CONNECT", "SSL_ERROR_WANT_ACCEPT"};

  if (ssl_error < 0 || ssl_error >= (int)countof(names)) {
    return "unknown SSL error";
  }

  return names[ssl_error];
}

void
SSLDebugBufferPrint(const char *tag, const char *buffer, unsigned buflen, const char *message)
{
  if (is_debug_tag_set(tag)) {
    if (message != NULL) {
      fprintf(stdout, "%s\n", message);
    }
    for (unsigned ii = 0; ii < buflen; ii++) {
      putc(buffer[ii], stdout);
    }
    putc('\n', stdout);
  }
}

SSL_CTX *
SSLDefaultServerContext()
{
  ink_ssl_method_t meth = NULL;

  meth = SSLv23_server_method();
  return SSL_CTX_new(meth);
}

static bool
SSLPrivateKeyHandler(SSL_CTX *ctx, const SSLConfigParams *params, const ats_scoped_str &completeServerCertPath, const char *keyPath)
{
  if (!keyPath) {
    // assume private key is contained in cert obtained from multicert file.
    if (!SSL_CTX_use_PrivateKey_file(ctx, completeServerCertPath, SSL_FILETYPE_PEM)) {
      SSLError("failed to load server private key from %s", (const char *)completeServerCertPath);
      return false;
    }
  } else if (params->serverKeyPathOnly != NULL) {
    ats_scoped_str completeServerKeyPath(Layout::get()->relative_to(params->serverKeyPathOnly, keyPath));
    if (!SSL_CTX_use_PrivateKey_file(ctx, completeServerKeyPath, SSL_FILETYPE_PEM)) {
      SSLError("failed to load server private key from %s", (const char *)completeServerKeyPath);
      return false;
    }
  } else {
    SSLError("empty SSL private key path in records.config");
    return false;
  }

  if (!SSL_CTX_check_private_key(ctx)) {
    SSLError("server private key does not match the certificate public key");
    return false;
  }

  return true;
}

SSL_CTX *
SSLInitServerContext(const SSLConfigParams *params, const ssl_user_config &sslMultCertSettings)
{
  int server_verify_client;
  ats_scoped_str completeServerCertPath;
  SSL_CTX *ctx = SSLDefaultServerContext();
  EVP_MD_CTX digest;
  STACK_OF(X509_NAME) * ca_list;
  unsigned char hash_buf[EVP_MAX_MD_SIZE];
  unsigned int hash_len = 0;
  char const *setting_cert = sslMultCertSettings.cert.get();

  // disable selected protocols
  SSL_CTX_set_options(ctx, params->ssl_ctx_options);

  Debug("ssl.session_cache", "ssl context=%p: using session cache options, enabled=%d, size=%d, num_buckets=%d, "
                             "skip_on_contention=%d, timeout=%d, auto_clear=%d",
        ctx, params->ssl_session_cache, params->ssl_session_cache_size, params->ssl_session_cache_num_buckets,
        params->ssl_session_cache_skip_on_contention, params->ssl_session_cache_timeout, params->ssl_session_cache_auto_clear);

  if (params->ssl_session_cache_timeout) {
    SSL_CTX_set_timeout(ctx, params->ssl_session_cache_timeout);
  }

  int additional_cache_flags = 0;
  additional_cache_flags |= (params->ssl_session_cache_auto_clear == 0) ? SSL_SESS_CACHE_NO_AUTO_CLEAR : 0;

  switch (params->ssl_session_cache) {
  case SSLConfigParams::SSL_SESSION_CACHE_MODE_OFF:
    Debug("ssl.session_cache", "disabling SSL session cache");

    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF | SSL_SESS_CACHE_NO_INTERNAL);
    break;
  case SSLConfigParams::SSL_SESSION_CACHE_MODE_SERVER_OPENSSL_IMPL:
    Debug("ssl.session_cache", "enabling SSL session cache with OpenSSL implementation");

    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER | additional_cache_flags);
    SSL_CTX_sess_set_cache_size(ctx, params->ssl_session_cache_size);
    break;
  case SSLConfigParams::SSL_SESSION_CACHE_MODE_SERVER_ATS_IMPL: {
    Debug("ssl.session_cache", "enabling SSL session cache with ATS implementation");
    /* Add all the OpenSSL callbacks */
    SSL_CTX_sess_set_new_cb(ctx, ssl_new_cached_session);
    SSL_CTX_sess_set_remove_cb(ctx, ssl_rm_cached_session);
    SSL_CTX_sess_set_get_cb(ctx, ssl_get_cached_session);

    SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_SERVER | SSL_SESS_CACHE_NO_INTERNAL | additional_cache_flags);

    break;
  }
  }

#ifdef SSL_MODE_RELEASE_BUFFERS
  if (OPENSSL_VERSION_NUMBER > 0x1000107fL) {
    Debug("ssl", "enabling SSL_MODE_RELEASE_BUFFERS");
    SSL_CTX_set_mode(ctx, SSL_MODE_RELEASE_BUFFERS);
  }
#endif

#ifdef SSL_OP_SAFARI_ECDHE_ECDSA_BUG
  SSL_CTX_set_options(ctx, SSL_OP_SAFARI_ECDHE_ECDSA_BUG);
#endif

  // pass phrase dialog configuration
  passphrase_cb_userdata ud(params, sslMultCertSettings.dialog, sslMultCertSettings.first_cert, sslMultCertSettings.key);

  if (sslMultCertSettings.dialog) {
    pem_password_cb *passwd_cb = NULL;
    if (strncmp(sslMultCertSettings.dialog, "exec:", 5) == 0) {
      ud._serverDialog = &sslMultCertSettings.dialog[5];
      // validate the exec program
      if (!ssl_private_key_validate_exec(ud._serverDialog)) {
        SSLError("failed to access '%s' pass phrase program: %s", (const char *)ud._serverDialog, strerror(errno));
        goto fail;
      }
      passwd_cb = ssl_private_key_passphrase_callback_exec;
    } else if (strcmp(sslMultCertSettings.dialog, "builtin") == 0) {
      passwd_cb = ssl_private_key_passphrase_callback_builtin;
    } else { // unknown config
      SSLError("unknown " SSL_KEY_DIALOG " configuration value '%s'", (const char *)sslMultCertSettings.dialog);
      goto fail;
    }
    SSL_CTX_set_default_passwd_cb(ctx, passwd_cb);
    SSL_CTX_set_default_passwd_cb_userdata(ctx, &ud);
  }

  if (sslMultCertSettings.cert) {
    SimpleTokenizer cert_tok((const char *)sslMultCertSettings.cert, SSL_CERT_SEPARATE_DELIM);
    SimpleTokenizer key_tok((sslMultCertSettings.key ? (const char *)sslMultCertSettings.key : ""), SSL_CERT_SEPARATE_DELIM);

    if (sslMultCertSettings.key && cert_tok.getNumTokensRemaining() != key_tok.getNumTokensRemaining()) {
      Error("the number of certificates in ssl_cert_name and ssl_key_name doesn't match");
      goto fail;
    }

    for (const char *certname = cert_tok.getNext(); certname; certname = cert_tok.getNext()) {
      completeServerCertPath = Layout::relative_to(params->serverCertPathOnly, certname);
      if (SSL_CTX_use_certificate_chain_file(ctx, completeServerCertPath) <= 0) {
        SSLError("failed to load certificate chain from %s", (const char *)completeServerCertPath);
        goto fail;
      }

      const char *keyPath = key_tok.getNext();
      if (!SSLPrivateKeyHandler(ctx, params, completeServerCertPath, keyPath)) {
        goto fail;
      }
    }

    // First, load any CA chains from the global chain file.
    if (params->serverCertChainFilename) {
      ats_scoped_str completeServerCertChainPath(Layout::relative_to(params->serverCertPathOnly, params->serverCertChainFilename));
      if (!SSL_CTX_add_extra_chain_cert_file(ctx, completeServerCertChainPath)) {
        SSLError("failed to load global certificate chain from %s", (const char *)completeServerCertChainPath);
        goto fail;
      }
    }

    // Now, load any additional certificate chains specified in this entry.
    if (sslMultCertSettings.ca) {
      ats_scoped_str completeServerCertChainPath(Layout::relative_to(params->serverCertPathOnly, sslMultCertSettings.ca));
      if (!SSL_CTX_add_extra_chain_cert_file(ctx, completeServerCertChainPath)) {
        SSLError("failed to load certificate chain from %s", (const char *)completeServerCertChainPath);
        goto fail;
      }
    }
  }

  // SSL_CTX_load_verify_locations() builds the cert chain from the
  // serverCACertFilename if that is not NULL.  Otherwise, it uses the hashed
  // symlinks in serverCACertPath.
  //
  // if ssl_ca_name is NOT configured for this cert in ssl_multicert.config
  //     AND
  // if proxy.config.ssl.CA.cert.filename and proxy.config.ssl.CA.cert.path
  //     are configured
  //   pass that file as the chain (include all certs in that file)
  // else if proxy.config.ssl.CA.cert.path is configured (and
  //       proxy.config.ssl.CA.cert.filename is NULL)
  //   use the hashed symlinks in that directory to build the chain
  if (!sslMultCertSettings.ca && params->serverCACertPath != NULL) {
    if ((!SSL_CTX_load_verify_locations(ctx, params->serverCACertFilename, params->serverCACertPath)) ||
        (!SSL_CTX_set_default_verify_paths(ctx))) {
      SSLError("invalid CA Certificate file or CA Certificate path");
      goto fail;
    }
  }

  if (params->clientCertLevel != 0) {
    if (params->serverCACertFilename != NULL && params->serverCACertPath != NULL) {
      if ((!SSL_CTX_load_verify_locations(ctx, params->serverCACertFilename, params->serverCACertPath)) ||
          (!SSL_CTX_set_default_verify_paths(ctx))) {
        SSLError("CA Certificate file or CA Certificate path invalid");
        goto fail;
      }
    }

    if (params->clientCertLevel == 2) {
      server_verify_client = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT | SSL_VERIFY_CLIENT_ONCE;
    } else if (params->clientCertLevel == 1) {
      server_verify_client = SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE;
    } else {
      // disable client cert support
      server_verify_client = SSL_VERIFY_NONE;
      Error("illegal client certification level %d in records.config", server_verify_client);
    }
    SSL_CTX_set_verify(ctx, server_verify_client, NULL);
    SSL_CTX_set_verify_depth(ctx, params->verify_depth); // might want to make configurable at some point.
  }

  ca_list = SSL_load_client_CA_file(params->serverCACertFilename);
  SSL_CTX_set_client_CA_list(ctx, ca_list);
  EVP_MD_CTX_init(&digest);

  if (EVP_DigestInit_ex(&digest, evp_md_func, NULL) == 0) {
    SSLError("EVP_DigestInit_ex failed");
    goto fail;
  }

  Debug("ssl", "Using '%s' in hash for session id context", sslMultCertSettings.cert.get());

  if (NULL != setting_cert) {
    if (EVP_DigestUpdate(&digest, sslMultCertSettings.cert, strlen(setting_cert)) == 0) {
      SSLError("EVP_DigestUpdate failed");
      goto fail;
    }
  }

  if (ca_list != NULL) {
    size_t num_certs = sk_X509_NAME_num(ca_list);

    for (size_t i = 0; i < num_certs; i++) {
      X509_NAME *name = sk_X509_NAME_value(ca_list, i);
      if (X509_NAME_digest(name, evp_md_func, hash_buf /* borrow our final hash buffer. */, &hash_len) == 0 ||
          EVP_DigestUpdate(&digest, hash_buf, hash_len) == 0) {
        SSLError("Adding X509 name to digest failed");
        goto fail;
      }
    }
  }

  if (EVP_DigestFinal_ex(&digest, hash_buf, &hash_len) == 0) {
    SSLError("EVP_DigestFinal_ex failed");
    goto fail;
  }

  EVP_MD_CTX_cleanup(&digest);
  if (SSL_CTX_set_session_id_context(ctx, hash_buf, hash_len) == 0) {
    SSLError("SSL_CTX_set_session_id_context failed");
    goto fail;
  }

  if (params->cipherSuite != NULL) {
    if (!SSL_CTX_set_cipher_list(ctx, params->cipherSuite)) {
      SSLError("invalid cipher suite in records.config");
      goto fail;
    }
  }
#define SSL_CLEAR_PW_REFERENCES(UD, CTX)               \
  {                                                    \
    memset(static_cast<void *>(&UD), 0, sizeof(UD));   \
    SSL_CTX_set_default_passwd_cb(CTX, NULL);          \
    SSL_CTX_set_default_passwd_cb_userdata(CTX, NULL); \
  }
  SSL_CLEAR_PW_REFERENCES(ud, ctx)
  if (params->dhparamsFile != NULL && !ssl_context_enable_dhe(params->dhparamsFile, ctx)) {
    goto fail;
  }
  return ssl_context_enable_ecdh(ctx);

fail:
  SSL_CLEAR_PW_REFERENCES(ud, ctx)
  SSL_CTX_free(ctx);

  return NULL;
}

SSL_CTX *
SSLInitClientContext(const SSLConfigParams *params)
{
  ink_ssl_method_t meth = NULL;
  SSL_CTX *client_ctx = NULL;
  char *clientKeyPtr = NULL;

  // Note that we do not call RAND_seed() explicitly here, we depend on OpenSSL
  // to do the seeding of the PRNG for us. This is the case for all platforms that
  // has /dev/urandom for example.

  meth = SSLv23_client_method();
  client_ctx = SSL_CTX_new(meth);

  // disable selected protocols
  SSL_CTX_set_options(client_ctx, params->ssl_ctx_options);
  if (!client_ctx) {
    SSLError("cannot create new client context");
    _exit(1);
  }

  if (params->ssl_client_ctx_protocols) {
    SSL_CTX_set_options(client_ctx, params->ssl_client_ctx_protocols);
  }
  if (params->client_cipherSuite != NULL) {
    if (!SSL_CTX_set_cipher_list(client_ctx, params->client_cipherSuite)) {
      SSLError("invalid client cipher suite in records.config");
      goto fail;
    }
  }

  // if no path is given for the client private key,
  // assume it is contained in the client certificate file.
  clientKeyPtr = params->clientKeyPath;
  if (clientKeyPtr == NULL) {
    clientKeyPtr = params->clientCertPath;
  }

  if (params->clientCertPath != 0) {
    if (!SSL_CTX_use_certificate_chain_file(client_ctx, params->clientCertPath)) {
      SSLError("failed to load client certificate from %s", params->clientCertPath);
      goto fail;
    }

    if (!SSL_CTX_use_PrivateKey_file(client_ctx, clientKeyPtr, SSL_FILETYPE_PEM)) {
      SSLError("failed to load client private key file from %s", clientKeyPtr);
      goto fail;
    }

    if (!SSL_CTX_check_private_key(client_ctx)) {
      SSLError("client private key (%s) does not match the certificate public key (%s)", clientKeyPtr, params->clientCertPath);
      goto fail;
    }
  }

  if (params->clientVerify) {
    int client_verify_server;

    client_verify_server = params->clientVerify ? SSL_VERIFY_PEER : SSL_VERIFY_NONE;
    SSL_CTX_set_verify(client_ctx, client_verify_server, NULL);
    SSL_CTX_set_verify_depth(client_ctx, params->client_verify_depth);

    if (params->clientCACertFilename != NULL && params->clientCACertPath != NULL) {
      if (!SSL_CTX_load_verify_locations(client_ctx, params->clientCACertFilename, params->clientCACertPath)) {
        SSLError("invalid client CA Certificate file (%s) or CA Certificate path (%s)", params->clientCACertFilename,
                 params->clientCACertPath);
        goto fail;
      }
    }

    if (!SSL_CTX_set_default_verify_paths(client_ctx)) {
      SSLError("failed to set the default verify paths");
      goto fail;
    }
  }

  if (SSLConfigParams::init_ssl_ctx_cb) {
    SSLConfigParams::init_ssl_ctx_cb(client_ctx, false);
  }

  return client_ctx;

fail:
  SSL_CTX_free(client_ctx);
  _exit(1);
}

static char *
asn1_strdup(ASN1_STRING *s)
{
  // Make sure we have an 8-bit encoding.
  ink_assert(ASN1_STRING_type(s) == V_ASN1_IA5STRING || ASN1_STRING_type(s) == V_ASN1_UTF8STRING ||
             ASN1_STRING_type(s) == V_ASN1_PRINTABLESTRING || ASN1_STRING_type(s) == V_ASN1_T61STRING);

  return ats_strndup((const char *)ASN1_STRING_data(s), ASN1_STRING_length(s));
}

// Given a certificate and it's corresponding SSL_CTX context, insert hash
// table aliases for subject CN and subjectAltNames DNS without wildcard,
// insert trie aliases for those with wildcard.
static bool
ssl_index_certificate(SSLCertLookup *lookup, SSLCertContext const &cc, const char *certfile)
{
  X509_NAME *subject = NULL;
  X509 *cert;
  scoped_BIO bio(BIO_new_file(certfile, "r"));
  bool inserted = false;

  cert = PEM_read_bio_X509_AUX(bio.get(), NULL, NULL, NULL);
  if (NULL == cert) {
    Error("Failed to load certificate from file %s", certfile);
    lookup->is_valid = false;
    return false;
  }

  // Insert a key for the subject CN.
  subject = X509_get_subject_name(cert);
  ats_scoped_str subj_name;
  if (subject) {
    int pos = -1;
    for (;;) {
      pos = X509_NAME_get_index_by_NID(subject, NID_commonName, pos);
      if (pos == -1) {
        break;
      }

      X509_NAME_ENTRY *e = X509_NAME_get_entry(subject, pos);
      ASN1_STRING *cn = X509_NAME_ENTRY_get_data(e);
      subj_name = asn1_strdup(cn);

      Debug("ssl", "mapping '%s' to certificate %s", (const char *)subj_name, certfile);
      if (lookup->insert(subj_name, cc) >= 0)
        inserted = true;
    }
  }

#if HAVE_OPENSSL_TS_H
  // Traverse the subjectAltNames (if any) and insert additional keys for the SSL context.
  GENERAL_NAMES *names = (GENERAL_NAMES *)X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
  if (names) {
    unsigned count = sk_GENERAL_NAME_num(names);
    for (unsigned i = 0; i < count; ++i) {
      GENERAL_NAME *name;

      name = sk_GENERAL_NAME_value(names, i);
      if (name->type == GEN_DNS) {
        ats_scoped_str dns(asn1_strdup(name->d.dNSName));
        // only try to insert if the alternate name is not the main name
        if (strcmp(dns, subj_name) != 0) {
          Debug("ssl", "mapping '%s' to certificate %s", (const char *)dns, certfile);
          if (lookup->insert(dns, cc) >= 0)
            inserted = true;
        }
      }
    }

    GENERAL_NAMES_free(names);
  }
#endif // HAVE_OPENSSL_TS_H
  X509_free(cert);
  return inserted;
}

// This callback function is executed while OpenSSL processes the SSL
// handshake and does SSL record layer stuff.  It's used to trap
// client-initiated renegotiations and update cipher stats
static void
ssl_callback_info(const SSL *ssl, int where, int ret)
{
  Debug("ssl", "ssl_callback_info ssl: %p where: %d ret: %d", ssl, where, ret);
  SSLNetVConnection *netvc = (SSLNetVConnection *)SSL_get_app_data(ssl);

  if ((where & SSL_CB_ACCEPT_LOOP) && netvc->getSSLHandShakeComplete() == true &&
      SSLConfigParams::ssl_allow_client_renegotiation == false) {
    int state = SSL_get_state(ssl);

    if (state == SSL3_ST_SR_CLNT_HELLO_A || state == SSL23_ST_SR_CLNT_HELLO_A) {
      netvc->setSSLClientRenegotiationAbort(true);
      Debug("ssl", "ssl_callback_info trying to renegotiate from the client");
    }
  }
  if (where & SSL_CB_HANDSHAKE_DONE) {
    // handshake is complete
    const SSL_CIPHER *cipher = SSL_get_current_cipher(ssl);
    if (cipher) {
      const char *cipherName = SSL_CIPHER_get_name(cipher);
      // lookup index of stat by name and incr count
      InkHashTableValue data;
      if (ink_hash_table_lookup(ssl_cipher_name_table, cipherName, &data)) {
        SSL_INCREMENT_DYN_STAT((intptr_t)data);
      }
    }
  }
}

static void
ssl_set_handshake_callbacks(SSL_CTX *ctx)
{
#if TS_USE_TLS_SNI
// Make sure the callbacks are set
#if TS_USE_CERT_CB
  SSL_CTX_set_cert_cb(ctx, ssl_cert_callback, NULL);
#else
  SSL_CTX_set_tlsext_servername_callback(ctx, ssl_servername_callback);
#endif
#endif
}

static SSL_CTX *
ssl_store_ssl_context(const SSLConfigParams *params, SSLCertLookup *lookup, const ssl_user_config &sslMultCertSettings)
{
  SSL_CTX *ctx = SSLInitServerContext(params, sslMultCertSettings);
  ats_scoped_str certpath;
  ats_scoped_str session_key_path;
  ssl_ticket_key_block *keyblock = NULL;
  bool inserted = false;

  if (!ctx) {
    lookup->is_valid = false;
    return ctx;
  }

  // The certificate callbacks are set by the caller only
  // for the default certificate

  SSL_CTX_set_info_callback(ctx, ssl_callback_info);

#if TS_USE_TLS_NPN
  SSL_CTX_set_next_protos_advertised_cb(ctx, SSLNetVConnection::advertise_next_protocol, NULL);
#endif /* TS_USE_TLS_NPN */

#if TS_USE_TLS_ALPN
  SSL_CTX_set_alpn_select_cb(ctx, SSLNetVConnection::select_next_protocol, NULL);
#endif /* TS_USE_TLS_ALPN */
  if (sslMultCertSettings.first_cert) {
    certpath = Layout::relative_to(params->serverCertPathOnly, sslMultCertSettings.first_cert);
  } else {
    certpath = NULL;
  }

  // Load the session ticket key if session tickets are not disabled and we have key name.
  if (sslMultCertSettings.session_ticket_enabled != 0 && sslMultCertSettings.ticket_key_filename) {
    ats_scoped_str ticket_key_path(Layout::relative_to(params->serverCertPathOnly, sslMultCertSettings.ticket_key_filename));
    keyblock = ssl_context_enable_tickets(ctx, ticket_key_path);
  } else if (sslMultCertSettings.session_ticket_enabled != 0) {
    keyblock = ssl_context_enable_tickets(ctx, NULL);
  }


  // Index this certificate by the specified IP(v6) address. If the address is "*", make it the default context.
  if (sslMultCertSettings.addr) {
    if (strcmp(sslMultCertSettings.addr, "*") == 0) {
      if (lookup->insert(sslMultCertSettings.addr, SSLCertContext(ctx, sslMultCertSettings.opt, keyblock)) >= 0) {
        inserted = true;
        lookup->ssl_default = ctx;
        ssl_set_handshake_callbacks(ctx);
      }
    } else {
      IpEndpoint ep;

      if (ats_ip_pton(sslMultCertSettings.addr, &ep) == 0) {
        Debug("ssl", "mapping '%s' to certificate %s", (const char *)sslMultCertSettings.addr, (const char *)certpath);
        if (certpath != NULL && lookup->insert(ep, SSLCertContext(ctx, sslMultCertSettings.opt, keyblock)) >= 0) {
          inserted = true;
        }
      } else {
        Error("'%s' is not a valid IPv4 or IPv6 address", (const char *)sslMultCertSettings.addr);
        lookup->is_valid = false;
      }
    }
  }
  if (!inserted) {
#if HAVE_OPENSSL_SESSION_TICKETS
    if (keyblock != NULL) {
      ticket_block_free(keyblock);
    }
#endif
  }


#if defined(SSL_OP_NO_TICKET)
  // Session tickets are enabled by default. Disable if explicitly requested.
  if (sslMultCertSettings.session_ticket_enabled == 0) {
    SSL_CTX_set_options(ctx, SSL_OP_NO_TICKET);
    Debug("ssl", "ssl session ticket is disabled");
  }
#endif

#ifdef HAVE_OPENSSL_OCSP_STAPLING
  if (SSLConfigParams::ssl_ocsp_enabled) {
    Debug("ssl", "ssl ocsp stapling is enabled");
    SSL_CTX_set_tlsext_status_cb(ctx, ssl_callback_ocsp_stapling);
    if (!ssl_stapling_init_cert(ctx, (const char *)certpath)) {
      Warning("fail to configure SSL_CTX for OCSP Stapling info for certificate at %s", (const char *)certpath);
    }
  } else {
    Debug("ssl", "ssl ocsp stapling is disabled");
  }
#else
  if (SSLConfigParams::ssl_ocsp_enabled) {
    Warning("fail to enable ssl ocsp stapling, this openssl version does not support it");
  }
#endif /* HAVE_OPENSSL_OCSP_STAPLING */

  // Insert additional mappings. Note that this maps multiple keys to the same value, so when
  // this code is updated to reconfigure the SSL certificates, it will need some sort of
  // refcounting or alternate way of avoiding double frees.
  Debug("ssl", "importing SNI names from %s", (const char *)certpath);
  if (certpath != NULL && ssl_index_certificate(lookup, SSLCertContext(ctx, sslMultCertSettings.opt), certpath)) {
    inserted = true;
  }

  if (inserted) {
    if (SSLConfigParams::init_ssl_ctx_cb) {
      SSLConfigParams::init_ssl_ctx_cb(ctx, true);
    }
  }
  if (!inserted) {
    if (ctx != NULL) {
      SSL_CTX_free(ctx);
      ctx = NULL;
    }
  }
  return ctx;
}

static bool
ssl_extract_certificate(const matcher_line *line_info, ssl_user_config &sslMultCertSettings)
{
  for (int i = 0; i < MATCHER_MAX_TOKENS; ++i) {
    const char *label;
    const char *value;

    label = line_info->line[0][i];
    value = line_info->line[1][i];

    if (label == NULL) {
      continue;
    }

    if (strcasecmp(label, SSL_IP_TAG) == 0) {
      sslMultCertSettings.addr = ats_strdup(value);
    }

    if (strcasecmp(label, SSL_CERT_TAG) == 0) {
      sslMultCertSettings.cert = ats_strdup(value);
    }

    if (strcasecmp(label, SSL_CA_TAG) == 0) {
      sslMultCertSettings.ca = ats_strdup(value);
    }

    if (strcasecmp(label, SSL_PRIVATE_KEY_TAG) == 0) {
      sslMultCertSettings.key = ats_strdup(value);
    }

    if (strcasecmp(label, SSL_SESSION_TICKET_ENABLED) == 0) {
      sslMultCertSettings.session_ticket_enabled = atoi(value);
    }

    if (strcasecmp(label, SSL_SESSION_TICKET_KEY_FILE_TAG) == 0) {
      sslMultCertSettings.ticket_key_filename = ats_strdup(value);
    }

    if (strcasecmp(label, SSL_KEY_DIALOG) == 0) {
      sslMultCertSettings.dialog = ats_strdup(value);
    }
    if (strcasecmp(label, SSL_ACTION_TAG) == 0) {
      if (strcasecmp(SSL_ACTION_TUNNEL_TAG, value) == 0) {
        sslMultCertSettings.opt = SSLCertContext::OPT_TUNNEL;
      } else {
        Error("Unrecognized action for " SSL_ACTION_TAG);
        return false;
      }
    }
  }
  if (!sslMultCertSettings.cert) {
    Warning("missing %s tag", SSL_CERT_TAG);
    return false;
  } else {
    SimpleTokenizer cert_tok(sslMultCertSettings.cert, SSL_CERT_SEPARATE_DELIM);
    const char *first_cert = cert_tok.getNext();
    if (first_cert) {
      sslMultCertSettings.first_cert = ats_strdup(first_cert);
    }
  }

  return true;
}

bool
SSLParseCertificateConfiguration(const SSLConfigParams *params, SSLCertLookup *lookup)
{
  char *tok_state = NULL;
  char *line = NULL;
  ats_scoped_str file_buf;
  unsigned line_num = 0;
  matcher_line line_info;

  const matcher_tags sslCertTags = {NULL, NULL, NULL, NULL, NULL, NULL, false};

  Note("loading SSL certificate configuration from %s", params->configFilePath);

  if (params->configFilePath) {
    file_buf = readIntoBuffer(params->configFilePath, __func__, NULL);
  }

  if (!file_buf) {
    Error("failed to read SSL certificate configuration from %s", params->configFilePath);
    return false;
  }

#if TS_USE_POSIX_CAP
  // elevate/allow file access to root read only files/certs
  uint32_t elevate_setting = 0;
  REC_ReadConfigInteger(elevate_setting, "proxy.config.ssl.cert.load_elevated");
  ElevateAccess elevate_access(elevate_setting != 0); // destructor will demote for us
#endif                                                /* TS_USE_POSIX_CAP */

  line = tokLine(file_buf, &tok_state);
  while (line != NULL) {
    line_num++;

    // skip all blank spaces at beginning of line
    while (*line && isspace(*line)) {
      line++;
    }

    if (*line != '\0' && *line != '#') {
      ssl_user_config sslMultiCertSettings;
      const char *errPtr;

      errPtr = parseConfigLine(line, &line_info, &sslCertTags);

      if (errPtr != NULL) {
        RecSignalWarning(REC_SIGNAL_CONFIG_ERROR, "%s: discarding %s entry at line %d: %s", __func__, params->configFilePath,
                         line_num, errPtr);
      } else {
        if (ssl_extract_certificate(&line_info, sslMultiCertSettings)) {
          ssl_store_ssl_context(params, lookup, sslMultiCertSettings);
        }
      }
    }

    line = tokLine(NULL, &tok_state);
  }

  // We *must* have a default context even if it can't possibly work. The default context is used to
  // bootstrap the SSL handshake so that we can subsequently do the SNI lookup to switch to the real
  // context.
  if (lookup->ssl_default == NULL) {
    ssl_user_config sslMultiCertSettings;
    sslMultiCertSettings.addr = ats_strdup("*");
    if (ssl_store_ssl_context(params, lookup, sslMultiCertSettings) == NULL) {
      Error("failed set default context");
      return false;
    }
  }
  return true;
}

#if HAVE_OPENSSL_SESSION_TICKETS

static void
session_ticket_free(void * /*parent*/, void *ptr, CRYPTO_EX_DATA * /*ad*/, int /*idx*/, long /*argl*/, void * /*argp*/)
{
  ticket_block_free((struct ssl_ticket_key_block *)ptr);
}

/*
 * RFC 5077. Create session ticket to resume SSL session without requiring session-specific state at the TLS server.
 * Specifically, it distributes the encrypted session-state information to the client in the form of a ticket and
 * a mechanism to present the ticket back to the server.
 * */
static int
ssl_callback_session_ticket(SSL *ssl, unsigned char *keyname, unsigned char *iv, EVP_CIPHER_CTX *cipher_ctx, HMAC_CTX *hctx,
                            int enc)
{
  SSLCertificateConfig::scoped_config lookup;
  SSLNetVConnection *netvc = (SSLNetVConnection *)SSL_get_app_data(ssl);

  // Get the IP address to look up the keyblock
  IpEndpoint ip;
  int namelen = sizeof(ip);
  safe_getsockname(netvc->get_socket(), &ip.sa, &namelen);
  SSLCertContext *cc = lookup->find(ip);
  if (cc == NULL || cc->keyblock == NULL) {
    // Try the default
    cc = lookup->find("*");
  }
  if (cc == NULL || cc->keyblock == NULL) {
    // No, key specified.  Must fail out at this point.
    // Alternatively we could generate a random key

    return -1;
  }
  ssl_ticket_key_block *keyblock = cc->keyblock;

  ink_release_assert(keyblock != NULL && keyblock->num_keys > 0);

  if (enc == 1) {
    const ssl_ticket_key_t &most_recent_key = keyblock->keys[0];
    memcpy(keyname, most_recent_key.key_name, sizeof(ssl_ticket_key_t::key_name));
    RAND_pseudo_bytes(iv, EVP_MAX_IV_LENGTH);
    EVP_EncryptInit_ex(cipher_ctx, EVP_aes_128_cbc(), NULL, most_recent_key.aes_key, iv);
    HMAC_Init_ex(hctx, most_recent_key.hmac_secret, sizeof(ssl_ticket_key_t::hmac_secret), evp_md_func, NULL);

    Debug("ssl", "create ticket for a new session.");
    SSL_INCREMENT_DYN_STAT(ssl_total_tickets_created_stat);
    return 0;
  } else if (enc == 0) {
    for (unsigned i = 0; i < keyblock->num_keys; ++i) {
      if (memcmp(keyname, keyblock->keys[i].key_name, sizeof(ssl_ticket_key_t::key_name)) == 0) {
        EVP_DecryptInit_ex(cipher_ctx, EVP_aes_128_cbc(), NULL, keyblock->keys[i].aes_key, iv);
        HMAC_Init_ex(hctx, keyblock->keys[i].hmac_secret, sizeof(ssl_ticket_key_t::hmac_secret), evp_md_func, NULL);

        Debug("ssl", "verify the ticket for an existing session.");
        // Increase the total number of decrypted tickets.
        SSL_INCREMENT_DYN_STAT(ssl_total_tickets_verified_stat);

        if (i != 0) // The number of tickets decrypted with "older" keys.
          SSL_INCREMENT_DYN_STAT(ssl_total_tickets_verified_old_key_stat);

        // When we decrypt with an "older" key, encrypt the ticket again with the most recent key.
        return (i == 0) ? 1 : 2;
      }
    }

    Debug("ssl", "keyname is not consistent.");
    SSL_INCREMENT_DYN_STAT(ssl_total_tickets_not_found_stat);
    return 0;
  }

  return -1;
}
#endif /* HAVE_OPENSSL_SESSION_TICKETS */

void
SSLReleaseContext(SSL_CTX *ctx)
{
  SSL_CTX_free(ctx);
}

ssl_error_t
SSLWriteBuffer(SSL *ssl, const void *buf, int64_t nbytes, int64_t &nwritten)
{
  nwritten = 0;

  if (unlikely(nbytes == 0)) {
    return SSL_ERROR_NONE;
  }
  ERR_clear_error();
  int ret = SSL_write(ssl, buf, (int)nbytes);
  if (ret > 0) {
    nwritten = ret;
    BIO *bio = SSL_get_wbio(ssl);
    if (bio != NULL) {
      (void)BIO_flush(bio);
    }
    return SSL_ERROR_NONE;
  }
  int ssl_error = SSL_get_error(ssl, ret);
  if (ssl_error == SSL_ERROR_SSL) {
    char buf[512];
    unsigned long e = ERR_peek_last_error();
    ERR_error_string_n(e, buf, sizeof(buf));
    Debug("ssl.error.write", "SSL write returned %d, ssl_error=%d, ERR_get_error=%ld (%s)", ret, ssl_error, e, buf);
  }
  return ssl_error;
}

ssl_error_t
SSLReadBuffer(SSL *ssl, void *buf, int64_t nbytes, int64_t &nread)
{
  nread = 0;

  if (unlikely(nbytes == 0)) {
    return SSL_ERROR_NONE;
  }
  ERR_clear_error();
  int ret = SSL_read(ssl, buf, (int)nbytes);
  if (ret > 0) {
    nread = ret;
    return SSL_ERROR_NONE;
  }
  int ssl_error = SSL_get_error(ssl, ret);
  if (ssl_error == SSL_ERROR_SSL) {
    char buf[512];
    unsigned long e = ERR_peek_last_error();
    ERR_error_string_n(e, buf, sizeof(buf));
    Debug("ssl.error.read", "SSL read returned %d, ssl_error=%d, ERR_get_error=%ld (%s)", ret, ssl_error, e, buf);
  }

  return ssl_error;
}

ssl_error_t
SSLAccept(SSL *ssl)
{
  ERR_clear_error();
  int ret = SSL_accept(ssl);
  if (ret > 0) {
    return SSL_ERROR_NONE;
  }
  int ssl_error = SSL_get_error(ssl, ret);
  if (ssl_error == SSL_ERROR_SSL) {
    char buf[512];
    unsigned long e = ERR_peek_last_error();
    ERR_error_string_n(e, buf, sizeof(buf));
    Debug("ssl.error.accept", "SSL accept returned %d, ssl_error=%d, ERR_get_error=%ld (%s)", ret, ssl_error, e, buf);
  }

  return ssl_error;
}

ssl_error_t
SSLConnect(SSL *ssl)
{
  ERR_clear_error();
  int ret = SSL_connect(ssl);
  if (ret > 0) {
    return SSL_ERROR_NONE;
  }
  int ssl_error = SSL_get_error(ssl, ret);
  if (ssl_error == SSL_ERROR_SSL) {
    char buf[512];
    unsigned long e = ERR_peek_last_error();
    ERR_error_string_n(e, buf, sizeof(buf));
    Debug("ssl.error.connect", "SSL connect returned %d, ssl_error=%d, ERR_get_error=%ld (%s)", ret, ssl_error, e, buf);
  }

  return ssl_error;
}