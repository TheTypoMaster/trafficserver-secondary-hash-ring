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

#include "ink_defs.h"
#include "ink_platform.h"

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdbool.h>
#include <getopt.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ts/ts.h>

#ifdef HAVE_PCRE_PCRE_H
#include <pcre/pcre.h>
#else
#include <pcre.h>
#endif

typedef struct invalidate_t {
  const char *regex_text;
  pcre *regex;
  pcre_extra *regex_extra;
  time_t epoch;
  time_t expiry;
  struct invalidate_t *volatile next;
} invalidate_t;

typedef invalidate_t config_t;

typedef struct {
  char *config_path;
  volatile time_t last_load;
  config_t *config;
  TSTextLogObject log;
} config_holder_t;

static int free_handler(TSCont cont, TSEvent event, void *edata);
static int config_handler(TSCont cont, TSEvent event, void *edata);
static config_t *get_config(TSCont cont);
static config_holder_t *new_config_holder();
static config_holder_t *init_config_holder(config_holder_t *config_holder, const char *path);
static void free_config_holder_t(config_holder_t *config_holder);
static void schedule_free_invalidate_t(invalidate_t *iptr);

#define PLUGIN_TAG "regex_revalidate"
#define DEFAULT_CONFIG_NAME "regex_revalidate.config"
#define PRUNE_TMOUT 60000
#define FREE_TMOUT 300000
#define OVECTOR_SIZE 30
#define LOG_ROLL_INTERVAL 86400
#define LOG_ROLL_OFFSET 0

static inline void *
ts_malloc(size_t s)
{
  return TSmalloc(s);
}

static inline void
ts_free(void *s)
{
  return TSfree(s);
}

static invalidate_t *
init_invalidate_t(invalidate_t *i)
{
  i->regex_text = NULL;
  i->regex = NULL;
  i->regex_extra = NULL;
  i->epoch = 0;
  i->expiry = 0;
  i->next = NULL;
  return i;
}

static void
free_invalidate_t(invalidate_t *i)
{
  if (i->regex_extra)
#ifndef PCRE_STUDY_JIT_COMPILE
    pcre_free(i->regex_extra);
#else
    pcre_free_study(i->regex_extra);
#endif
  if (i->regex)
    pcre_free(i->regex);
  if (i->regex_text)
    pcre_free_substring(i->regex_text);
  TSfree(i);
}

static void
free_invalidate_t_list(invalidate_t *i)
{
  if (i->next)
    free_invalidate_t_list(i->next);
  free_invalidate_t(i);
}

static bool
prune_config(invalidate_t **i)
{
  invalidate_t *iptr, *ilast;
  time_t now;
  bool pruned = false;

  now = time(NULL);

  if (*i) {
    iptr = *i;
    ilast = NULL;
    while (iptr) {
      if (difftime(iptr->expiry, now) < 0) {
        TSDebug(PLUGIN_TAG, "Removing %s expiry: %d now: %d", iptr->regex_text, (int)iptr->expiry, (int)now);
        TSError(PLUGIN_TAG " - Removing %s expiry: %d now: %d", iptr->regex_text, (int)iptr->expiry, (int)now);
        if (ilast) {
          // jlaue: TODO is this right?
          //                    iptr = __sync_val_compare_and_swap(&(ilast->next), ilast->next, iptr->next);
          ilast->next = iptr->next;
          //                    free_invalidate_t(iptr);
          schedule_free_invalidate_t(iptr);
          iptr = ilast->next;

        } else {
          *i = iptr->next;
          //                    free_invalidate_t(iptr);
          schedule_free_invalidate_t(iptr);
          iptr = *i;
        }
        pruned = true;
      } else {
        ilast = iptr;
        iptr = iptr->next;
      }
    }
  }
  return pruned;
}


static void
list_config(config_holder_t *config_holder, invalidate_t *i)
{
  invalidate_t *iptr;

  TSDebug(PLUGIN_TAG, "Current config:");
  if (config_holder->log)
    TSTextLogObjectWrite(config_holder->log, "Current config:");
  if (i) {
    iptr = i;
    while (iptr) {
      TSDebug(PLUGIN_TAG, "%s epoch: %d expiry: %d", iptr->regex_text, (int)iptr->epoch, (int)iptr->expiry);
      if (config_holder->log)
        TSTextLogObjectWrite(config_holder->log, "%s epoch: %d expiry: %d", iptr->regex_text, (int)iptr->epoch, (int)iptr->expiry);
      iptr = iptr->next;
    }
  } else {
    TSDebug(PLUGIN_TAG, "EMPTY");
    if (config_holder->log)
      TSTextLogObjectWrite(config_holder->log, "EMPTY");
  }
}

