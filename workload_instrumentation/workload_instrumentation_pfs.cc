#include <array>
#include <iostream>
#include <string>

#include "workload_instrumentation_pfs.h"
#include "workload_instrumentation_thd_stats.h"

#include <mysql/components/services/log_builtins.h> /* LogComponentErr */
#include <mysql/components/services/mysql_rwlock.h>
#include <mysqld_error.h> /* Errors */

#define WORKLOAD_MAX_RECORDS 5000
#define OVERFLOW_WORKLOAD "__OVERFLOW__"
#define UNSPECIFIED_WORKLOAD "__UNSPECIFIED__"

extern mysql_service_pfs_plugin_table_v1_t *mysql_service_pfs_plugin_table_v1;
extern mysql_service_pfs_plugin_column_string_v2_t *pfs_string;
extern mysql_service_pfs_plugin_column_bigint_v1_t *pfs_bigint;

static size_t next_record = 0;

mysql_rwlock_t LOCK_workload_duration;
PSI_rwlock_key key_workload_instrumentation_LOCK_workload_duration;
PSI_rwlock_info psi_lock_workload_duration_info = {
    &key_workload_instrumentation_LOCK_workload_duration,
    "workload_instrumentation_duration", 0, 0,
    "Workload instrumentation duration"};
static PSI_rwlock_info all_workload_instrumentation_rwlocks[] = {
    psi_lock_workload_duration_info};

std::map<std::string, int> workload_pfs_record_map;
std::array<workload_instrumentation_record *, WORKLOAD_MAX_RECORDS + 2>
    workload_instrumentation_array;
std::map<int, unsigned long long> pfs_record_duration_us_map;

PFS_engine_table_share_proxy workload_instrumentation_st_share;

int workload_instrumentation_pfs_init() {
  // Lock initialization
  mysql_rwlock_register("workload_instrumentation",
                        all_workload_instrumentation_rwlocks, 1);
  int result =
      mysql_rwlock_init(key_workload_instrumentation_LOCK_workload_duration,
                        &LOCK_workload_duration);
  if (result != 0) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "Failed to init lock.");

    return result;
  }

  // Grab locks to initialize data structures used by component.
  result = mysql_rwlock_wrlock(&LOCK_workload_duration);
  if (result != 0) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "Failed to grab lock during P_S table initialization.");

    return result;
  }
  next_record = 0;
  workload_instrumentation_array.fill(nullptr);
  workload_pfs_record_map.clear();
  pfs_record_duration_us_map.clear();

  std::string predefined_workloads[] = {UNSPECIFIED_WORKLOAD,
                                        OVERFLOW_WORKLOAD};

  for (std::string predefined_workload : predefined_workloads) {
    auto record = new workload_instrumentation_record;
    record->count_queries = 0;
    record->sum_query_duration_us = 0;
    record->sum_rows_examined = 0;
    record->sum_rows_sent = 0;
    record->sum_rows_affected = 0;
    record->workload = predefined_workload;

    workload_pfs_record_map[predefined_workload] = next_record;
    workload_instrumentation_array[next_record] = record;
    next_record++;
  }

  // Release lock & exit.
  result = mysql_rwlock_unlock(&LOCK_workload_duration);
  if (result != 0) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "Failed to release lock during initialization.");

    return result;
  }

  init_workload_instrumentation_share(&workload_instrumentation_st_share);
  share_list[0] = &workload_instrumentation_st_share;

  auto res = mysql_service_pfs_plugin_table_v1->add_tables(&share_list[0],
                                                           share_list_count);
  if (res) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "PFS table could not be registered");

    result = 1;
  }

  return result;
}

int workload_instrumentation_pfs_deinit() {
  // Grab locks to clear data structures used by component.
  int result = mysql_rwlock_wrlock(&LOCK_workload_duration);
  if (result != 0) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "Failed to grab lock during initialization.");

    return result;
  }
  next_record = 0;
  workload_instrumentation_array.fill(nullptr);
  workload_pfs_record_map.clear();
  pfs_record_duration_us_map.clear();

  // Release lock.
  result = mysql_rwlock_unlock(&LOCK_workload_duration);

  // Destroy lock & exit.
  result = mysql_rwlock_destroy(&LOCK_workload_duration);
  if (result != 0) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG, "Failed to destroy lock.");

    return result;
  }

  auto res = mysql_service_pfs_plugin_table_v1->delete_tables(&share_list[0],
                                                              share_list_count);
  if (res) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "PFS table could not be registered");

    result = 1;
  }

  return result;
}

