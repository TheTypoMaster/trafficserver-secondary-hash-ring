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

#include "P_Net.h"

ink_hrtime last_throttle_warning;
ink_hrtime last_shedding_warning;
ink_hrtime emergency_throttle_time;
int net_connections_throttle;
int fds_throttle;
int fds_limit = 8000;
ink_hrtime last_transient_accept_error;

extern "C" void fd_reify(struct ev_loop *);


#ifndef INACTIVITY_TIMEOUT
int update_cop_config(const char *name, RecDataT data_type, RecData data, void *cookie);

// INKqa10496
// One Inactivity cop runs on each thread once every second and
// loops through the list of NetVCs and calls the timeouts
class InactivityCop : public Continuation
{
public:
  InactivityCop(ProxyMutex *m)
    : Continuation(m), default_inactivity_timeout(0), total_connections_in(0), max_connections_in(0), connections_per_thread_in(0)
  {
    SET_HANDLER(&InactivityCop::check_inactivity);
    REC_ReadConfigInteger(default_inactivity_timeout, "proxy.config.net.default_inactivity_timeout");
    Debug("inactivity_cop", "default inactivity timeout is set to: %d", default_inactivity_timeout);
    REC_ReadConfigInt32(max_connections_in, "proxy.config.net.max_connections_in");

    RecRegisterConfigUpdateCb("proxy.config.net.max_connections_in", update_cop_config, (void *)this);
    RecRegisterConfigUpdateCb("proxy.config.net.default_inactivity_timeout", update_cop_config, (void *)this);
  }

  int
  check_inactivity(int event, Event *e)
  {
    (void)event;
    ink_hrtime now = ink_get_hrtime();
    NetHandler &nh = *get_NetHandler(this_ethread());
    total_connections_in = 0;
    // Copy the list and use pop() to catch any closes caused by callbacks.
    forl_LL(UnixNetVConnection, vc, nh.open_list)
    {
      if (vc->thread == this_ethread()) {
        if (vc->from_accept_thread == true) {
          ++total_connections_in;
        }
        nh.cop_list.push(vc);
      }
    }
    while (UnixNetVConnection *vc = nh.cop_list.pop()) {
      // If we cannot get the lock don't stop just keep cleaning
      MUTEX_TRY_LOCK(lock, vc->mutex, this_ethread());
      if (!lock.is_locked()) {
        NET_INCREMENT_DYN_STAT(inactivity_cop_lock_acquire_failure_stat);
        continue;
      }

      if (vc->closed) {
        close_UnixNetVConnection(vc, e->ethread);
        continue;
      }

      // set a default inactivity timeout if one is not set
      if (vc->next_inactivity_timeout_at == 0 && default_inactivity_timeout > 0) {
        Debug("inactivity_cop", "vc: %p inactivity timeout not set, setting a default of %d", vc, default_inactivity_timeout);
        vc->set_inactivity_timeout(HRTIME_SECONDS(default_inactivity_timeout));
        NET_INCREMENT_DYN_STAT(default_inactivity_timeout_stat);
      } else {
        Debug("inactivity_cop_verbose", "vc: %p now: %" PRId64 " timeout at: %" PRId64 " timeout in: %" PRId64, vc, now,
              ink_hrtime_to_sec(vc->next_inactivity_timeout_at), ink_hrtime_to_sec(vc->inactivity_timeout_in));
      }

      if (vc->next_inactivity_timeout_at && vc->next_inactivity_timeout_at < now) {
        if (nh.keep_alive_list.in(vc)) {
          // only stat if the connection is in keep-alive, there can be other inactivity timeouts
          ink_hrtime diff = (now - (vc->next_inactivity_timeout_at - vc->inactivity_timeout_in)) / HRTIME_SECOND;
          NET_SUM_DYN_STAT(keep_alive_lru_timeout_total_stat, diff);
          NET_INCREMENT_DYN_STAT(keep_alive_lru_timeout_count_stat);
        }
        Debug("inactivity_cop_verbose", "vc: %p now: %" PRId64 " timeout at: %" PRId64 " timeout in: %" PRId64, vc, now,
              vc->next_inactivity_timeout_at, vc->inactivity_timeout_in);
        vc->handleEvent(EVENT_IMMEDIATE, e);
      }
    }

    // Keep-alive LRU for incoming connections
    keep_alive_lru(nh, now, e);

    return 0;
  }