static int
config_pruner(TSCont cont, TSEvent event ATS_UNUSED, void *edata ATS_UNUSED)
{
  invalidate_t *i;

  TSDebug(PLUGIN_TAG, "config_pruner");
  config_holder_t *configh = (config_holder_t *)TSContDataGet(cont);
  i = configh->config;

  prune_config(&i);

  configh->config = i;

  TSContSchedule(cont, PRUNE_TMOUT, TS_THREAD_POOL_TASK);
  return 0;
}

static time_t
get_date_from_cached_hdr(TSHttpTxn txn)
{
  TSMBuffer buf;
  TSMLoc hdr_loc, date_loc;
  time_t date = 0;

  if (TSHttpTxnCachedRespGet(txn, &buf, &hdr_loc) == TS_SUCCESS) {
    date_loc = TSMimeHdrFieldFind(buf, hdr_loc, TS_MIME_FIELD_DATE, TS_MIME_LEN_DATE);
    if (date_loc != TS_NULL_MLOC) {
      date = TSMimeHdrFieldValueDateGet(buf, hdr_loc, date_loc);
      TSHandleMLocRelease(buf, hdr_loc, date_loc);
    }
    TSHandleMLocRelease(buf, TS_NULL_MLOC, hdr_loc);
  }

  return date;
}

static int
main_handler(TSCont cont, TSEvent event, void *edata)
{
  TSHttpTxn txn = (TSHttpTxn)edata;
  int status;
  invalidate_t *iptr;

  time_t date = 0, now = 0;
  char *url = NULL;
  int url_len = 0;

  switch (event) {
  case TS_EVENT_HTTP_CACHE_LOOKUP_COMPLETE:
    if (TSHttpTxnCacheLookupStatusGet(txn, &status) == TS_SUCCESS) {
      if (status == TS_CACHE_LOOKUP_HIT_FRESH) {
        iptr = get_config(cont);
        while (iptr) {
          if (!date) {
            date = get_date_from_cached_hdr(txn);
            now = time(NULL);
          }
          if ((difftime(iptr->epoch, date) >= 0) && (difftime(iptr->expiry, now) >= 0)) {
            if (!url)
              url = TSHttpTxnEffectiveUrlStringGet(txn, &url_len);
            if (pcre_exec(iptr->regex, iptr->regex_extra, url, url_len, 0, 0, NULL, 0) >= 0) {
              TSHttpTxnCacheLookupStatusSet(txn, TS_CACHE_LOOKUP_HIT_STALE);
              iptr = NULL;
              TSDebug(PLUGIN_TAG, "Forced revalidate - %.*s", url_len, url);
            }
          }
          if (iptr)
            iptr = iptr->next;
        }
        if (url)
          TSfree(url);
      }
    }
    break;
  default:
    break;
  }

  TSHttpTxnReenable(txn, TS_EVENT_HTTP_CONTINUE);
  return 0;
}

static bool
check_ts_version()
{
  const char *ts_version = TSTrafficServerVersionGet();

  if (ts_version) {
    int major_ts_version = 0;
    int minor_ts_version = 0;
    int micro_ts_version = 0;

    if (sscanf(ts_version, "%d.%d.%d", &major_ts_version, &minor_ts_version, &micro_ts_version) != 3) {
      return false;
    }

    if ((TS_VERSION_MAJOR == major_ts_version) && (TS_VERSION_MINOR == minor_ts_version) &&
        (TS_VERSION_MICRO == micro_ts_version)) {
      return true;
    }
  }

  return false;
}

