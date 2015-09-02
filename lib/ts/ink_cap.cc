/** @file

    A brief file description

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
#include "Diags.h"
#include "ink_cap.h"
#include "ink_thread.h"

#include <grp.h>

#if HAVE_SYS_CAPABILITY_H
#include <sys/capability.h>
#endif

#if HAVE_SYS_PRCTL_H
#include <sys/prctl.h>
#endif

// NOTE: Failing to acquire or release privileges is a fatal error. This is because that should never happen
// and if it does, it is likely that some fundamental security assumption has been violated. In that case
// it is dangerous to continue.

#if !TS_USE_POSIX_CAP
ink_mutex ElevateAccess::lock = INK_MUTEX_INIT;
#endif

#define DEBUG_CREDENTIALS(tag)                                                                                               \
  do {                                                                                                                       \
    if (is_debug_tag_set(tag)) {                                                                                             \
      uid_t uid = -1, euid = -1, suid = -1;                                                                                  \
      gid_t gid = -1, egid = -1, sgid = -1;                                                                                  \
      getresuid(&uid, &euid, &suid);                                                                                         \
      getresgid(&gid, &egid, &sgid);                                                                                         \
      Debug(tag, "uid=%ld, gid=%ld, euid=%ld, egid=%ld, suid=%ld, sgid=%ld", static_cast<long>(uid), static_cast<long>(gid), \
            static_cast<long>(euid), static_cast<long>(egid), static_cast<long>(suid), static_cast<long>(sgid));             \
    }                                                                                                                        \
  } while (0)

#if TS_USE_POSIX_CAP

#define DEBUG_PRIVILEGES(tag)                                                                                    \
  do {                                                                                                           \
    if (is_debug_tag_set(tag)) {                                                                                 \
      cap_t caps = cap_get_proc();                                                                               \
      char *caps_text = cap_to_text(caps, NULL);                                                                 \
      Debug(tag, "caps='%s', core=%s, death signal=%d, thread=0x%llx", caps_text, is_dumpable(), death_signal(), \
            (unsigned long long)pthread_self());                                                                 \
      cap_free(caps_text);                                                                                       \
      cap_free(caps);                                                                                            \
    }                                                                                                            \
  } while (0)

#else /* TS_USE_POSIX_CAP */

#define DEBUG_PRIVILEGES(tag)                                                                       \
  do {                                                                                              \
    if (is_debug_tag_set(tag)) {                                                                    \
      Debug(tag, "caps='', core=%s, death signal=%d, thread=0x%llx", is_dumpable(), death_signal(), \
            (unsigned long long)pthread_self());                                                    \
    }                                                                                               \
  } while (0)

#endif /* TS_USE_POSIX_CAP */

#if !HAVE_GETRESUID
static int
getresuid(uid_t *uid, uid_t *euid, uid_t *suid)
{
  *uid = getuid();
  *euid = geteuid();
  return 0;
}
#endif /* !HAVE_GETRESUID */

#if !HAVE_GETRESGID
static int
getresgid(gid_t *gid, gid_t *egid, gid_t *sgid)
{
  *gid = getgid();
  *egid = getegid();
  return 0;
}
#endif /* !HAVE_GETRESGID */

static unsigned
max_passwd_size()
{
#if defined(_SC_GETPW_R_SIZE_MAX)
  long val = sysconf(_SC_GETPW_R_SIZE_MAX);
  if (val > 0) {
    return (unsigned)val;
  }
#endif

  return 4096;
}

static const char *
is_dumpable()
{
#if defined(PR_GET_DUMPABLE)
  return (prctl(PR_GET_DUMPABLE) != 1) ? "disabled" : "enabled";
#else
  return "unknown";
#endif
}

static int
death_signal()
{
  int signum = -1;

#if defined(PR_GET_PDEATHSIG)
  prctl(PR_GET_PDEATHSIG, &signum, 0, 0, 0);
#endif

  return signum;
}

void
DebugCapabilities(char const *tag)
{
  DEBUG_CREDENTIALS(tag);
  DEBUG_PRIVILEGES(tag);
}