  void
  set_max_connections(const int32_t x)
  {
    max_connections_in = x;
  }
  void
  set_connections_per_thread(const int32_t x)
  {
    connections_per_thread_in = x;
  }
  void
  set_default_timeout(const int x)
  {
    default_inactivity_timeout = x;
  }

private:
  void keep_alive_lru(NetHandler &nh, ink_hrtime now, Event *e);
  int default_inactivity_timeout; // only used when one is not set for some bad reason
  int32_t total_connections_in;
  int32_t max_connections_in;
  int32_t connections_per_thread_in;
};

int
update_cop_config(const char *name, RecDataT data_type ATS_UNUSED, RecData data, void *cookie)
{
  InactivityCop *cop = static_cast<InactivityCop *>(cookie);
  ink_assert(cop != NULL);

  if (cop != NULL) {
    if (strcmp(name, "proxy.config.net.max_connections_in") == 0) {
      Debug("inactivity_cop_dynamic", "proxy.config.net.max_connections_in updated to %" PRId64, data.rec_int);
      cop->set_max_connections(data.rec_int);
      cop->set_connections_per_thread(0);
    }

    if (strcmp(name, "proxy.config.net.default_inactivity_timeout") == 0) {
      Debug("inactivity_cop_dynamic", "proxy.config.net.default_inactivity_timeout updated to %" PRId64, data.rec_int);
      cop->set_default_timeout(data.rec_int);
    }
  }

  return REC_ERR_OKAY;
}

void
InactivityCop::keep_alive_lru(NetHandler &nh, const ink_hrtime now, Event *e)
{
  // maximum incoming connections is set to 0 then the feature is disabled
  if (max_connections_in == 0) {
    return;
  }

  if (connections_per_thread_in == 0) {
    // figure out the number of threads and calculate the number of connections per thread
    const int event_threads = eventProcessor.n_threads_for_type[ET_NET];
    const int ssl_threads = (ET_NET == SSLNetProcessor::ET_SSL) ? 0 : eventProcessor.n_threads_for_type[SSLNetProcessor::ET_SSL];
    connections_per_thread_in = max_connections_in / (event_threads + ssl_threads);
  }

  // calculate how many connections to close
  int32_t to_process = total_connections_in - connections_per_thread_in;
  if (to_process <= 0) {
    return;
  }
  to_process = min((int32_t)nh.keep_alive_lru_size, to_process);

  Debug("inactivity_cop_dynamic", "max cons: %d active: %d idle: %d process: %d"
                                  " net type: %d ssl type: %d",
        connections_per_thread_in, total_connections_in - nh.keep_alive_lru_size, nh.keep_alive_lru_size, to_process, ET_NET,
        SSLNetProcessor::ET_SSL);

  // loop over the non-active connections and try to close them
  UnixNetVConnection *vc = nh.keep_alive_list.head;
  UnixNetVConnection *vc_next = NULL;
  int closed = 0;
  int handle_event = 0;
  int total_idle_time = 0;
  int total_idle_count = 0;
  for (int32_t i = 0; i < to_process && vc != NULL; ++i, vc = vc_next) {
    vc_next = vc->keep_alive_link.next;
    if (vc->thread != this_ethread()) {
      continue;
    }
    MUTEX_TRY_LOCK(lock, vc->mutex, this_ethread());
    if (!lock.is_locked()) {
      continue;
    }
    ink_hrtime diff = (now - (vc->next_inactivity_timeout_at - vc->inactivity_timeout_in)) / HRTIME_SECOND;
    if (diff > 0) {
      total_idle_time += diff;
      ++total_idle_count;
      NET_SUM_DYN_STAT(keep_alive_lru_timeout_total_stat, diff);
      NET_INCREMENT_DYN_STAT(keep_alive_lru_timeout_count_stat);
    }
    Debug("inactivity_cop_dynamic",
          "closing connection NetVC=%p idle: %u now: %" PRId64 " at: %" PRId64 " in: %" PRId64 " diff: %" PRId64, vc,
          nh.keep_alive_lru_size, ink_hrtime_to_sec(now), ink_hrtime_to_sec(vc->next_inactivity_timeout_at),
          ink_hrtime_to_sec(vc->inactivity_timeout_in), diff);
    if (vc->closed) {
      close_UnixNetVConnection(vc, e->ethread);
      ++closed;
    } else {
      vc->next_inactivity_timeout_at = now;
      nh.keep_alive_list.head->handleEvent(EVENT_IMMEDIATE, e);
      ++handle_event;
    }
  }

  if (total_idle_count > 0) {
    Debug("inactivity_cop_dynamic", "max cons: %d active: %d idle: %d already closed: %d, close event: %d"
                                    " mean idle: %d\n",
          connections_per_thread_in, total_connections_in - nh.keep_alive_lru_size - closed - handle_event, nh.keep_alive_lru_size,
          closed, handle_event, total_idle_time / total_idle_count);
  }
}
#endif

