#ifndef MYSQL_WORKLOAD_INSTRUMENTATION_PFS_H
#define MYSQL_WORKLOAD_INSTRUMENTATION_PFS_H

#include "chrono"
#include "map"

#include <mysql/components/component_implementation.h>
#include <mysql/components/services/bits/mysql_rwlock_bits.h>
#include <mysql/components/services/bits/psi_rwlock_bits.h>
#include <mysql/components/services/pfs_plugin_table_service.h>

#define LOG_COMPONENT_TAG "workload_instrumentation"

struct thread_stats;

struct workload_instrumentation_record {
  std::string workload;
  unsigned long long count_queries;
  unsigned long long sum_rows_examined;
  unsigned long long sum_rows_sent;
  unsigned long long sum_rows_affected;
  unsigned long long sum_query_duration_us;
};

class workload_instrumentation_POS {
 private:
  unsigned int m_index = 0;

 public:
  ~workload_instrumentation_POS() = default;
  workload_instrumentation_POS() { m_index = 0; }

  void reset() { m_index = 0; }
  unsigned int get_index() { return m_index; }
  void set_at(workload_instrumentation_POS *pos) { m_index = pos->m_index; }
  void set_after(workload_instrumentation_POS *pos) {
    m_index = pos->m_index + 1;
  }
};

struct workload_instrumentation_table_handle {
  workload_instrumentation_POS m_pos;
  workload_instrumentation_POS m_next_pos;
  workload_instrumentation_record m_current_row;
  unsigned int index_num;
};

void init_workload_instrumentation_share(PFS_engine_table_share_proxy *share);

extern PFS_engine_table_share_proxy workload_instrumentation_st_share;
extern PFS_engine_table_share_proxy *share_list[];
extern unsigned int share_list_count;


void record_stats(std::string workload, thread_stats *thd_stats);
int workload_instrumentation_pfs_init();
int workload_instrumentation_pfs_deinit();

#endif  // MYSQL_WORKLOAD_INSTRUMENTATION_PFS_H
