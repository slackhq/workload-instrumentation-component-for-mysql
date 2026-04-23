#include "plugin/query_metrics/query_metrics_vars.h"

#include <atomic>

#include <mysql/plugin.h>

#include "my_inttypes.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "sql/sql_class.h"
#include "sql/sql_error.h"
#include "plugin/query_metrics/query_metrics.h"
#include "plugin/query_metrics/query_metrics_socket.h"
#include "plugin/query_metrics/query_metrics_table.h"

bool query_metrics_enabled = true;
bool query_metrics_pfs_enabled = true;
bool query_metrics_warnings_enabled = true;
ulong query_metrics_table_size = 10000;
ulong query_metrics_sample_rate = 100;
bool query_metrics_socket_enabled = false;
char *query_metrics_socket_path = nullptr;

static MYSQL_SYSVAR_BOOL(enabled, query_metrics_enabled, PLUGIN_VAR_OPCMDARG,
                         "Enable or disable the query metrics plugin.", nullptr,
                         nullptr, true);
static int pfs_enabled_check(MYSQL_THD, SYS_VAR *, void *save,
                             struct st_mysql_value *value) {
  longlong in_val;
  value->val_int(value, &in_val);
  bool new_val = (in_val != 0);

  if (new_val && !qm_pfs_services_initialized()) {
    if (qm_pfs_services_init(qm_registry_service, &qm_plugin_handle)) {
      my_plugin_log_message(&qm_plugin_handle, MY_ERROR_LEVEL,
                            "Failed to lazy-init PFS services");
      my_message(ER_UNKNOWN_ERROR,
                 "query_metrics: failed to initialize PFS services", MYF(0));
      return 1;
    }
  }

  *static_cast<bool *>(save) = new_val;
  return 0;
}

static MYSQL_SYSVAR_BOOL(
    pfs_enabled, query_metrics_pfs_enabled, PLUGIN_VAR_OPCMDARG,
    "Expose collected metrics via performance_schema table.", pfs_enabled_check,
    nullptr, true);
static MYSQL_SYSVAR_BOOL(warnings_enabled, query_metrics_warnings_enabled,
                         PLUGIN_VAR_OPCMDARG,
                         "Emit a warning with query metrics after each query.",
                         nullptr, nullptr, true);
static void table_size_update(MYSQL_THD, SYS_VAR *, void *,
                              const void *save) {
  ulong new_size = *static_cast<const ulong *>(save);
  qm_resize_table(new_size);
}

static MYSQL_SYSVAR_ULONG(table_size, query_metrics_table_size,
                          PLUGIN_VAR_OPCMDARG,
                          "Number of rows in the query_metrics performance "
                          "schema table.",
                          nullptr, table_size_update, 10000, 1, 1000000, 1);

static MYSQL_SYSVAR_ULONG(sample_rate, query_metrics_sample_rate,
                          PLUGIN_VAR_OPCMDARG,
                          "Percentage of queries to process (1-100).",
                          nullptr, nullptr, 100, 1, 100, 1);

static int socket_enabled_check(MYSQL_THD thd, SYS_VAR *, void *save,
                                struct st_mysql_value *value) {
  longlong in_val;
  value->val_int(value, &in_val);
  bool new_val = (in_val != 0);

  if (new_val) {
    if (qm_socket_open()) {
      push_warning_printf(thd, Sql_condition::SL_WARNING, ER_YES,
                          "query_metrics: socket not available yet, "
                          "will retry on send");
    }
  } else {
    qm_socket_close();
  }

  *static_cast<bool *>(save) = new_val;
  return 0;
}

static MYSQL_SYSVAR_BOOL(
    socket_enabled, query_metrics_socket_enabled, PLUGIN_VAR_OPCMDARG,
    "Send query metrics JSON to a unix domain DGRAM socket.",
    socket_enabled_check, nullptr, false);

static void socket_path_update(MYSQL_THD thd, SYS_VAR *, void *var_ptr,
                               const void *save) {
  const char *new_path = *static_cast<const char *const *>(save);
  *static_cast<const char **>(var_ptr) = new_path;

  if (query_metrics_socket_enabled) {
    qm_socket_close();
    if (qm_socket_open()) {
      push_warning_printf(thd, Sql_condition::SL_WARNING, ER_YES,
                          "query_metrics: socket not available at new path, "
                          "will retry on send");
    }
  }
}

static MYSQL_SYSVAR_STR(socket_path, query_metrics_socket_path,
                        PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Path to the unix domain DGRAM socket.", nullptr,
                        socket_path_update, nullptr);