PollCont::PollCont(ProxyMutex *m, int pt) : Continuation(m), net_handler(NULL), nextPollDescriptor(NULL), poll_timeout(pt)
{
  pollDescriptor = new PollDescriptor;
  pollDescriptor->init();
  SET_HANDLER(&PollCont::pollEvent);
}

PollCont::PollCont(ProxyMutex *m, NetHandler *nh, int pt)
  : Continuation(m), net_handler(nh), nextPollDescriptor(NULL), poll_timeout(pt)
{
  pollDescriptor = new PollDescriptor;
  pollDescriptor->init();
  SET_HANDLER(&PollCont::pollEvent);
}

PollCont::~PollCont()
{
  delete pollDescriptor;
  if (nextPollDescriptor != NULL) {
    delete nextPollDescriptor;
  }
}

//
// PollCont continuation which does the epoll_wait
// and stores the resultant events in ePoll_Triggered_Events
//
int
PollCont::pollEvent(int event, Event *e)
{
  (void)event;
  (void)e;

  if (likely(net_handler)) {
    /* checking to see whether there are connections on the ready_queue (either read or write) that need processing [ebalsa] */
    if (likely(!net_handler->read_ready_list.empty() || !net_handler->write_ready_list.empty() ||
               !net_handler->read_enable_list.empty() || !net_handler->write_enable_list.empty())) {
      NetDebug("iocore_net_poll", "rrq: %d, wrq: %d, rel: %d, wel: %d", net_handler->read_ready_list.empty(),
               net_handler->write_ready_list.empty(), net_handler->read_enable_list.empty(),
               net_handler->write_enable_list.empty());
      poll_timeout = 0; // poll immediately returns -- we have triggered stuff to process right now
    } else {
      poll_timeout = net_config_poll_timeout;
    }
  }
// wait for fd's to tigger, or don't wait if timeout is 0
#if TS_USE_EPOLL
  pollDescriptor->result =
    epoll_wait(pollDescriptor->epoll_fd, pollDescriptor->ePoll_Triggered_Events, POLL_DESCRIPTOR_SIZE, poll_timeout);
  NetDebug("iocore_net_poll", "[PollCont::pollEvent] epoll_fd: %d, timeout: %d, results: %d", pollDescriptor->epoll_fd,
           poll_timeout, pollDescriptor->result);
#elif TS_USE_KQUEUE
  struct timespec tv;
  tv.tv_sec = poll_timeout / 1000;
  tv.tv_nsec = 1000000 * (poll_timeout % 1000);
  pollDescriptor->result =
    kevent(pollDescriptor->kqueue_fd, NULL, 0, pollDescriptor->kq_Triggered_Events, POLL_DESCRIPTOR_SIZE, &tv);
  NetDebug("iocore_net_poll", "[PollCont::pollEvent] kueue_fd: %d, timeout: %d, results: %d", pollDescriptor->kqueue_fd,
           poll_timeout, pollDescriptor->result);
#elif TS_USE_PORT
  int retval;
  timespec_t ptimeout;
  ptimeout.tv_sec = poll_timeout / 1000;
  ptimeout.tv_nsec = 1000000 * (poll_timeout % 1000);
  unsigned nget = 1;
  if ((retval = port_getn(pollDescriptor->port_fd, pollDescriptor->Port_Triggered_Events, POLL_DESCRIPTOR_SIZE, &nget, &ptimeout)) <
      0) {
    pollDescriptor->result = 0;
    switch (errno) {
    case EINTR:
    case EAGAIN:
    case ETIME:
      if (nget > 0) {
        pollDescriptor->result = (int)nget;
      }
      break;
    default:
      ink_assert(!"unhandled port_getn() case:");
      break;
    }
  } else {
    pollDescriptor->result = (int)nget;
  }
  NetDebug("iocore_net_poll", "[PollCont::pollEvent] %d[%s]=port_getn(%d,%p,%d,%d,%d),results(%d)", retval,
           retval < 0 ? strerror(errno) : "ok", pollDescriptor->port_fd, pollDescriptor->Port_Triggered_Events,
           POLL_DESCRIPTOR_SIZE, nget, poll_timeout, pollDescriptor->result);
#else
#error port me
#endif
  return EVENT_CONT;
}

