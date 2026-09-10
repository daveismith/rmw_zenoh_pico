/*
 * Copyright(C) 2024 eSOL Co., Ltd.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef RMW_ZENOH_PICO_OPTIONS_H
#define RMW_ZENOH_PICO_OPTIONS_H

#include <stdbool.h>
#include <rmw_zenoh_pico/config.h>   /* RMW_ZENOH_PICO_TRANSPORT_* */
#include <zenoh-pico/system/platform.h>

#define _Z_LOG_LVL_NONE  0
#define _Z_LOG_LVL_ERROR 1
#define _Z_LOG_LVL_INFO  2
#define _Z_LOG_LVL_DEBUG 3

#define RMW_ZENOH_SERIAL_MODE "serial"
#define RMW_ZENOH_CLIENT_MODE "client"
#define RMW_ZENOH_PEER_MODE   "peer"

#if defined(__cplusplus)
extern "C"
{
#endif  // if defined(__cplusplus)

  extern void rmw_zenoh_pico_set_mode(const char *mode);
  extern const char *rmw_zenoh_pico_get_mode();

#if defined(RMW_ZENOH_PICO_TRANSPORT_UNICAST)
  extern void rmw_zenoh_pico_set_unicast(const char *connect,
					 const char *connect_port,
					 const char *lieten,
					 const char *listen_port);
#elif defined (RMW_ZENOH_PICO_TRANSPORT_MULTICAST)
  extern void rmw_zenoh_pico_set_mcast(const char *mcast,
				       const char *mcast_port,
				       const char *mcast_dev);
#elif defined (RMW_ZENOH_PICO_TRANSPORT_SERIAL)
  extern void rmw_zenoh_pico_set_serial_device(const char *device);
#endif

  extern int rmw_zenoh_pico_debug_level_get(void);
  extern int rmw_zenoh_pico_debug_level_set(int);

  /* Attributes for zenoh-pico's background task(s). Since zenoh-pico 1.7 one
   * executor task does the reading, the subscription callbacks, the lease and
   * reconnects, and `read_attr` is what it gets; `lease_attr` is kept for older
   * zenoh-pico where the lease task is separate. z_task_attr_t is the platform's
   * type (on FreeRTOS: name, priority, stack depth, optional static stack). Call
   * before rclc_support_init(); NULL keeps zenoh-pico's defaults. The pointers
   * must stay valid while a session is open. */
  extern void rmw_zenoh_pico_set_task_attrs(z_task_attr_t *read_attr, z_task_attr_t *lease_attr);

  /* True when there is no open session, or the open one has lost its transport
   * (zenoh-pico's z_session_is_closed). For an application watchdog. */
  extern bool rmw_zenoh_pico_session_is_closed(void);

  /* Take the session off the network before dismantling entities: stops and joins
   * zenoh's executor (so no subscription callback is in flight, and freeing the
   * mutexes an entity's callback uses becomes safe) and clears the transport (so
   * each entity's undeclare is a no-op instead of a network send from the tearing-
   * down task). Call immediately before destroying publishers/subscriptions/
   * services and finalising the node; close the session as usual afterwards.
   * Idempotent, and harmless when no session is open. */
  extern void rmw_zenoh_pico_session_quiesce(void);

#if defined(__cplusplus)
}
#endif  // if defined(__cplusplus)

#endif
