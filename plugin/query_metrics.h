//
// Created by eduardo.ortega on 12/18/25.
//

#ifndef MYSQL_AUDIT_QUERY_METRICS_H
#define MYSQL_AUDIT_QUERY_METRICS_H
#include <mysql/plugin.h>

#include <cstddef>

#include "my_inttypes.h"
#include "mysql/components/services/bits/psi_statement_bits.h"
class THD;

class query_stats_t {
 public:
  ulonglong affected_rows = 0;
  ulonglong examined_rows = 0;
  ulonglong sent_rows = 0;

  ulonglong lock_time = 0;

  unsigned long created_tmp_disk_tables = 0;
  unsigned long created_tmp_tables = 0;
  unsigned long select_full_join = 0;
  unsigned long select_full_range_join = 0;
  unsigned long select_range = 0;
  unsigned long select_range_check = 0;
  unsigned long select_scan = 0;
  unsigned long sort_merge_passes = 0;
  unsigned long sort_range = 0;
  unsigned long sort_rows = 0;
  unsigned long sort_scan = 0;
  unsigned char no_index_used = 0;
  unsigned char no_good_index_used = 0;

  query_stats_t(MYSQL_THD thd, PSI_statement_locker_state_v5 *psi_state);

  ulonglong get_wall_time_nanosec() const;
  ulonglong get_cpu_time_nanosec() const;
  const char *to_json(const char *workload_name, ulonglong team_id,
                      char *buf, size_t buf_size) const;

 private:
  ulonglong psi_timer_start = 0;
  ulonglong psi_timer_end = 0;
  ulonglong psi_cpu_time_start = 0;
  ulonglong psi_cpu_time_end = 0;
};

extern MYSQL_PLUGIN qm_plugin_handle;

#include "mysql/components/services/registry.h"
extern SERVICE_TYPE(registry) *qm_registry_service;

#endif  // MYSQL_AUDIT_QUERY_METRICS_H
