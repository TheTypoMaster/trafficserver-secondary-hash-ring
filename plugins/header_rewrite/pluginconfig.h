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
 * pluginconfig.h
 *
 *  Created on: Jul 15, 2014
 *      Author: jlaue
 */

#define UID_LEN 32

class PluginConfig
{
public:
  PluginConfig(){};
  virtual ~PluginConfig(){};

  virtual bool
  parse_config(const std::string path)
  {
    TSDebug("", "new_config parse failed: %s", path.c_str());
    return 0;
  }
  virtual PluginConfig *
  clone()
  {
    return 0;
  }
};

class ConfigHolder
{
public:
  ConfigHolder(PluginConfig *config, const char *defaultConfigName, const char *pluginName)
    : config(config), log(0), config_path(0), last_load(0), default_config_name(defaultConfigName), pluginName(pluginName)
  {
    snprintf(uniqueID, UID_LEN, "%p", this);
  }
  ~ConfigHolder()
  {
    delete config;
    if (config_path)
      TSfree(config_path);
    if (log)
      TSTextLogObjectDestroy(log);
  }
  const char *
  getPluginName()
  {
    return pluginName;
  }
  ConfigHolder *init(const char *path);
  bool addUpdateRegister();
  bool removeUpdateRegister();

  static PluginConfig *get_config(TSCont cont);

  PluginConfig *config;

private:
  TSTextLogObject log;
  char *config_path;
  volatile time_t last_load;
  const char *default_config_name;
  const char *pluginName;
  char uniqueID[UID_LEN];
  TSCont config_cont;

  void load_config_file();

  static int config_handler(TSCont cont, TSEvent event, void *edata);
};