SYS_VAR *query_metrics_system_variables[] = {
    MYSQL_SYSVAR(enabled), MYSQL_SYSVAR(pfs_enabled),
    MYSQL_SYSVAR(warnings_enabled), MYSQL_SYSVAR(table_size),
    MYSQL_SYSVAR(sample_rate), MYSQL_SYSVAR(socket_enabled),
    MYSQL_SYSVAR(socket_path), nullptr};

static std::atomic<ulonglong> events_processed{0};
static std::atomic<ulonglong> events_ignored_wrong_class{0};
static std::atomic<ulonglong> events_ignored_wrong_subclass{0};
static std::atomic<ulonglong> events_ignored_wrong_sql_command{0};
static std::atomic<ulonglong> events_skipped_sampling{0};
static std::atomic<ulonglong> events_socket_send_errors{0};

static ulonglong events_processed_value = 0;
static ulonglong events_ignored_wrong_class_value = 0;
static ulonglong events_ignored_wrong_subclass_value = 0;
static ulonglong events_ignored_wrong_sql_command_value = 0;
static ulonglong events_skipped_sampling_value = 0;
static ulonglong events_socket_send_errors_value = 0;

static int show_query_completion_count(MYSQL_THD, SHOW_VAR *var, char *) {
  var->type = SHOW_LONGLONG;
  events_processed_value = events_processed.load();
  var->value = (char *)&events_processed_value;
  return 0;
}

static int show_events_ignored_wrong_class(MYSQL_THD, SHOW_VAR *var, char *) {
  var->type = SHOW_LONGLONG;
  events_ignored_wrong_class_value = events_ignored_wrong_class.load();
  var->value = (char *)&events_ignored_wrong_class_value;
  return 0;
}

static int show_events_ignored_wrong_subclass(MYSQL_THD, SHOW_VAR *var,
                                              char *) {
  var->type = SHOW_LONGLONG;
  events_ignored_wrong_subclass_value = events_ignored_wrong_subclass.load();
  var->value = (char *)&events_ignored_wrong_subclass_value;
  return 0;
}

static int show_events_ignored_wrong_sql_command(MYSQL_THD, SHOW_VAR *var,
                                                 char *) {
  var->type = SHOW_LONGLONG;
  events_ignored_wrong_sql_command_value =
      events_ignored_wrong_sql_command.load();
  var->value = (char *)&events_ignored_wrong_sql_command_value;
  return 0;
}

static int show_events_skipped_sampling(MYSQL_THD, SHOW_VAR *var, char *) {
  var->type = SHOW_LONGLONG;
  events_skipped_sampling_value = events_skipped_sampling.load();
  var->value = (char *)&events_skipped_sampling_value;
  return 0;
}

static int show_events_socket_send_errors(MYSQL_THD, SHOW_VAR *var, char *) {
  var->type = SHOW_LONGLONG;
  events_socket_send_errors_value = events_socket_send_errors.load();
  var->value = (char *)&events_socket_send_errors_value;
  return 0;
}

SHOW_VAR audit_query_metrics_status[] = {
    {"Audit_query_metrics_events_processed",
     (char *)show_query_completion_count, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Audit_query_metrics_events_ignored_wrong_class",
     (char *)show_events_ignored_wrong_class, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Audit_query_metrics_events_ignored_wrong_subclass",
     (char *)show_events_ignored_wrong_subclass, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Audit_query_metrics_events_ignored_wrong_sql_command",
     (char *)show_events_ignored_wrong_sql_command, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"Audit_query_metrics_events_skipped_sampling",
     (char *)show_events_skipped_sampling, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"Audit_query_metrics_socket_send_errors",
     (char *)show_events_socket_send_errors, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_GLOBAL}};

void qm_reset_status_counters() {
  events_processed.store(0);
  events_ignored_wrong_class.store(0);
  events_ignored_wrong_subclass.store(0);
  events_ignored_wrong_sql_command.store(0);
  events_skipped_sampling.store(0);
  events_socket_send_errors.store(0);
}

void qm_increment_events_processed() { events_processed.fetch_add(1); }

void qm_increment_events_ignored_wrong_class() {
  events_ignored_wrong_class.fetch_add(1);
}

void qm_increment_events_ignored_wrong_subclass() {
  events_ignored_wrong_subclass.fetch_add(1);
}

void qm_increment_events_ignored_wrong_sql_command() {
  events_ignored_wrong_sql_command.fetch_add(1);
}

void qm_increment_events_skipped_sampling() {
  events_skipped_sampling.fetch_add(1);
}

void qm_increment_socket_send_errors() {
  events_socket_send_errors.fetch_add(1);
}