void
TSPluginInit(int argc, const char *argv[])
{
  TSPluginRegistrationInfo info;
  TSCont main_cont, config_cont;
  config_holder_t *config_holder;
  char *path = NULL;

  TSDebug(PLUGIN_TAG, "Starting plugin init.");

  config_holder = new_config_holder();

  int c;
  optind = 1;
  static const struct option longopts[] = {
    {"config", required_argument, NULL, 'c'}, {"log", required_argument, NULL, 'l'}, {NULL, 0, NULL, 0}};

  while ((c = getopt_long(argc, (char *const *)argv, "c:l:", longopts, NULL)) != -1) {
    switch (c) {
    case 'c':
      path = TSstrdup(optarg);
      break;
    case 'l':
      TSTextLogObjectCreate(optarg, TS_LOG_MODE_ADD_TIMESTAMP, &config_holder->log);
      TSTextLogObjectRollingEnabledSet(config_holder->log, 1);
      TSTextLogObjectRollingIntervalSecSet(config_holder->log, LOG_ROLL_INTERVAL);
      TSTextLogObjectRollingOffsetHrSet(config_holder->log, LOG_ROLL_OFFSET);
      break;
    default:
      break;
    }
  }
  config_holder = init_config_holder(config_holder, path);

  if (!config_holder->config_path) {
    TSError("Plugin requires a --config option along with a config file name.");
    free_config_holder_t(config_holder);
    return;
  }

  //    if (!load_config(free_config_holder_t, &iptr))
  if (config_holder->config)
    TSDebug(PLUGIN_TAG, "Problem loading config from file %s", config_holder->config_path);
  else {
    //        config_holder->config = iptr;
    list_config(config_holder, config_holder->config);
  }

  info.plugin_name = PLUGIN_TAG;
  info.vendor_name = "Apache Software Foundation";
  info.support_email = "dev@trafficserver.apache.org";

  if (TSPluginRegister(TS_SDK_VERSION_3_0, &info) != TS_SUCCESS) {
    TSError("Plugin registration failed.");
    free_config_holder_t(config_holder);
    return;
  } else
    TSDebug(PLUGIN_TAG, "Plugin registration succeeded.");

  if (!check_ts_version()) {
    TSError("Plugin requires Traffic Server %d.%d.%d", TS_VERSION_MAJOR, TS_VERSION_MINOR, TS_VERSION_MICRO);
    free_config_holder_t(config_holder);
    return;
  }

  pcre_malloc = &ts_malloc;
  pcre_free = &ts_free;

  main_cont = TSContCreate(main_handler, NULL);
  TSContDataSet(main_cont, (void *)config_holder);
  TSHttpHookAdd(TS_HTTP_CACHE_LOOKUP_COMPLETE_HOOK, main_cont);

  config_cont = TSContCreate(config_pruner, TSMutexCreate());
  TSContDataSet(config_cont, (void *)config_holder);
  TSContSchedule(config_cont, PRUNE_TMOUT, TS_THREAD_POOL_TASK);

  config_cont = TSContCreate(config_handler, TSMutexCreate());
  TSContDataSet(config_cont, (void *)config_holder);
  TSMgmtUpdateRegister(config_cont, PLUGIN_TAG);

  TSDebug(PLUGIN_TAG, "Plugin Init Complete.");
}

static config_t *
new_config(TSFile fs)
{
  char line[LINE_MAX];
  time_t now;
  pcre *config_re;
  const char *errptr;
  int erroffset, ovector[OVECTOR_SIZE], rc;
  int ln = 0;
  invalidate_t *iptr, *i, *config = 0;

  now = time(NULL);

  config_re = pcre_compile("^([^#].+?)\\s+(\\d+)\\s*$", 0, &errptr, &erroffset, NULL);
  while (TSfgets(fs, line, LINE_MAX - 1) != NULL) {
    ln++;
    TSDebug(PLUGIN_TAG, "Processing: %d %s", ln, line);
    rc = pcre_exec(config_re, NULL, line, strlen(line), 0, 0, ovector, OVECTOR_SIZE);
    if (rc == 3) {
      i = (invalidate_t *)TSmalloc(sizeof(invalidate_t));
      init_invalidate_t(i);
      pcre_get_substring(line, ovector, rc, 1, &i->regex_text);
      i->epoch = now;
      i->expiry = atoi(line + ovector[4]);
      i->regex = pcre_compile(i->regex_text, 0, &errptr, &erroffset, NULL);
      if (i->expiry <= i->epoch) {
        TSDebug(PLUGIN_TAG, "NOT Loaded, already expired! %s %d %d", i->regex_text, (int)i->epoch, (int)i->expiry);
        TSError(PLUGIN_TAG " - NOT Loaded, already expired: %s %d %d", i->regex_text, (int)i->epoch, (int)i->expiry);
        free_invalidate_t(i);
      } else if (i->regex == NULL) {
        TSDebug(PLUGIN_TAG, "%s did not compile", i->regex_text);
        free_invalidate_t(i);
      } else {
        i->regex_extra = pcre_study(i->regex, 0, &errptr);
        if (!config) {
          config = i;
          TSDebug(PLUGIN_TAG, "Created new list and Loaded %s %d %d", i->regex_text, (int)i->epoch, (int)i->expiry);
          TSError(PLUGIN_TAG " - New Revalidate: %s %d %d", i->regex_text, (int)i->epoch, (int)i->expiry);
        } else {
          iptr = config;
          while (1) {
            if (strcmp(i->regex_text, iptr->regex_text) == 0) {
              if (iptr->expiry != i->expiry) {
                TSDebug(PLUGIN_TAG, "Updating duplicate %s", i->regex_text);
                iptr->epoch = i->epoch;
                iptr->expiry = i->expiry;
              }
              free_invalidate_t(i);
              i = NULL;
              break;
            } else if (!iptr->next)
              break;
            else
              iptr = iptr->next;
          }
          if (i) {
            iptr->next = i;
            TSDebug(PLUGIN_TAG, "Loaded %s %d %d", i->regex_text, (int)i->epoch, (int)i->expiry);
          }
        }
      }
    } else
      TSDebug(PLUGIN_TAG, "Skipping line %d", ln);
  }
  pcre_free(config_re);

  return config;
}

