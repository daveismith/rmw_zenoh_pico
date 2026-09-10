// Copyright 2019 Proyectos y Sistemas de Mantenimiento SL (eProsima).
// Copyright(C) 2024 eSOL Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <rmw_zenoh_pico/rmw_zenoh_pico.h>
#include <string.h>

static ZenohPicoSession _zenohSession;

/* Attributes for zenoh-pico's read and lease tasks. NULL means zenoh's own
 * defaults; an RTOS application sets them (name, priority, stack, optionally a
 * caller-provided stack buffer) before rclc_support_init(). */
static z_task_attr_t *_rmw_zenoh_pico_read_task_attr = NULL;
static z_task_attr_t *_rmw_zenoh_pico_lease_task_attr = NULL;

void rmw_zenoh_pico_set_task_attrs(z_task_attr_t *read_attr, z_task_attr_t *lease_attr)
{
  _rmw_zenoh_pico_read_task_attr = read_attr;
  _rmw_zenoh_pico_lease_task_attr = lease_attr;
}

bool rmw_zenoh_pico_session_is_closed(void)
{
  if(_zenohSession.ref <= 0 || !_zenohSession.enable_session)
    return true;

  return z_session_is_closed(z_loan(_zenohSession.session));
}

void rmw_zenoh_pico_session_quiesce(void)
{
  RMW_ZENOH_FUNC_ENTRY(NULL);

  if(_zenohSession.ref <= 0 || !_zenohSession.enable_session)
    return;

  /*
   * Call this before destroying entities, and the whole teardown goes quiet.
   *
   * Two things were wrong without it. Destroying an entity frees the mutexes its
   * zenoh callback uses, but a callback runs OUTSIDE the session mutex, so a sample
   * arriving mid-teardown could be dispatched into a context that had just been
   * freed. And each entity's undeclare was a real network send from whatever task
   * was doing the teardown -- with traffic still coming in, that send under the
   * lwIP mbox lock was enough to trip the interrupt watchdog.
   *
   * zp_session_disconnect() stops and JOINS the executor (so no callback is in
   * flight when it returns) and clears the transport (so every undeclare below
   * becomes a no-op). The router drops the declarations when the session closes
   * regardless, so nothing is lost by not sending them.
   */
  (void)zp_session_disconnect(z_loan_mut(_zenohSession.session));
}

ZenohPicoSession *zenoh_pico_generate_session(const z_loaned_config_t *config,
					      const char *enclave)
{
  RMW_ZENOH_FUNC_ENTRY(NULL);

  ZenohPicoSession *session = &_zenohSession;
  if(_zenohSession.ref > 0){
    ZenohPicoDataRefClone(session);
    return &_zenohSession;
  }

  /* Generate() zeroes the object, initialises its lock and sets ref to 1 for
   * this reference. The memset that used to follow wiped the lock (leaking one
   * pthread mutex per session) and the count; the RefClone that used to follow
   * that then took a second reference nothing ever released. */
  session = ZenohPicoDataGenerate(session);
  RMW_CHECK_FOR_NULL_WITH_MSG(
    session,
    "failed to allocate struct for the ZenohPicoSession",
    return NULL);

  z_config_clone(&session->config, config);

  if(enclave != NULL) {
    z_string_copy_from_str(&session->enclave, enclave);
  }

  session->graph_guard_condition.implementation_identifier = rmw_get_implementation_identifier();
  /* The allocated condition data, not the allocator's address: the original
   * stored the function pointer here, so every session teardown destroyed a
   * "mutex" that lived in code and faulted. */
  session->graph_guard_condition.data = zenoh_pico_guard_condition_data();
  if(session->graph_guard_condition.data == NULL) {
    z_drop(z_move(session->config));
    z_drop(z_move(session->enclave));
    ZenohPicoDataRelease(session);
    return NULL;
  }

  session->enable_session = false;

  return session;
}

bool zenoh_pico_destroy_session(ZenohPicoSession *session)
{
  RMW_ZENOH_FUNC_ENTRY(NULL);

  RMW_CHECK_ARGUMENT_FOR_NULL(session, false);

  if(ZenohPicoDataRelease(session)){

    // stop background zenoh task
    if(session->enable_session){
      zp_stop_read_task(z_loan_mut(session->session));
      zp_stop_lease_task(z_loan_mut(session->session));
    }

    zenoh_pico_destroy_guard_condition_data((ZenohPicoGuardConditionData *)session->graph_guard_condition.data);
    session->graph_guard_condition.data = NULL;

    z_drop(z_move(session->config));
    z_drop(z_move(session->enclave));
    z_drop(z_move(session->session));
    session->enable_session = false;

    /* The session object is static, so ZenohPicoDataDestroy() (which would
     * free it) is never called; drop its lock here or one pthread mutex leaks
     * per session. The next generate re-initialises it. */
    z_mutex_drop(z_move(session->lock));
    session->ref = 0;
  }

  return true;
}

rmw_ret_t session_connect(ZenohPicoSession *session)
{
  RMW_ZENOH_FUNC_ENTRY(NULL);

  RMW_CHECK_ARGUMENT_FOR_NULL(session, false);

  if(session->enable_session){
    RMW_ZENOH_LOG_DEBUG("already session open.");
    return RMW_RET_OK;
  }

  RMW_ZENOH_LOG_DEBUG("Opening session...");

  /* zenoh-pico >= 1.7 starts one background executor task inside z_open()
   * (reads, callbacks, lease, reconnect); the attributes must go in here, the
   * zp_start_*_task() calls below find it already running. */
  z_open_options_t open_opts;
  z_open_options_default(&open_opts);
#if Z_FEATURE_MULTI_THREAD == 1
  open_opts.executor_task_attributes = _rmw_zenoh_pico_read_task_attr;
#endif
  if(_Z_IS_ERR(z_open(&session->session, z_move(session->config), &open_opts))){
    RMW_ZENOH_LOG_ERROR("Error setting up zenoh session");
    return RMW_RET_ERROR;
  }

  zp_task_read_options_t read_opts;
  zp_task_lease_options_t lease_opts;
  zp_task_read_options_default(&read_opts);
  zp_task_lease_options_default(&lease_opts);
  read_opts.task_attributes = _rmw_zenoh_pico_read_task_attr;
  lease_opts.task_attributes = _rmw_zenoh_pico_lease_task_attr;

  if (_Z_IS_ERR(zp_start_read_task(z_loan_mut(session->session), &read_opts))
      || _Z_IS_ERR(zp_start_lease_task(z_loan_mut(session->session), &lease_opts))) {
    RMW_ZENOH_LOG_ERROR("Unable to start read and lease tasks");
    z_drop(z_move(session->config));
    z_drop(z_move(session->session));
    return RMW_RET_ERROR;
  }

  session->enable_session = true;

  RMW_ZENOH_LOG_DEBUG("complite.");

  return RMW_RET_OK;
}

bool isEnableSession(ZenohPicoSession *session) {
  return session->enable_session;
}

bool declaration_liveliness(ZenohPicoSession *session,
			    const z_loaned_string_t *keyexpr,
			    z_owned_liveliness_token_t *token)
{
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_substr(&ke, z_string_data(keyexpr), z_string_len(keyexpr));
    if(_Z_IS_ERR(z_liveliness_declare_token(z_loan(session->session),
					    token,
					    z_loan(ke),
					    NULL))){
      RMW_ZENOH_LOG_INFO("Unable to declare token.");
      return false;
    }
    return true;
}
