#include "plugin/workload_instrumentation/workload_instrumentation_vars.h"

#include <atomic>

#include <mysql/plugin.h>

#include "my_inttypes.h"
#include "my_sys.h"
#include "mysqld_error.h"
#include "sql/sql_class.h"
#include "sql/sql_error.h"
#include "plugin/workload_instrumentation/workload_instrumentation.h"
#include "plugin/workload_instrumentation/workload_instrumentation_socket.h"
#include "plugin/workload_instrumentation/workload_instrumentation_table.h"

bool workload_instrumentation_enabled = true;
bool workload_instrumentation_pfs_enabled = true;
bool workload_instrumentation_warnings_enabled = true;
ulong workload_instrumentation_table_size = 10000;
ulong workload_instrumentation_sample_rate = 100;
bool workload_instrumentation_socket_enabled = false;
char *workload_instrumentation_socket_path = nullptr;

static MYSQL_SYSVAR_BOOL(enabled, workload_instrumentation_enabled, PLUGIN_VAR_OPCMDARG,
                         "Enable or disable the workload instrumentation plugin.", nullptr,
                         nullptr, true);
static int pfs_enabled_check(MYSQL_THD, SYS_VAR *, void *save,
                             struct st_mysql_value *value) {
  longlong in_val;
  value->val_int(value, &in_val);
  bool new_val = (in_val != 0);

  if (new_val && !wi_pfs_services_initialized()) {
    if (wi_pfs_services_init(wi_registry_service, &wi_plugin_handle)) {
      my_plugin_log_message(&wi_plugin_handle, MY_ERROR_LEVEL,
                            "Failed to lazy-init PFS services");
      my_message(ER_UNKNOWN_ERROR,
                 "workload_instrumentation: failed to initialize PFS services", MYF(0));
      return 1;
    }
  }

  *static_cast<bool *>(save) = new_val;
  return 0;
}

static MYSQL_SYSVAR_BOOL(
    pfs_enabled, workload_instrumentation_pfs_enabled, PLUGIN_VAR_OPCMDARG,
    "Expose collected metrics via performance_schema table.", pfs_enabled_check,
    nullptr, true);
static MYSQL_SYSVAR_BOOL(warnings_enabled, workload_instrumentation_warnings_enabled,
                         PLUGIN_VAR_OPCMDARG,
                         "Emit a warning with query metrics after each query.",
                         nullptr, nullptr, true);
static void table_size_update(MYSQL_THD, SYS_VAR *, void *,
                              const void *save) {
  ulong new_size = *static_cast<const ulong *>(save);
  wi_resize_table(new_size);
}

static MYSQL_SYSVAR_ULONG(table_size, workload_instrumentation_table_size,
                          PLUGIN_VAR_OPCMDARG,
                          "Number of rows in the workload_instrumentation performance "
                          "schema table.",
                          nullptr, table_size_update, 10000, 1, 1000000, 1);

static MYSQL_SYSVAR_ULONG(sample_rate, workload_instrumentation_sample_rate,
                          PLUGIN_VAR_OPCMDARG,
                          "Percentage of queries to process (1-100).",
                          nullptr, nullptr, 100, 1, 100, 1);

static int socket_enabled_check(MYSQL_THD thd, SYS_VAR *, void *save,
                                struct st_mysql_value *value) {
  longlong in_val;
  value->val_int(value, &in_val);
  bool new_val = (in_val != 0);

  if (new_val) {
    if (wi_socket_open()) {
      push_warning_printf(thd, Sql_condition::SL_WARNING, ER_YES,
                          "workload_instrumentation: socket not available yet, "
                          "will retry on send");
    }
  } else {
    wi_socket_close();
  }

  *static_cast<bool *>(save) = new_val;
  return 0;
}

static MYSQL_SYSVAR_BOOL(
    socket_enabled, workload_instrumentation_socket_enabled, PLUGIN_VAR_OPCMDARG,
    "Send query metrics JSON to a unix domain DGRAM socket.",
    socket_enabled_check, nullptr, false);

static void socket_path_update(MYSQL_THD thd, SYS_VAR *, void *var_ptr,
                               const void *save) {
  const char *new_path = *static_cast<const char *const *>(save);
  *static_cast<const char **>(var_ptr) = new_path;

  if (workload_instrumentation_socket_enabled) {
    wi_socket_close();
    if (wi_socket_open()) {
      push_warning_printf(thd, Sql_condition::SL_WARNING, ER_YES,
                          "workload_instrumentation: socket not available at new path, "
                          "will retry on send");
    }
  }
}

static MYSQL_SYSVAR_STR(socket_path, workload_instrumentation_socket_path,
                        PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Path to the unix domain DGRAM socket.", nullptr,
                        socket_path_update, nullptr);

SYS_VAR *workload_instrumentation_system_variables[] = {
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

SHOW_VAR workload_instrumentation_status[] = {
    {"workload_instrumentation_events_processed",
     (char *)show_query_completion_count, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"workload_instrumentation_events_ignored_wrong_class",
     (char *)show_events_ignored_wrong_class, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"workload_instrumentation_events_ignored_wrong_subclass",
     (char *)show_events_ignored_wrong_subclass, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"workload_instrumentation_events_ignored_wrong_sql_command",
     (char *)show_events_ignored_wrong_sql_command, SHOW_FUNC,
     SHOW_SCOPE_GLOBAL},
    {"workload_instrumentation_events_skipped_sampling",
     (char *)show_events_skipped_sampling, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {"workload_instrumentation_socket_send_errors",
     (char *)show_events_socket_send_errors, SHOW_FUNC, SHOW_SCOPE_GLOBAL},
    {nullptr, nullptr, SHOW_UNDEF, SHOW_SCOPE_GLOBAL}};

void wi_reset_status_counters() {
  events_processed.store(0);
  events_ignored_wrong_class.store(0);
  events_ignored_wrong_subclass.store(0);
  events_ignored_wrong_sql_command.store(0);
  events_skipped_sampling.store(0);
  events_socket_send_errors.store(0);
}

void wi_increment_events_processed() { events_processed.fetch_add(1); }

void wi_increment_events_ignored_wrong_class() {
  events_ignored_wrong_class.fetch_add(1);
}

void wi_increment_events_ignored_wrong_subclass() {
  events_ignored_wrong_subclass.fetch_add(1);
}

void wi_increment_events_ignored_wrong_sql_command() {
  events_ignored_wrong_sql_command.fetch_add(1);
}

void wi_increment_events_skipped_sampling() {
  events_skipped_sampling.fetch_add(1);
}

void wi_increment_socket_send_errors() {
  events_socket_send_errors.fetch_add(1);
}