void record_stats(std::string workload, thread_stats *ts) {
  timeval now;
  gettimeofday(&now, nullptr);

  unsigned long long duration_us =
      now.tv_sec * 1000000 + now.tv_usec -
      (ts->start_time->tv_sec * 1000000 + ts->start_time->tv_usec);

  auto lock_result = mysql_rwlock_wrlock(&LOCK_workload_duration);
  if (lock_result != 0) {
    LogComponentErr(
        ERROR_LEVEL, ER_LOG_PRINTF_MSG,
        "Failed to grab lock for storing query stats, skipping this query.");

    return;
  }

  // Map empty workloads to unspecified workloads
  if (workload == "") {
    workload = UNSPECIFIED_WORKLOAD;
  }
  // Map new workloads that won't fit in the table to the overflow workload
  if (!workload_pfs_record_map.contains(workload)) {
    if (next_record == WORKLOAD_MAX_RECORDS + 2) {
      workload = OVERFLOW_WORKLOAD;
    }
  }

  // For non-existent workloads, create/initialize row with zero values
  if (!workload_pfs_record_map.contains(workload)) {
    workload_pfs_record_map[workload] = next_record;

    auto record = new workload_instrumentation_record;
    record->workload = workload;
    record->count_queries = 0;
    record->sum_rows_examined = 0;
    record->sum_rows_sent = 0;
    record->sum_rows_affected = 0;
    record->sum_query_duration_us = 0;

    workload_instrumentation_array[next_record] = record;
    next_record++;
  }

  auto record = workload_instrumentation_array[workload_pfs_record_map[workload]];
  record->count_queries += 1;
  record->sum_rows_sent += ts->rows_sent;
  record->sum_rows_examined += ts->rows_examined;
  record->sum_rows_affected += ts->rows_affected;
  record->sum_query_duration_us += duration_us;

  lock_result = mysql_rwlock_unlock(&LOCK_workload_duration);
  if (lock_result != 0) {
    LogComponentErr(ERROR_LEVEL, ER_LOG_PRINTF_MSG,
                    "Failed to release lock after storing query stats, "
                    "undefined behavior may follow.");
  }
}

/* Access to PS table */
PFS_engine_table_share_proxy *share_list[1] = {nullptr};
unsigned int share_list_count = 1;

int workload_instrumentation_delete_all_rows() { return 0; }

PSI_table_handle *workload_instrumentation_open_table(PSI_pos **pos) {
  auto temp = new workload_instrumentation_table_handle();
  *pos = (PSI_pos *)(&temp->m_pos);
  return (PSI_table_handle *)temp;
}

void workload_instrumentation_close_table(PSI_table_handle *handle) {
  auto temp = (workload_instrumentation_table_handle *)handle;
  delete temp;
}

void workload_instrumentation_copy_record(
    workload_instrumentation_record *dst,
    const workload_instrumentation_record *src) {
  dst->workload = src->workload;
  dst->count_queries = src->count_queries;
  dst->sum_query_duration_us = src->sum_query_duration_us;
  dst->sum_rows_examined = src->sum_rows_examined;
  dst->sum_rows_sent = src->sum_rows_sent;
  dst->sum_rows_affected = src->sum_rows_affected;

  return;
}

int workload_instrumentation_rnd_next(PSI_table_handle *handle) {
  auto th = (workload_instrumentation_table_handle *)handle;

  th->m_pos.set_at(&th->m_next_pos);
  size_t idx = th->m_pos.get_index();

  if (idx >= workload_instrumentation_array.size())
    return PFS_HA_ERR_END_OF_FILE;

  auto record = workload_instrumentation_array[idx];
  if (record == nullptr)
    return PFS_HA_ERR_END_OF_FILE;

  workload_instrumentation_copy_record(&th->m_current_row, record);
  th->m_next_pos.set_after(&th->m_pos);

  return 0;
}

