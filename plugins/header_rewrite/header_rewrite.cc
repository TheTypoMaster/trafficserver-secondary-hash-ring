/*
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
#include <fstream>
#include <string>

#include "ts/ts.h"
#include "ts/remap.h"

#include "parser.h"
#include "ruleset.h"
#include "resources.h"

#include "pluginconfig.h"

// Debugs
const char PLUGIN_NAME[] = "header_rewrite";
const char PLUGIN_NAME_DBG[] = "dbg_header_rewrite";

// Forward declaration for the main continuation.
static int holder_rewrite_headers(TSCont contp, TSEvent event, void *edata);

// Simple wrapper around a configuration file / set. This is useful such that
// we can reuse most of the code for both global and per-remap rule sets.
class RulesConfig : public PluginConfig
{
public:
  RulesConfig(TSHttpHookID default_hook)
  {
    memset(_rules, 0, sizeof(_rules));
    memset(_resids, 0, sizeof(_resids));

    this->default_hook = default_hook;
    _cont = 0;
  }

  ~RulesConfig()
  {
    for (int i = TS_HTTP_READ_REQUEST_HDR_HOOK; i < TS_HTTP_LAST_HOOK; ++i) {
      delete _rules[i];
    }
  }

  TSCont
  continuation() const
  {
    return _cont;
  }
  void
  continuation(TSCont c)
  {
    _cont = c;
  }

  ResourceIDs
  resid(int hook) const
  {
    return _resids[hook];
  }
  RuleSet *
  rule(int hook) const
  {
    return _rules[hook];
  }

  virtual bool parse_config(const std::string fname);

  virtual PluginConfig *
  clone()
  {
    TSDebug(PLUGIN_NAME, "pr_list::load(TSFile fh)");
    RulesConfig *conf = new RulesConfig(this->default_hook);
    conf->_cont = this->_cont;
    return conf;
  }

  int rewrite_headers(TSEvent event, TSHttpTxn txnp);

private:
  bool add_rule(RuleSet *rule);

  TSCont _cont;
  RuleSet *_rules[TS_HTTP_LAST_HOOK + 1];
  ResourceIDs _resids[TS_HTTP_LAST_HOOK + 1];
  TSHttpHookID default_hook;
};

#define DEFAULT_CONFIG_NAME "header_rewrite.config"

// Helper function to add a rule to the rulesets
bool
RulesConfig::add_rule(RuleSet *rule)
{
  if (rule && rule->has_operator()) {
    TSDebug(PLUGIN_NAME_DBG, "   Adding rule to hook=%s\n", TSHttpHookNameLookup(rule->get_hook()));
    if (NULL == _rules[rule->get_hook()]) {
      _rules[rule->get_hook()] = rule;
    } else {
      _rules[rule->get_hook()]->append(rule);
    }
    return true;
  }

  return false;
}

///////////////////////////////////////////////////////////////////////////////
// Config parser, use to parse both the global, and per-remap, configurations.
//
// Note that this isn't particularly efficient, but it's a startup time cost
// anyways (or reload for remap.config), so not really in the critical path.
//
bool
RulesConfig::parse_config(const std::string fname)
{
  RuleSet *rule = NULL;
  std::string filename;
  std::ifstream f;
  int lineno = 0;
  TSDebug(PLUGIN_NAME, "parse_config");

  if (0 == fname.size()) {
    TSError("%s: no config filename provided", PLUGIN_NAME);
    return false;
  }

  if (fname[0] != '/') {
    filename = TSConfigDirGet();
    filename += "/" + fname;
  } else {
    filename = fname;
  }

  f.open(filename.c_str(), std::ios::in);
  if (!f.is_open()) {
    TSError("%s: unable to open %s", PLUGIN_NAME, filename.c_str());
    return false;
  }

  while (!f.eof()) {
    std::string line;

    getline(f, line);
    ++lineno; // ToDo: we should probably use this for error messages ...
    TSDebug(PLUGIN_NAME_DBG, "Reading line: %d: %s", lineno, line.c_str());

    while (std::isspace(line[0])) {
      line.erase(0, 1);
    }

    while (std::isspace(line[line.length() - 1])) {
      line.erase(line.length() - 1, 1);
    }

    if (line.empty() || (line[0] == '#')) {
      continue;
    }

    // include -> file reference
    int inp = line.find("include ");
    TSDebug(PLUGIN_NAME, "inp: %d: %s", inp, line.c_str());
    if (inp >= 0) {
      std::string path = line.substr(inp + strlen("include "));
      while (std::isspace(path[0])) {
        path.erase(0, 1);
      }

      while (std::isspace(path[path.length() - 1])) {
        path.erase(path.length() - 1, 1);
      }
      TSDebug(PLUGIN_NAME, "load included config file: %s", path.c_str());
      parse_config(path);
    }

    Parser p(line); // Tokenize and parse this line
    if (p.empty()) {
      continue;
    }

    // If we are at the beginning of a new condition, save away the previous rule (but only if it has operators).
    if (p.is_cond() && add_rule(rule)) {
      rule = NULL;
    }

    if (NULL == rule) {
      rule = new RuleSet();
      rule->set_hook(default_hook);

      // Special case for specifying the HOOK this rule applies to.
      // These can only be at the beginning of a rule, and have an implicit [AND].
      if (p.cond_op_is("TXN_START_HOOK")) {
        rule->set_hook(TS_HTTP_TXN_START_HOOK);
        continue;
      } else if (p.cond_op_is("READ_RESPONSE_HDR_HOOK")) {
        rule->set_hook(TS_HTTP_READ_RESPONSE_HDR_HOOK);
        continue;
      } else if (p.cond_op_is("READ_REQUEST_HDR_HOOK")) {
        rule->set_hook(TS_HTTP_READ_REQUEST_HDR_HOOK);
        continue;
      } else if (p.cond_op_is("READ_REQUEST_PRE_REMAP_HOOK")) {
        rule->set_hook(TS_HTTP_READ_REQUEST_PRE_REMAP_HOOK);
        continue;
      } else if (p.cond_op_is("SEND_REQUEST_HDR_HOOK")) {
        rule->set_hook(TS_HTTP_SEND_REQUEST_HDR_HOOK);
        continue;
      } else if (p.cond_op_is("SEND_RESPONSE_HDR_HOOK")) {
        rule->set_hook(TS_HTTP_SEND_RESPONSE_HDR_HOOK);
        continue;
      } else if (p.cond_op_is("REMAP_PSEUDO_HOOK")) {
        rule->set_hook(TS_REMAP_PSEUDO_HOOK);
        continue;
      } else if (p.cond_op_is("TXN_CLOSE_HOOK")) {
        rule->set_hook(TS_HTTP_TXN_CLOSE_HOOK);
        continue;
      }
    }

    if (p.is_cond()) {
      rule->add_condition(p);
    } else {
      rule->add_operator(p);
    }
  }

  // Add the last rule (possibly the only rule)
  add_rule(rule);

  // Collect all resource IDs that we need
  for (int i = TS_HTTP_READ_REQUEST_HDR_HOOK; i < TS_HTTP_LAST_HOOK; ++i) {
    if (_rules[i]) {
      _resids[i] = _rules[i]->get_all_resource_ids();
      if (default_hook == TS_HTTP_READ_RESPONSE_HDR_HOOK) {
        // TODO jlaue do not re-register
        TSDebug(PLUGIN_NAME, "Adding global ruleset to hook=%s", TSHttpHookNameLookup((TSHttpHookID)i));
        TSHttpHookAdd(static_cast<TSHttpHookID>(i), this->_cont);
      }
    }
  }

  return true;
}

///////////////////////////////////////////////////////////////////////////////
// Continuation
//
int
RulesConfig::rewrite_headers(TSEvent event, TSHttpTxn txnp)
{
  TSHttpHookID hook = TS_HTTP_LAST_HOOK;

  switch (event) {
  case TS_EVENT_HTTP_TXN_START:
    hook = TS_HTTP_TXN_START_HOOK;
    break;
  case TS_EVENT_HTTP_READ_RESPONSE_HDR:
    hook = TS_HTTP_READ_RESPONSE_HDR_HOOK;
    break;
  case TS_EVENT_HTTP_READ_REQUEST_HDR:
    hook = TS_HTTP_READ_REQUEST_HDR_HOOK;
    break;
  case TS_EVENT_HTTP_READ_REQUEST_PRE_REMAP:
    hook = TS_HTTP_READ_REQUEST_PRE_REMAP_HOOK;
    break;
  case TS_EVENT_HTTP_SEND_REQUEST_HDR:
    hook = TS_HTTP_SEND_REQUEST_HDR_HOOK;
    break;
  case TS_EVENT_HTTP_SEND_RESPONSE_HDR:
    hook = TS_HTTP_SEND_RESPONSE_HDR_HOOK;
    break;
  case TS_EVENT_HTTP_TXN_CLOSE:
    hook = TS_HTTP_TXN_CLOSE_HOOK;
    break;
  default:
    TSError("%s: unknown event for this plugin", PLUGIN_NAME);
    TSDebug(PLUGIN_NAME, "unknown event for this plugin");
    break;
  }

  if (hook != TS_HTTP_LAST_HOOK) {
    const RuleSet *rule = this->rule(hook);
    Resources res(txnp, _cont);

    // Get the resources necessary to process this event
    res.gather(resid(hook), hook);

    // Evaluation of all rules. This code is sort of duplicate in DoRemap as well.
    while (rule) {
      if (rule->eval(res)) {
        OperModifiers rt = rule->exec(res);

        if (rule->last() || (rt & OPER_LAST)) {
          break; // Conditional break, force a break with [L]
        }
      }
      rule = rule->next;
    }
  }

  return 0;
}

static int
holder_rewrite_headers(TSCont contp, TSEvent event, void *edata)
{
  TSHttpTxn txnp = static_cast<TSHttpTxn>(edata);
  RulesConfig *conf = static_cast<RulesConfig *>(ConfigHolder::get_config(contp));

  conf->rewrite_headers(event, txnp);

  TSHttpTxnReenable(txnp, TS_EVENT_HTTP_CONTINUE);
  return 0;
}

///////////////////////////////////////////////////////////////////////////////
// Initialize the InkAPI plugin for the global hooks we support.
//
void
TSPluginInit(int argc, const char *argv[])
{
  ConfigHolder *config_holder;
  TSPluginRegistrationInfo info;
  const char *path = NULL;

  info.plugin_name = (char *)PLUGIN_NAME;
  info.vendor_name = (char *)"Apache Software Foundation";
  info.support_email = (char *)"dev@trafficserver.apache.org";

  if (TS_SUCCESS != TSPluginRegister(TS_SDK_VERSION_3_0, &info)) {
    TSError("%s: plugin registration failed.\n", PLUGIN_NAME);
  }

  // Parse the global config file(s). All rules are just appended
  // to the "global" Rules configuration.
  RulesConfig *conf = new RulesConfig(TS_HTTP_READ_RESPONSE_HDR_HOOK);

  config_holder = new ConfigHolder(conf, DEFAULT_CONFIG_NAME, PLUGIN_NAME);
  if (1 < argc) {
    // Parse the config file. jlaue - reduced to single config file
    path = argv[1];
    TSDebug(PLUGIN_NAME, "Loading global configuration file %s", path);
  }

  if (!path) {
    delete config_holder;
    return;
  }

  TSCont contp = TSContCreate(holder_rewrite_headers, NULL);
  TSContDataSet(contp, config_holder);
  conf->continuation(contp);

  config_holder->init(path);
  config_holder->addUpdateRegister();
}

///////////////////////////////////////////////////////////////////////////////
// Initialize the plugin as a remap plugin.
//
TSReturnCode
TSRemapInit(TSRemapInterface *api_info, char *errbuf, int errbuf_size)
{
  if (!api_info) {
    strncpy(errbuf, "[TSRemapInit] - Invalid TSRemapInterface argument", errbuf_size - 1);
    return TS_ERROR;
  }

  if (api_info->size < sizeof(TSRemapInterface)) {
    strncpy(errbuf, "[TSRemapInit] - Incorrect size of TSRemapInterface structure", errbuf_size - 1);
    return TS_ERROR;
  }

  if (api_info->tsremap_version < TSREMAP_VERSION) {
    snprintf(errbuf, errbuf_size - 1, "[TSRemapInit] - Incorrect API version %ld.%ld", api_info->tsremap_version >> 16,
             (api_info->tsremap_version & 0xffff));
    return TS_ERROR;
  }

  TSDebug(PLUGIN_NAME, "Remap plugin is successfully initialized");
  return TS_SUCCESS;
}

TSReturnCode
TSRemapNewInstance(int argc, char *argv[], void **ih, char * /* errbuf ATS_UNUSED */, int /* errbuf_size ATS_UNUSED */)
{
  TSDebug(PLUGIN_NAME, "Instantiating a new remap.config plugin rule");

  if (argc < 3) {
    TSError("%s: Unable to create remap instance, need config file", PLUGIN_NAME);
    return TS_ERROR;
  }

  RulesConfig *conf = new RulesConfig(TS_REMAP_PSEUDO_HOOK);

  ConfigHolder *config_holder;
  config_holder = new ConfigHolder(conf, DEFAULT_CONFIG_NAME, PLUGIN_NAME);
  TSCont contp = TSContCreate(holder_rewrite_headers, NULL);
  TSContDataSet(contp, config_holder);
  conf->continuation(contp);

  if (argc < 4) { // jlaue: config reload is only supported with 1 top level config

    char *path = 0;
    if (argc > 2) {
      // Parse the config file. jlaue - reduced to single config file
      path = argv[2];
    }
    TSDebug(PLUGIN_NAME, "Loading reloadable configuration file %s", path);

    if (!path) {
      delete config_holder;
      return TS_ERROR;
    }

    config_holder->init(path);
    config_holder->addUpdateRegister();

  } else {
    for (int i = 2; i < argc; ++i) {
      TSDebug(PLUGIN_NAME, "Loading remap configuration file %s", argv[i]);
      if (!conf->parse_config(argv[i])) {
        TSError("%s: Unable to create remap instance", PLUGIN_NAME);
        return TS_ERROR;
      } else {
        TSDebug(PLUGIN_NAME, "Successfully loaded remap config file %s", argv[i]);
      }
    }

    // For debugging only
    if (TSIsDebugTagSet(PLUGIN_NAME)) {
      for (int i = TS_HTTP_READ_REQUEST_HDR_HOOK; i < TS_HTTP_LAST_HOOK; ++i) {
        if (conf->rule(i)) {
          TSDebug(PLUGIN_NAME, "Adding remap ruleset to hook=%s", TSHttpHookNameLookup((TSHttpHookID)i));
        }
      }
    }
    config_holder->config = conf;
  }

  *ih = static_cast<void *>(config_holder);

  return TS_SUCCESS;
}