static void
impersonate(const struct passwd *pwd, ImpersonationLevel level)
{
  int deathsig = death_signal();
  bool dumpable = false;

  DEBUG_CREDENTIALS("privileges");
  DEBUG_PRIVILEGES("privileges");

  ink_release_assert(pwd != NULL);

#if defined(PR_GET_DUMPABLE)
  dumpable = (prctl(PR_GET_DUMPABLE) == 1);
#endif

  // Always repopulate the supplementary group list for the new user.
  initgroups(pwd->pw_name, pwd->pw_gid);

  switch (level) {
  case IMPERSONATE_PERMANENT:
    if (setregid(pwd->pw_gid, pwd->pw_gid) != 0) {
      Fatal("switching to user %s, failed to set group ID %ld", pwd->pw_name, (long)pwd->pw_gid);
    }

    if (setreuid(pwd->pw_uid, pwd->pw_uid) != 0) {
      Fatal("switching to user %s, failed to set user ID %ld", pwd->pw_name, (long)pwd->pw_uid);
    }
    break;

  case IMPERSONATE_EFFECTIVE:
    if (setegid(pwd->pw_gid) != 0) {
      Fatal("switching to user %s, failed to set group ID %ld", pwd->pw_name, (long)pwd->pw_gid);
    }

    if (seteuid(pwd->pw_uid) != 0) {
      Fatal("switching to user %s, failed to set effective user ID %ld", pwd->pw_name, (long)pwd->pw_gid);
    }
    break;
  }

  // Reset process flags if necessary. Elevating privilege using capabilities does not reset process
  // flags, so we don't have to bother with this in elevateFileAccess().

  EnableCoreFile(dumpable);

  if (deathsig > 0) {
    EnableDeathSignal(deathsig);
  }

  DEBUG_CREDENTIALS("privileges");
  DEBUG_PRIVILEGES("privileges");
}

void
ImpersonateUserID(uid_t uid, ImpersonationLevel level)
{
  struct passwd *pwd;
  struct passwd pbuf;
  char buf[max_passwd_size()];

  if (getpwuid_r(uid, &pbuf, buf, sizeof(buf), &pwd) != 0) {
    Fatal("missing password database entry for UID %ld: %s", (long)uid, strerror(errno));
  }

  if (pwd == NULL) {
    // Password entry not found ...
    Fatal("missing password database entry for UID %ld", (long)uid);
  }

  impersonate(pwd, level);
}

void
ImpersonateUser(const char *user, ImpersonationLevel level)
{
  struct passwd *pwd;
  struct passwd pbuf;
  char buf[max_passwd_size()];

  if (*user == '#') {
    // Numeric user notation.
    uid_t uid = (uid_t)atoi(&user[1]);
    if (getpwuid_r(uid, &pbuf, buf, sizeof(buf), &pwd) != 0) {
      Fatal("missing password database entry for UID %ld: %s", (long)uid, strerror(errno));
    }
  } else {
    if (getpwnam_r(user, &pbuf, buf, sizeof(buf), &pwd) != 0) {
      Fatal("missing password database entry for username '%s': %s", user, strerror(errno));
    }
  }

  if (pwd == NULL) {
    // Password entry not found ...
    Fatal("missing password database entry for '%s'", user);
  }

  impersonate(pwd, level);
}

bool
PreserveCapabilities()
{
  int zret = 0;
#if TS_USE_POSIX_CAP
  zret = prctl(PR_SET_KEEPCAPS, 1);
#endif
  Debug("privileges", "[PreserveCapabilities] zret : %d\n", zret);
  return zret == 0;
}

// Adjust the capabilities to only those needed.
bool
RestrictCapabilities()
{
  int zret = 0; // return value.
#if TS_USE_POSIX_CAP
  cap_t caps = cap_init(); // start with nothing.
  // Capabilities we need.
  cap_value_t perm_list[] = {CAP_NET_ADMIN, CAP_NET_BIND_SERVICE, CAP_IPC_LOCK, CAP_DAC_OVERRIDE};
  static int const PERM_CAP_COUNT = sizeof(perm_list) / sizeof(*perm_list);
  cap_value_t eff_list[] = {CAP_NET_ADMIN, CAP_NET_BIND_SERVICE, CAP_IPC_LOCK};
  static int const EFF_CAP_COUNT = sizeof(eff_list) / sizeof(*eff_list);

  cap_set_flag(caps, CAP_PERMITTED, PERM_CAP_COUNT, perm_list, CAP_SET);
  cap_set_flag(caps, CAP_EFFECTIVE, EFF_CAP_COUNT, eff_list, CAP_SET);
  zret = cap_set_proc(caps);
  cap_free(caps);
#endif
  Debug("privileges", "[RestrictCapabilities] zret : %d\n", zret);
  return zret == 0;
}