int workload_instrumentation_rnd_init(PSI_table_handle *, bool) { return 0; }

int workload_instrumentation_rnd_pos(PSI_table_handle *handle) {
  auto th = (workload_instrumentation_table_handle *)handle;
  size_t idx = th->m_pos.get_index();

  if (idx < workload_instrumentation_array.size()) {
    auto record = workload_instrumentation_array[idx];

    if (record != nullptr) {
      workload_instrumentation_copy_record(&th->m_current_row, record);
    }
  }
  return 0;
}

void workload_instrumentation_reset_position(PSI_table_handle *handle) {
  auto th = (workload_instrumentation_table_handle *)handle;
  th->m_pos.reset();
  th->m_next_pos.reset();
}

int workload_instrumentation_read_column_value(PSI_table_handle *handle,
                                               PSI_field *field,
                                               unsigned int index) {
  auto th = (workload_instrumentation_table_handle *)handle;

  PSI_ulonglong *count_queries, *rows_examined, *rows_sent, *rows_affected, *duration_us;

  switch (index) {
    case 0: /* WORKLOAD */
      pfs_string->set_varchar_utf8mb4(field, (th->m_current_row.workload.c_str()));
      break;
    case 1: /* COUNT_QUERIES */
      count_queries = new PSI_ulonglong;
      count_queries->is_null = false;
      count_queries->val = th->m_current_row.count_queries;
      pfs_bigint->set_unsigned(field, *count_queries);
      break;
    case 2: /* ROWS_EXAMINED */
      rows_examined = new PSI_ulonglong;
      rows_examined->is_null = false;
      rows_examined->val = th->m_current_row.sum_rows_examined;
      pfs_bigint->set_unsigned(field, *rows_examined);
      break;
    case 3: /* ROWS_SENT */
      rows_sent = new PSI_ulonglong;
      rows_sent->is_null = false;
      rows_sent->val = th->m_current_row.sum_rows_sent;
      pfs_bigint->set_unsigned(field, *rows_sent);
      break;
    case 4: /* ROWS_AFFECTED */
      rows_affected = new PSI_ulonglong;
      rows_affected->is_null = false;
      rows_affected->val = th->m_current_row.sum_rows_affected;
      pfs_bigint->set_unsigned(field, *rows_affected);
      break;
    case 5: /* DURATION_US */
      duration_us = new PSI_ulonglong;
      duration_us->is_null = false;
      duration_us->val = th->m_current_row.sum_query_duration_us;
      pfs_bigint->set_unsigned(field, *duration_us);
      break;
    default: /* We should never reach here */
      assert(0);
  }
  return 0;
}

unsigned long long workload_instrumentation_get_row_count(void) {
  return WORKLOAD_MAX_RECORDS + 2;
}

void init_workload_instrumentation_share(PFS_engine_table_share_proxy *share) {
  share->m_table_name = "workload_instrumentation";
  share->m_table_name_length = 24;
  share->m_table_definition =
      "`WORKLOAD` varchar(50), `COUNT_QUERIES` BIGINT UNSIGNED, `SUM_ROWS_EXAMINED` BIGINT UNSIGNED, "
      "`SUM_ROWS_SENT` BIGINT UNSIGNED, `SUM_ROWS_AFFECTED` BIGINT UNSIGNED, `SUM_DURATION_US` BIGINT UNSIGNED";
  share->m_ref_length = sizeof(workload_instrumentation_POS);
  share->m_acl = READONLY;
  share->get_row_count = workload_instrumentation_get_row_count;
  share->delete_all_rows = workload_instrumentation_delete_all_rows;

  share->m_proxy_engine_table = {workload_instrumentation_rnd_next,
                                 workload_instrumentation_rnd_init,
                                 workload_instrumentation_rnd_pos,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 workload_instrumentation_read_column_value,
                                 workload_instrumentation_reset_position,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 nullptr,
                                 workload_instrumentation_open_table,
                                 workload_instrumentation_close_table};
}
