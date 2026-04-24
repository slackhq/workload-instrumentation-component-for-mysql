#include <mysql/plugin.h>
#include <mysql/plugin_audit.h>

#include <pthread.h>

#include "my_inttypes.h"
#include "my_sqlcommand.h"
#include "mysql/components/services/bits/psi_statement_bits.h"
#include "mysql/components/services/registry.h"
#include "mysql/psi/mysql_rwlock.h"
#include "plugin/workload_instrumentation/workload_instrumentation.h"
#include "plugin/workload_instrumentation/workload_instrumentation_table.h"
#include "plugin/workload_instrumentation/workload_instrumentation_vars.h"
#include "plugin/workload_instrumentation/workload_instrumentation_socket.h"
#include "plugin/workload_instrumentation/workload_instrumentation_workload.h"
#include "sql/sql_class.h"
#include "sql/sql_error.h"
#include "storage/perfschema/pfs_timer.h"

MYSQL_PLUGIN wi_plugin_handle = nullptr;

SERVICE_TYPE(registry) *wi_registry_service = nullptr;

static PSI_rwlock_key key_wi_rwlock;
#ifdef HAVE_PSI_INTERFACE
static PSI_rwlock_info all_wi_rwlocks[] = {
    {&key_wi_rwlock, "wi_rwlock", 0, 0, PSI_DOCUMENT_ME}};
static void init_wi_psi_keys() {
  const char *category = "workload_instrumentation";
  int count = static_cast<int>(array_elements(all_wi_rwlocks));
  mysql_rwlock_register(category, all_wi_rwlocks, count);
}
#endif

static bool should_process_query_type(MYSQL_THD thd) {
  enum_sql_command sql_command = (enum_sql_command)thd_sql_command(thd);

  switch (sql_command) {
  case SQLCOM_SELECT:
  case SQLCOM_INSERT:
  case SQLCOM_INSERT_SELECT:
  case SQLCOM_UPDATE:
  case SQLCOM_UPDATE_MULTI:
  case SQLCOM_DELETE:
  case SQLCOM_DELETE_MULTI:
  case SQLCOM_REPLACE:
  case SQLCOM_REPLACE_SELECT:
  case SQLCOM_LOAD:
    return true;
  default:
    return false;
  }
}

static int audit_workload_instrumentation_plugin_init(MYSQL_PLUGIN p) {
  wi_plugin_handle = p;

  SERVICE_TYPE(registry) *reg = mysql_plugin_registry_acquire();
  if (reg == nullptr) {
    my_plugin_log_message(&wi_plugin_handle, MY_ERROR_LEVEL,
                          "Failed to acquire registry service");
    return 1;
  }

  wi_registry_service = reg;

  wi_workload_services_init(wi_registry_service, &wi_plugin_handle);
  wi_socket_services_init(wi_registry_service);

  wi_reset_status_counters();

#ifdef HAVE_PSI_INTERFACE
  init_wi_psi_keys();
#endif
  mysql_rwlock_init(key_wi_rwlock, &wi_rwlock);

  wi_allocate_records();
  init_wi_share(&wi_st_share);

  if (workload_instrumentation_pfs_enabled) {
    if (wi_pfs_services_init(wi_registry_service, &wi_plugin_handle)) {
      wi_free_records();
      mysql_rwlock_destroy(&wi_rwlock);
      wi_socket_services_deinit(wi_registry_service);
      wi_workload_services_deinit(wi_registry_service);
      mysql_plugin_registry_release(wi_registry_service);
      wi_registry_service = nullptr;
      return 1;
    }
  }

  my_plugin_log_message(&wi_plugin_handle, MY_INFORMATION_LEVEL,
                        "Plugin initialized");

  return 0;
}

static int audit_workload_instrumentation_check_uninstall(void *) {
  return wi_pfs_check_uninstall();
}

static int audit_workload_instrumentation_plugin_deinit(void *arg [[maybe_unused]]) {
  wi_socket_close();
  wi_free_records();
  mysql_rwlock_destroy(&wi_rwlock);

  if (wi_registry_service != nullptr) {
    wi_socket_services_deinit(wi_registry_service);
    wi_workload_services_deinit(wi_registry_service);
    wi_pfs_services_deinit(wi_registry_service);
    mysql_plugin_registry_release(wi_registry_service);
    wi_registry_service = nullptr;
  }

  my_plugin_log_message(&wi_plugin_handle, MY_INFORMATION_LEVEL,
                        "Plugin shutting down");
  wi_plugin_handle = nullptr;

  return 0;
}