static void
net_signal_hook_callback(EThread *thread)
{
#if HAVE_EVENTFD
  uint64_t counter;
  ATS_UNUSED_RETURN(read(thread->evfd, &counter, sizeof(uint64_t)));
#elif TS_USE_PORT
/* Nothing to drain or do */
#else
  char dummy[1024];
  ATS_UNUSED_RETURN(read(thread->evpipe[0], &dummy[0], 1024));
#endif
}

static void
net_signal_hook_function(EThread *thread)
{
#if HAVE_EVENTFD
  uint64_t counter = 1;
  ATS_UNUSED_RETURN(write(thread->evfd, &counter, sizeof(uint64_t)));
#elif TS_USE_PORT
  PollDescriptor *pd = get_PollDescriptor(thread);
  ATS_UNUSED_RETURN(port_send(pd->port_fd, 0, thread->ep));
#else
  char dummy = 1;
  ATS_UNUSED_RETURN(write(thread->evpipe[1], &dummy, 1));
#endif
}

void
initialize_thread_for_net(EThread *thread)
{
  new ((ink_dummy_for_new *)get_NetHandler(thread)) NetHandler();
  new ((ink_dummy_for_new *)get_PollCont(thread)) PollCont(thread->mutex, get_NetHandler(thread));
  get_NetHandler(thread)->mutex = new_ProxyMutex();
  PollCont *pc = get_PollCont(thread);
  PollDescriptor *pd = pc->pollDescriptor;

  thread->schedule_imm(get_NetHandler(thread));

#ifndef INACTIVITY_TIMEOUT
  InactivityCop *inactivityCop = new InactivityCop(get_NetHandler(thread)->mutex);
  thread->schedule_every(inactivityCop, HRTIME_SECONDS(1));
#endif

  thread->signal_hook = net_signal_hook_function;
  thread->ep = (EventIO *)ats_malloc(sizeof(EventIO));
  thread->ep->type = EVENTIO_ASYNC_SIGNAL;
#if HAVE_EVENTFD
  thread->ep->start(pd, thread->evfd, 0, EVENTIO_READ);
#else
  thread->ep->start(pd, thread->evpipe[0], 0, EVENTIO_READ);
#endif
}

// NetHandler method definitions

NetHandler::NetHandler() : Continuation(NULL), trigger_event(0), keep_alive_lru_size(0)
{
  SET_HANDLER((NetContHandler)&NetHandler::startNetEvent);
}

//
// Initialization here, in the thread in which we will be executing
// from now on.
//
int
NetHandler::startNetEvent(int event, Event *e)
{
  (void)event;
  SET_HANDLER((NetContHandler)&NetHandler::mainNetEvent);
  e->schedule_every(NET_PERIOD);
  trigger_event = e;
  return EVENT_CONT;
}

