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

/*
 * pluginconfig.cc
 *
 *  Created on: Jul 15, 2014
 *      Author: jlaue
 */

#include <sys/stat.h>
#include <time.h>
#include <stdio.h>
#include <string>

#include "ink_defs.h"
#include "ts/ts.h"
#include "pluginconfig.h"

#define FREE_TMOUT 300000

static int free_handler(TSCont cont, TSEvent event, void *edata);

PluginConfig *
ConfigHolder::get_config(TSCont cont)
{
  ConfigHolder *configh = (ConfigHolder *)TSContDataGet(cont);
  if (!configh) {
    return 0;
  }
  return configh->config;
}

void
ConfigHolder::load_config_file()
{
  struct stat s;

  PluginConfig *newconfig, *oldconfig;
  TSCont free_cont;

  TSDebug(pluginName, "load_config_file() here");

  // check date
  if (stat(config_path, &s) < 0) {
    TSDebug(pluginName, "Could not stat %s", config_path);
    if (config) {
      return;
    }
  } else {
    TSDebug(pluginName, "s.st_mtime=%lu, last_load=%lu", s.st_mtime, last_load);
    if (s.st_mtime < last_load) {
      return;
    }
  }

  TSDebug(pluginName, "Calling new_config: %s / %p", config_path, config);
  newconfig = config->clone();
  if (newconfig) {
    if (newconfig->parse_config(config_path)) {
      TSDebug(pluginName, "after new_config parse: %s", config_path);
      last_load = time(NULL);
      PluginConfig **confp = &(config);
      oldconfig = __sync_lock_test_and_set(confp, newconfig);
      if (oldconfig) {
        TSDebug(pluginName, "scheduling free: %p (%p)", oldconfig, newconfig);
        free_cont = TSContCreate(free_handler, NULL);
        TSContDataSet(free_cont, (void *)oldconfig);
        TSContSchedule(free_cont, FREE_TMOUT, TS_THREAD_POOL_TASK);
      }
    } else {
      TSDebug(pluginName, "new_config parse failed: %s", config_path);
      delete newconfig;
    }
  } else {
    TSDebug(pluginName, "config clone failed");
  }
  TSDebug(pluginName, "load_config_file end");
  return;
}

ConfigHolder *
ConfigHolder::init(const char *path)
{
  char default_config_file[1024];

  if (path) {
    if (path[0] != '/') {
      sprintf(default_config_file, "%s/%s", TSConfigDirGet(), path);
      config_path = TSstrdup(default_config_file);
    } else {
      config_path = TSstrdup(path);
    }
  } else {
    /* Default config file of plugins/cacheurl.config */
    sprintf(default_config_file, "%s/%s", TSConfigDirGet(), default_config_name);
    config_path = TSstrdup(default_config_file);
  }
  TSDebug(pluginName, "calling load_config_file()");
  load_config_file();
  return this;
}

static int
free_handler(TSCont cont, TSEvent event, void *edata)
{
  (void)event;
  (void)edata;
  PluginConfig *config;

  TSDebug("free_handler", "Freeing old config");
  config = (PluginConfig *)TSContDataGet(cont);
  delete (config);
  TSContDestroy(cont);
  return 0;
}

int
ConfigHolder::config_handler(TSCont cont, TSEvent event, void *edata)
{
  (void)event;
  (void)edata;
  ConfigHolder *ch;

  ch = (ConfigHolder *)TSContDataGet(cont);
  TSDebug(ch->getPluginName(), "In config Handler");
  ch->load_config_file();
  return 0;
}

bool
ConfigHolder::addUpdateRegister()
{
  config_cont = TSContCreate(config_handler, TSMutexCreate());
  TSContDataSet(config_cont, (void *)this);
  TSMgmtUpdateRegister(config_cont, uniqueID);
  return true;
}

bool
ConfigHolder::removeUpdateRegister()
{
  TSMgmtUnRegister(uniqueID);
  TSContDestroy(config_cont);
  return true;
}
