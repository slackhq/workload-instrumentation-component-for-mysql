#define LOG_COMPONENT_TAG "workload_instrumentation"
#define NO_SIGNATURE_CHANGE 0
#define SIGNATURE_CHANGE 1

#include <iostream>
#include <regex>
#include <string>

#include <mysqld_error.h> /* Errors */

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/log_builtins.h> /* LogComponentErr */
#include <mysql/components/services/mysql_current_thread_reader.h>
#include <mysql/components/services/mysql_rwlock.h>
#include <mysql/components/services/pfs_plugin_table_service.h>

#include "mysql/components/util/event_tracking/event_tracking_query_consumer_helper.h"
#include "workload_instrumentation.h"
#include "workload_instrumentation_pfs.h"
#include "workload_instrumentation_thd_stats.h"

REQUIRES_SERVICE_PLACEHOLDER(mysql_current_thread_reader);
REQUIRES_SERVICE_PLACEHOLDER(pfs_plugin_table_v1);
REQUIRES_SERVICE_PLACEHOLDER_AS(pfs_plugin_column_bigint_v1, pfs_bigint);
REQUIRES_SERVICE_PLACEHOLDER_AS(pfs_plugin_column_string_v2, pfs_string);

static mysql_service_status_t workload_instrumentation_service_init() {
  log_bi = mysql_service_log_builtins;
  log_bs = mysql_service_log_builtins_string;
  mysql_service_status_t result = 0;

  LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                  "initializing component...");

  result = workload_instrumentation_pfs_init();
  if (result == 0) {
    LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                    "Component initialized");
  } else {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "Component failed to initialize properly");
  }

  return result;
}

static mysql_service_status_t workload_instrumentation_service_deinit() {
  mysql_service_status_t result = 0;

  result = workload_instrumentation_pfs_deinit();
  if (result == 0) {
    LogComponentErr(INFORMATION_LEVEL, ER_LOG_PRINTF_MSG,
                    "Component deinitialized");
  } else {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "Component failed to deinitialize properly");
  }

  return result;
}

std::string findWorkloadName(const std::string &input) {
  std::regex commentRegex(R"(/\*.*?\*/)");
  std::regex workloadRegex(R"(WORKLOAD_NAME=([A-Za-z0-9-_:.\/\\\\]+))");
  std::smatch match;
  std::sregex_iterator it(input.begin(), input.end(), commentRegex);
  std::sregex_iterator end;

  while (it != end) {
    std::string comment = it->str();
    if (std::regex_search(comment, match, workloadRegex)) {
      return match[1].str();
    }
    ++it;
  }

  return "";
}

mysql_event_tracking_query_subclass_t Event_tracking_implementation::
    Event_tracking_query_implementation::filtered_sub_events =
        EVENT_TRACKING_QUERY_START | EVENT_TRACKING_QUERY_NESTED_START |
        EVENT_TRACKING_QUERY_NESTED_STATUS_END;
bool Event_tracking_implementation::Event_tracking_query_implementation::
    callback(const mysql_event_tracking_query_data *data [[maybe_unused]]) {
  auto result = false;

  if (data->event_subclass != EVENT_TRACKING_QUERY_STATUS_END) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "Got incorrect event type, ignoring it.");
  }

  THD *current_thd = nullptr;
  mysql_service_status_t thd_res =
      mysql_service_mysql_current_thread_reader->get(&current_thd);
  if (thd_res != 0 || current_thd == nullptr)
    throw std::invalid_argument("Cannot extract current THD");

  auto *ts = get_thd_row_stats(current_thd);

  std::string workload = findWorkloadName(std::string(data->query.str));
  record_stats(workload, ts);

  return result;
}

IMPLEMENTS_SERVICE_EVENT_TRACKING_QUERY(workload_instrumentation);

BEGIN_COMPONENT_PROVIDES(workload_instrumentation_service)
PROVIDES_SERVICE_EVENT_TRACKING_QUERY(workload_instrumentation),
    END_COMPONENT_PROVIDES();

REQUIRES_MYSQL_RWLOCK_SERVICE_PLACEHOLDER;

BEGIN_COMPONENT_REQUIRES(workload_instrumentation_service)
  REQUIRES_SERVICE(log_builtins),
  REQUIRES_SERVICE(log_builtins_string),
  REQUIRES_SERVICE(mysql_current_thread_reader),
  REQUIRES_MYSQL_RWLOCK_SERVICE,
  REQUIRES_SERVICE(pfs_plugin_table_v1),
  REQUIRES_SERVICE_AS(pfs_plugin_column_bigint_v1, pfs_bigint),
  REQUIRES_SERVICE_AS(pfs_plugin_column_string_v2, pfs_string),
  END_COMPONENT_REQUIRES();

/* A list of metadata to describe the Component. */
BEGIN_COMPONENT_METADATA(workload_instrumentation_service)
METADATA("mysql.author", "Eduardo J. Ortega U."),
    METADATA("mysql.license", "GPL"), METADATA("mysql.dev", "ejortegau"),
    END_COMPONENT_METADATA();

/* Declaration of the Component. */
DECLARE_COMPONENT(workload_instrumentation_service,
                  "mysql:workload_instrumentation_service")
workload_instrumentation_service_init,
    workload_instrumentation_service_deinit END_DECLARE_COMPONENT();

/* Defines list of Components contained in this library. Note that for now
we assume that library will have exactly one Component. */
DECLARE_LIBRARY_COMPONENTS &COMPONENT_REF(workload_instrumentation_service)
    END_DECLARE_LIBRARY_COMPONENTS