void
TSRemapDeleteInstance(void *ih)
{
  ConfigHolder *config_holder = static_cast<ConfigHolder *>(ih);
  RulesConfig *conf = static_cast<RulesConfig *>(config_holder->config);
  TSContDestroy(conf->continuation());
  config_holder->removeUpdateRegister();
  delete config_holder;
}

///////////////////////////////////////////////////////////////////////////////
// This is the main "entry" point for the plugin, called for every request.
//
TSRemapStatus
TSRemapDoRemap(void *ih, TSHttpTxn rh, TSRemapRequestInfo *rri)
{
  // Make sure things are properly setup (this should never happen)
  if (NULL == ih) {
    TSDebug(PLUGIN_NAME, "No Rules configured, falling back to default");
    return TSREMAP_NO_REMAP;
  }

  TSRemapStatus rval = TSREMAP_NO_REMAP;
  ConfigHolder *config_holder = static_cast<ConfigHolder *>(ih);
  RulesConfig *conf = static_cast<RulesConfig *>(config_holder->config);

  // Go through all hooks we support, and setup the txn hook(s) as necessary
  for (int i = TS_HTTP_READ_REQUEST_HDR_HOOK; i < TS_HTTP_LAST_HOOK; ++i) {
    if (conf->rule(i)) {
      TSHttpTxnHookAdd(rh, static_cast<TSHttpHookID>(i), conf->continuation());
      TSDebug(PLUGIN_NAME, "Added remapped TXN hook=%s", TSHttpHookNameLookup((TSHttpHookID)i));
    }
  }

  // Now handle the remap specific rules for the "remap hook" (which is not a real hook).
  // This is sufficiently different than the normal cont_rewrite_headers() callback, and
  // we can't (shouldn't) schedule this as a TXN hook.
  RuleSet *rule = conf->rule(TS_REMAP_PSEUDO_HOOK);
  Resources res(rh, rri);

  res.gather(RSRC_CLIENT_REQUEST_HEADERS, TS_REMAP_PSEUDO_HOOK);
  while (rule) {
    if (rule->eval(res)) {
      OperModifiers rt = rule->exec(res);

      if (res.changed_url == true) {
        rval = TSREMAP_DID_REMAP;
      }

      if (rule->last() || (rt & OPER_LAST)) {
        break; // Conditional break, force a break with [L]
      }
    }
    rule = rule->next;
  }

  TSDebug(PLUGIN_NAME_DBG, "Returning from TSRemapDoRemap with status: %d", rval);
  return rval;
}