bool
EnableCoreFile(bool flag)
{
  int zret = 0;

#if defined(PR_SET_DUMPABLE)
  int state = flag ? 1 : 0;
  if (0 > (zret = prctl(PR_SET_DUMPABLE, state, 0, 0, 0))) {
    Warning("Unable to set PR_DUMPABLE : %s", strerror(errno));
  } else if (state != prctl(PR_GET_DUMPABLE)) {
    zret = ENOSYS; // best guess
    Warning("Call to set PR_DUMPABLE was ineffective");
  }
#endif // linux check

  Debug("privileges", "[EnableCoreFile] zret : %d\n", zret);
  return zret == 0;
}

void
EnableDeathSignal(int signum)
{
  (void)signum;

#if defined(PR_SET_PDEATHSIG)
  if (prctl(PR_SET_PDEATHSIG, signum, 0, 0, 0) != 0) {
    Debug("privileges", "prctl(PR_SET_PDEATHSIG) failed: %s", strerror(errno));
  }
#endif
}

#if TS_USE_POSIX_CAP
/** Control file access privileges to bypass DAC.
    @parm state Use @c true to enable elevated privileges,
    @c false to disable.
    @return @c true if successful, @c false otherwise.

    @internal After some pondering I decided that the file access
    privilege was worth the effort of restricting. Unlike the network
    privileges this can protect a host system from programming errors
    by not (usually) permitting such errors to access arbitrary
    files. This is particularly true since none of the config files
    current enable this feature so it's not actually called. Still,
    best to program defensively and have it available.
 */
static void
elevateFileAccess(unsigned level, bool state)
{
  Debug("privileges", "[elevateFileAccess] state : %d\n", state);

  cap_t cap_state = cap_get_proc(); // current capabilities

  unsigned cap_count = 0;
  cap_value_t cap_list[2];

  if (level & ElevateAccess::FILE_PRIVILEGE) {
    cap_list[cap_count] = CAP_DAC_OVERRIDE;
    ++cap_count;
  }

  if (level & ElevateAccess::TRACE_PRIVILEGE) {
    cap_list[cap_count] = CAP_SYS_PTRACE;
    ++cap_count;
  }

  ink_release_assert(cap_count <= sizeof(cap_list));

  cap_set_flag(cap_state, CAP_EFFECTIVE, cap_count, cap_list, state ? CAP_SET : CAP_CLEAR);
  if (cap_set_proc(cap_state) != 0) {
    Fatal("failed to %s privileged capabilities: %s", state ? "acquire" : "release", strerror(errno));
  }

  cap_free(cap_state);
}
#endif

ElevateAccess::ElevateAccess(const bool state, unsigned lvl) : elevated(false), saved_uid(geteuid()), level(lvl)
{
  // XXX Squash a clang [-Wunused-private-field] warning. The right solution is probably to extract
  // the capabilities into a policy class.
  (void)level;

  if (state == true) {
    elevate();
#if !TS_USE_POSIX_CAP
    DEBUG_CREDENTIALS("privileges");
#endif
    DEBUG_PRIVILEGES("privileges");
  }
}

ElevateAccess::~ElevateAccess()
{
  if (elevated == true) {
    demote();
#if !TS_USE_POSIX_CAP
    DEBUG_CREDENTIALS("privileges");
#endif
    DEBUG_PRIVILEGES("privileges");
  }
}

void
ElevateAccess::elevate()
{
#if TS_USE_POSIX_CAP
  elevateFileAccess(level, true);
#else
  // Since we are setting a process-wide credential, we have to block any other thread
  // attempting to elevate until this one demotes.
  ink_mutex_acquire(&lock);
  ImpersonateUserID(0, IMPERSONATE_EFFECTIVE);
#endif
  elevated = true;
}

void
ElevateAccess::demote()
{
#if TS_USE_POSIX_CAP
  elevateFileAccess(level, false);
#else
  ImpersonateUserID(saved_uid, IMPERSONATE_EFFECTIVE);
  ink_mutex_release(&lock);
#endif
  elevated = false;
}