static bool should_skip_event(MYSQL_THD thd,
                              const mysql_event_class_t event_class,
                              const void *event) {
  wi_increment_events_processed();
  if (!workload_instrumentation_enabled)
    return true;

  if (thd == nullptr || event == nullptr)
    return true;

  if (event_class != MYSQL_AUDIT_QUERY_CLASS) {
    wi_increment_events_ignored_wrong_class();
    return true;
  }

  const auto *event_query =
      static_cast<const struct mysql_event_query *>(event);
  if (event_query->event_subclass != MYSQL_AUDIT_QUERY_STATUS_END) {
    wi_increment_events_ignored_wrong_subclass();
    return true;
  }

  if (!should_process_query_type(thd)) {
    wi_increment_events_ignored_wrong_sql_command();
    return true;
  }

  ulong rate = workload_instrumentation_sample_rate;
  if (rate < 100) {
    static __thread unsigned int wi_rand_seed
        __attribute__((tls_model("global-dynamic"))) = 0;
    if (wi_rand_seed == 0)
      wi_rand_seed = static_cast<unsigned int>(pthread_self());
    if (static_cast<ulong>(rand_r(&wi_rand_seed) % 100) >= rate) {
      wi_increment_events_skipped_sampling();
      return true;
    }
  }

  return false;
}

static int audit_workload_instrumentation_notify(MYSQL_THD thd,
                                      const mysql_event_class_t event_class,
                                      const void *event) {
  if (should_skip_event(thd, event_class, event))
    return 0;

  PSI_statement_locker *psi_locker = thd->m_statement_psi;
  static_assert(sizeof(thd->m_statement_state) >=
                    sizeof(PSI_statement_locker_state_v5),
                "m_statement_state too small for PSI_statement_locker_state_v5");

  if (psi_locker == nullptr) {
    my_plugin_log_message(&wi_plugin_handle, MY_ERROR_LEVEL,
                          "Query psi_locker is null, unable to get its metrics");
    return 0;
  }

  PSI_statement_locker_state_v5 *psi_state = &thd->m_statement_state;

  query_stats_t metrics(thd, psi_state);

  ulonglong wall_time_ps = metrics.get_wall_time_nanosec() * NANOSEC_TO_PICOSEC;
  ulonglong cpu_time_ps = metrics.get_cpu_time_nanosec() * NANOSEC_TO_PICOSEC;
  ulonglong lock_time_ps = metrics.lock_time * MICROSEC_TO_PICOSEC;

  query_attrs_t qattrs = get_query_attrs(thd);

  if (workload_instrumentation_pfs_enabled) {
    wi_aggregate_stats(qattrs.workload_name, metrics, wall_time_ps,
                       cpu_time_ps, lock_time_ps);
  }

  bool need_json =
      workload_instrumentation_warnings_enabled || workload_instrumentation_socket_enabled;
  if (need_json) {
    char json_buf[8192];
    const char *json = metrics.to_json(qattrs.workload_name, qattrs.team_id,
                                       json_buf, sizeof(json_buf));

    if (workload_instrumentation_warnings_enabled) {
      push_warning_printf(thd, Sql_condition::SL_NOTE, ER_YES, "%s", json);
    }

    if (workload_instrumentation_socket_enabled) {
      wi_socket_send(json, strlen(json));
    }
  }

  return 0;
}

static struct st_mysql_audit audit_workload_instrumentation_descriptor = {
    MYSQL_AUDIT_INTERFACE_VERSION,
    nullptr,
    audit_workload_instrumentation_notify,
    {0, 0, 0, 0, 0, 0, 0, 0, 0,
     static_cast<unsigned long>(MYSQL_AUDIT_QUERY_STATUS_END), 0, 0, 0}};

mysql_declare_plugin(audit_workload_instrumentation){
    MYSQL_AUDIT_PLUGIN,
    &audit_workload_instrumentation_descriptor,
    "WORKLOAD_INSTRUMENTATION",
    PLUGIN_AUTHOR_ORACLE,
    "Audit plugin for workload instrumentation",
    PLUGIN_LICENSE_GPL,
    audit_workload_instrumentation_plugin_init,
    audit_workload_instrumentation_check_uninstall,
    audit_workload_instrumentation_plugin_deinit,
    0x0001,
    workload_instrumentation_status,
    workload_instrumentation_system_variables,
    nullptr,
    0,
} mysql_declare_plugin_end;
