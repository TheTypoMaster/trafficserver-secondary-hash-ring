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

#include "I_NetVConnection.h"

TS_INLINE sockaddr const *
NetVConnection::get_remote_addr()
{
  if (!got_remote_addr) {
    set_remote_addr();
    got_remote_addr = true;
  }
  return &remote_addr.sa;
}

TS_INLINE in_addr_t
NetVConnection::get_remote_ip()
{
  sockaddr const *addr = this->get_remote_addr();
  return ats_is_ip4(addr) ? ats_ip4_addr_cast(addr) : 0;
}


/// @return The remote port in host order.
TS_INLINE uint16_t
NetVConnection::get_remote_port()
{
  return ats_ip_port_host_order(this->get_remote_addr());
}

TS_INLINE sockaddr const *
NetVConnection::get_local_addr()
{
  if (!got_local_addr) {
    set_local_addr();
    if ((ats_is_ip(&local_addr) && ats_ip_port_cast(&local_addr))                    // IP and has a port.
        || (ats_is_ip4(&local_addr) && INADDR_ANY != ats_ip4_addr_cast(&local_addr)) // IPv4
        || (ats_is_ip6(&local_addr) && !IN6_IS_ADDR_UNSPECIFIED(&local_addr.sin6.sin6_addr))) {
      got_local_addr = 1;
    }
  }
  return &local_addr.sa;
}

TS_INLINE in_addr_t
NetVConnection::get_local_ip()
{
  sockaddr const *addr = this->get_local_addr();
  return ats_is_ip4(addr) ? ats_ip4_addr_cast(addr) : 0;
}

/// @return The local port in host order.
TS_INLINE uint16_t
NetVConnection::get_local_port()
{
  return ats_ip_port_host_order(this->get_local_addr());
}