//
// Move VC's enabled on a different thread to the ready list
//
void
NetHandler::process_enabled_list(NetHandler *nh)
{
  UnixNetVConnection *vc = NULL;

  SListM(UnixNetVConnection, NetState, read, enable_link) rq(nh->read_enable_list.popall());
  while ((vc = rq.pop())) {
    vc->ep.modify(EVENTIO_READ);
    vc->ep.refresh(EVENTIO_READ);
    vc->read.in_enabled_list = 0;
    if ((vc->read.enabled && vc->read.triggered) || vc->closed)
      nh->read_ready_list.in_or_enqueue(vc);
  }

  SListM(UnixNetVConnection, NetState, write, enable_link) wq(nh->write_enable_list.popall());
  while ((vc = wq.pop())) {
    vc->ep.modify(EVENTIO_WRITE);
    vc->ep.refresh(EVENTIO_WRITE);
    vc->write.in_enabled_list = 0;
    if ((vc->write.enabled && vc->write.triggered) || vc->closed)
      nh->write_ready_list.in_or_enqueue(vc);
  }
}


//
// The main event for NetHandler
// This is called every NET_PERIOD, and handles all IO operations scheduled
// for this period.
//
int
NetHandler::mainNetEvent(int event, Event *e)
{
  ink_assert(trigger_event == e && (event == EVENT_INTERVAL || event == EVENT_POLL));
  (void)event;
  (void)e;
  EventIO *epd = NULL;
  int poll_timeout;

  NET_INCREMENT_DYN_STAT(net_handler_run_stat);

  process_enabled_list(this);
  if (likely(!read_ready_list.empty() || !write_ready_list.empty() || !read_enable_list.empty() || !write_enable_list.empty()))
    poll_timeout = 0; // poll immediately returns -- we have triggered stuff to process right now
  else
    poll_timeout = net_config_poll_timeout;

  PollDescriptor *pd = get_PollDescriptor(trigger_event->ethread);
  UnixNetVConnection *vc = NULL;
#if TS_USE_EPOLL
  pd->result = epoll_wait(pd->epoll_fd, pd->ePoll_Triggered_Events, POLL_DESCRIPTOR_SIZE, poll_timeout);
  NetDebug("iocore_net_main_poll", "[NetHandler::mainNetEvent] epoll_wait(%d,%d), result=%d", pd->epoll_fd, poll_timeout,
           pd->result);
#elif TS_USE_KQUEUE
  struct timespec tv;
  tv.tv_sec = poll_timeout / 1000;
  tv.tv_nsec = 1000000 * (poll_timeout % 1000);
  pd->result = kevent(pd->kqueue_fd, NULL, 0, pd->kq_Triggered_Events, POLL_DESCRIPTOR_SIZE, &tv);
  NetDebug("iocore_net_main_poll", "[NetHandler::mainNetEvent] kevent(%d,%d), result=%d", pd->kqueue_fd, poll_timeout, pd->result);
#elif TS_USE_PORT
  int retval;
  timespec_t ptimeout;
  ptimeout.tv_sec = poll_timeout / 1000;
  ptimeout.tv_nsec = 1000000 * (poll_timeout % 1000);
  unsigned nget = 1;
  if ((retval = port_getn(pd->port_fd, pd->Port_Triggered_Events, POLL_DESCRIPTOR_SIZE, &nget, &ptimeout)) < 0) {
    pd->result = 0;
    switch (errno) {
    case EINTR:
    case EAGAIN:
    case ETIME:
      if (nget > 0) {
        pd->result = (int)nget;
      }
      break;
    default:
      ink_assert(!"unhandled port_getn() case:");
      break;
    }
  } else {
    pd->result = (int)nget;
  }
  NetDebug("iocore_net_main_poll", "[NetHandler::mainNetEvent] %d[%s]=port_getn(%d,%p,%d,%d,%d),results(%d)", retval,
           retval < 0 ? strerror(errno) : "ok", pd->port_fd, pd->Port_Triggered_Events, POLL_DESCRIPTOR_SIZE, nget, poll_timeout,
           pd->result);

#else
#error port me
#endif

  vc = NULL;
  for (int x = 0; x < pd->result; x++) {
    epd = (EventIO *)get_ev_data(pd, x);
    if (epd->type == EVENTIO_READWRITE_VC) {
      vc = epd->data.vc;
      if (get_ev_events(pd, x) & (EVENTIO_READ | EVENTIO_ERROR)) {
        vc->read.triggered = 1;
        if (!read_ready_list.in(vc))
          read_ready_list.enqueue(vc);
        else if (get_ev_events(pd, x) & EVENTIO_ERROR) {
          // check for unhandled epoll events that should be handled
          Debug("iocore_net_main", "Unhandled epoll event on read: 0x%04x read.enabled=%d closed=%d read.netready_queue=%d",
                get_ev_events(pd, x), vc->read.enabled, vc->closed, read_ready_list.in(vc));
        }
      }
      vc = epd->data.vc;
      if (get_ev_events(pd, x) & (EVENTIO_WRITE | EVENTIO_ERROR)) {
        vc->write.triggered = 1;
        if (!write_ready_list.in(vc))
          write_ready_list.enqueue(vc);
        else if (get_ev_events(pd, x) & EVENTIO_ERROR) {
          // check for unhandled epoll events that should be handled
          Debug("iocore_net_main", "Unhandled epoll event on write: 0x%04x write.enabled=%d closed=%d write.netready_queue=%d",
                get_ev_events(pd, x), vc->write.enabled, vc->closed, write_ready_list.in(vc));
        }
      } else if (!(get_ev_events(pd, x) & EVENTIO_ERROR)) {
        Debug("iocore_net_main", "Unhandled epoll event: 0x%04x", get_ev_events(pd, x));
      }
    } else if (epd->type == EVENTIO_DNS_CONNECTION) {
      if (epd->data.dnscon != NULL) {
        epd->data.dnscon->trigger(); // Make sure the DNSHandler for this con knows we triggered
#if defined(USE_EDGE_TRIGGER)
        epd->refresh(EVENTIO_READ);
#endif
      }
    } else if (epd->type == EVENTIO_ASYNC_SIGNAL) {
      net_signal_hook_callback(trigger_event->ethread);
    }
    ev_next_event(pd, x);
  }

  pd->result = 0;

#if defined(USE_EDGE_TRIGGER)
  // UnixNetVConnection *
  while ((vc = read_ready_list.dequeue())) {
    if (vc->closed)
      close_UnixNetVConnection(vc, trigger_event->ethread);
    else if (vc->read.enabled && vc->read.triggered)
      vc->net_read_io(this, trigger_event->ethread);
    else if (!vc->read.enabled) {
      read_ready_list.remove(vc);
#if defined(solaris)
      if (vc->read.triggered && vc->write.enabled) {
        vc->ep.modify(-EVENTIO_READ);
        vc->ep.refresh(EVENTIO_WRITE);
        vc->writeReschedule(this);
      }
#endif
    }
  }
  while ((vc = write_ready_list.dequeue())) {
    if (vc->closed)
      close_UnixNetVConnection(vc, trigger_event->ethread);
    else if (vc->write.enabled && vc->write.triggered)
      write_to_net(this, vc, trigger_event->ethread);
    else if (!vc->write.enabled) {
      write_ready_list.remove(vc);
#if defined(solaris)
      if (vc->write.triggered && vc->read.enabled) {
        vc->ep.modify(-EVENTIO_WRITE);
        vc->ep.refresh(EVENTIO_READ);
        vc->readReschedule(this);
      }
#endif
    }
  }
#else  /* !USE_EDGE_TRIGGER */
  while ((vc = read_ready_list.dequeue())) {
    if (vc->closed)
      close_UnixNetVConnection(vc, trigger_event->ethread);
    else if (vc->read.enabled && vc->read.triggered)
      vc->net_read_io(this, trigger_event->ethread);
    else if (!vc->read.enabled)
      vc->ep.modify(-EVENTIO_READ);
  }
  while ((vc = write_ready_list.dequeue())) {
    if (vc->closed)
      close_UnixNetVConnection(vc, trigger_event->ethread);
    else if (vc->write.enabled && vc->write.triggered)
      write_to_net(this, vc, trigger_event->ethread);
    else if (!vc->write.enabled)
      vc->ep.modify(-EVENTIO_WRITE);
  }
#endif /* !USE_EDGE_TRIGGER */

  return EVENT_CONT;
}