static void
delete_config(config_t *config)
{
  TSDebug(PLUGIN_TAG, "Freeing config");
  free_invalidate_t_list(config);
}

static int
free_invalidate_handler(TSCont cont, TSEvent event ATS_UNUSED, void *edata ATS_UNUSED)
{
  invalidate_t *i = (invalidate_t *)TSContDataGet(cont);
  free_invalidate_t(i);
  TSContDestroy(cont);
  return 0;
}

static void
schedule_free_invalidate_t(invalidate_t *iptr)
{
  TSCont free_cont;
  free_cont = TSContCreate(free_invalidate_handler, NULL);
  TSContDataSet(free_cont, (void *)iptr);
  TSContSchedule(free_cont, FREE_TMOUT, TS_THREAD_POOL_TASK);
  return;
}

static config_t *
get_config(TSCont cont)
{
  config_holder_t *configh = (config_holder_t *)TSContDataGet(cont);
  if (!configh) {
    return 0;
  }
  return configh->config;
}

static void
load_config_file(config_holder_t *config_holder)
{
  TSFile fh;
  struct stat s;

  config_t *newconfig, *oldconfig;
  TSCont free_cont;

  // check date
  if (stat(config_holder->config_path, &s) < 0) {
    TSDebug(PLUGIN_TAG, "Could not stat %s", config_holder->config_path);
    if (config_holder->config) {
      return;
    }
  } else {
    TSDebug(PLUGIN_TAG, "s.st_mtime=%lu, last_load=%lu", s.st_mtime, config_holder->last_load);
    if (s.st_mtime < config_holder->last_load) {
      return;
    }
  }

  TSDebug(PLUGIN_TAG, "Opening config file: %s", config_holder->config_path);
  fh = TSfopen(config_holder->config_path, "r");
  TSError(PLUGIN_TAG " - Reading config: %s", config_holder->config_path);

  if (!fh) {
    TSError("[%s] Unable to open config: %s.\n", PLUGIN_TAG, config_holder->config_path);
    return;
  }

  newconfig = 0;
  newconfig = new_config(fh);
  if (newconfig) {
    config_holder->last_load = time(NULL);
    config_t **confp = &(config_holder->config);
    oldconfig = __sync_lock_test_and_set(confp, newconfig);
    if (oldconfig) {
      TSDebug(PLUGIN_TAG, "scheduling free: %p (%p)", oldconfig, newconfig);
      free_cont = TSContCreate(free_handler, NULL);
      TSContDataSet(free_cont, (void *)oldconfig);
      TSContSchedule(free_cont, FREE_TMOUT, TS_THREAD_POOL_TASK);
    }
  }
  if (fh)
    TSfclose(fh);
  return;
}

static config_holder_t *
new_config_holder(void)
{
  config_holder_t *config_holder = TSmalloc(sizeof(config_holder_t));
  return config_holder;
}

static config_holder_t *
init_config_holder(config_holder_t *config_holder, const char *path)
{
  int path_len = 0;
  config_holder->config_path = 0;
  config_holder->config = 0;
  config_holder->last_load = 0;
  config_holder->log = 0;

  if (!path)
    path = DEFAULT_CONFIG_NAME;
  if (path[0] != '/') {
    path_len = strlen(TSConfigDirGet()) + strlen(path) + 2;
    config_holder->config_path = ts_malloc(path_len);
    snprintf(config_holder->config_path, path_len, "%s/%s", TSConfigDirGet(), path);
    TSDebug(PLUGIN_TAG, "path: '%s' len=%d", config_holder->config_path, path_len);
  } else
    config_holder->config_path = TSstrdup(path);

  load_config_file(config_holder);
  return config_holder;
}

static void
free_config_holder_t(config_holder_t *config_holder)
{
  if (config_holder->config)
    free_invalidate_t_list(config_holder->config);
  if (config_holder->config_path)
    TSfree(config_holder->config_path);
  if (config_holder->log)
    TSTextLogObjectDestroy(config_holder->log);
  TSfree(config_holder);
}

static int
free_handler(TSCont cont, TSEvent event ATS_UNUSED, void *edata ATS_UNUSED)
{
  config_t *config;

  TSDebug(PLUGIN_TAG, "Freeing old config");
  config = (config_t *)TSContDataGet(cont);
  delete_config(config);
  TSContDestroy(cont);
  return 0;
}

static int
config_handler(TSCont cont, TSEvent event ATS_UNUSED, void *edata ATS_UNUSED)
{
  config_holder_t *config_holder;

  TSDebug(PLUGIN_TAG, "In config Handler");
  config_holder = (config_holder_t *)TSContDataGet(cont);
  load_config_file(config_holder);
  return 0;
}
