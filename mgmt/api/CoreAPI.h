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


#ifndef _CORE_API_H
#define _CORE_API_H

#include <stdarg.h> // for va_list

#include "ink_llqueue.h"
#include "MgmtDefs.h" // MgmtInt, MgmtFloat, etc

#include "mgmtapi.h"
#include "CfgContextDefs.h"
#include "Tokenizer.h"

TSMgmtError Init(const char *socket_path = NULL, TSInitOptionT options = TS_MGMT_OPT_DEFAULTS);
TSMgmtError Terminate();

void DiagnosticMessage(TSDiagsT mode, const char *fmt, va_list ap);

/***************************************************************************
 * Control Operations
 ***************************************************************************/
TSProxyStateT ProxyStateGet();
TSMgmtError ProxyStateSet(TSProxyStateT state, TSCacheClearT clear);
TSMgmtError ServerBacktrace(unsigned options, char **trace);

TSMgmtError Reconfigure();                            // TS reread config files
TSMgmtError Restart(unsigned options);                // restart TM
TSMgmtError Bounce(unsigned options);                 // restart traffic_server
TSMgmtError StorageDeviceCmdOffline(const char *dev); // Storage device operation.

/***************************************************************************
 * Record Operations
 ***************************************************************************/
/* For remote implementation of this interface, these functions will have
   to marshal/unmarshal and send request across the network */
TSMgmtError MgmtRecordGet(const char *rec_name, TSRecordEle *rec_ele);

TSMgmtError MgmtRecordSet(const char *rec_name, const char *val, TSActionNeedT *action_need);
TSMgmtError MgmtRecordSetInt(const char *rec_name, MgmtInt int_val, TSActionNeedT *action_need);
TSMgmtError MgmtRecordSetCounter(const char *rec_name, MgmtIntCounter counter_val, TSActionNeedT *action_need);
TSMgmtError MgmtRecordSetFloat(const char *rec_name, MgmtFloat float_val, TSActionNeedT *action_need);
TSMgmtError MgmtRecordSetString(const char *rec_name, const char *string_val, TSActionNeedT *action_need);
TSMgmtError MgmtRecordGetMatching(const char *regex, TSList rec_vals);

TSMgmtError MgmtConfigRecordDescribe(const char *rec_name, unsigned flags, TSConfigRecordDescription *val);

/***************************************************************************
 * File Operations
 ***************************************************************************/
TSMgmtError ReadFile(TSFileNameT file, char **text, int *size, int *version);
TSMgmtError WriteFile(TSFileNameT file, const char *text, int size, int version);

/***************************************************************************
 * Events
 ***************************************************************************/

TSMgmtError EventSignal(const char *event_name, va_list ap);
TSMgmtError EventResolve(const char *event_name);
TSMgmtError ActiveEventGetMlt(LLQ *active_events);
TSMgmtError EventIsActive(const char *event_name, bool *is_current);
TSMgmtError EventSignalCbRegister(const char *event_name, TSEventSignalFunc func, void *data);
TSMgmtError EventSignalCbUnregister(const char *event_name, TSEventSignalFunc func);

/***************************************************************************
 * Snapshots
 ***************************************************************************/
TSMgmtError SnapshotTake(const char *snapshot_name);
TSMgmtError SnapshotRestore(const char *snapshot_name);
TSMgmtError SnapshotRemove(const char *snapshot_name);
TSMgmtError SnapshotGetMlt(LLQ *snapshots);

TSMgmtError StatsReset(bool cluster, const char *name = NULL);

#endif