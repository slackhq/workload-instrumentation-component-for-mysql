#include "my_rapidjson_size_t.h"

#include <rapidjson/allocators.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "plugin/query_metrics/query_metrics.h"

#include <mysql/plugin.h>

#include "mysql/components/services/bits/psi_statement_bits.h"
#include "sql/sql_class.h"
#include "storage/perfschema/pfs_timer.h"

query_stats_t::query_stats_t(MYSQL_THD thd,
                                 PSI_statement_locker_state_v5 *psi_state) {
  const Diagnostics_area *da = thd->get_stmt_da();
  if (da != nullptr && da->is_ok()) {
    affected_rows = da->affected_rows();
  }

  if (psi_state != nullptr) {
    examined_rows = psi_state->m_rows_examined;
    sent_rows = psi_state->m_rows_sent;
    psi_timer_start = psi_state->m_timer_start;
    psi_timer_end = get_statement_timer();
    lock_time = psi_state->m_lock_time;
    psi_cpu_time_start = psi_state->m_cpu_time_start;
    psi_cpu_time_end = get_thread_cpu_timer();

    created_tmp_disk_tables = psi_state->m_created_tmp_disk_tables;
    created_tmp_tables = psi_state->m_created_tmp_tables;
    select_full_join = psi_state->m_select_full_join;
    select_full_range_join = psi_state->m_select_full_range_join;
    select_range = psi_state->m_select_range;
    select_range_check = psi_state->m_select_range_check;
    select_scan = psi_state->m_select_scan;
    sort_merge_passes = psi_state->m_sort_merge_passes;
    sort_range = psi_state->m_sort_range;
    sort_rows = psi_state->m_sort_rows;
    sort_scan = psi_state->m_sort_scan;
    no_index_used = psi_state->m_no_index_used;
    no_good_index_used = psi_state->m_no_good_index_used;
  }
}

ulonglong query_stats_t::get_wall_time_nanosec() const {
  if (psi_timer_start != 0 && psi_timer_end >= psi_timer_start) {
    ulonglong duration = psi_timer_end - psi_timer_start;
#ifdef HAVE_NANOSEC_TIMER
    return duration;
#else
    return duration * 1000;
#endif
  }
  return 0;
}

ulonglong query_stats_t::get_cpu_time_nanosec() const {
  if (psi_cpu_time_start != 0 && psi_cpu_time_end >= psi_cpu_time_start) {
    return psi_cpu_time_end - psi_cpu_time_start;
  }
  return 0;
}

const char *query_stats_t::to_json(const char *workload_name,
                                   ulonglong team_id, char *buf,
                                   size_t buf_size) const {
  char alloc_buf[1024];
  rapidjson::MemoryPoolAllocator<> sb_alloc(buf, buf_size);
  rapidjson::MemoryPoolAllocator<> wr_alloc(alloc_buf, sizeof(alloc_buf));
  rapidjson::GenericStringBuffer<rapidjson::UTF8<>,
                                 rapidjson::MemoryPoolAllocator<>>
      sb(&sb_alloc, buf_size);
  rapidjson::Writer<decltype(sb), rapidjson::UTF8<>, rapidjson::UTF8<>,
                    rapidjson::MemoryPoolAllocator<>>
      w(sb, &wr_alloc);

  w.StartObject();
  w.Key("workload_name");
  w.String(workload_name);
  w.Key("team_id");
  w.Uint64(team_id);
  w.Key("timer_wait_ns");
  w.Uint64(get_wall_time_nanosec());
  w.Key("cpu_time_ns");
  w.Uint64(get_cpu_time_nanosec());
  w.Key("lock_time_us");
  w.Uint64(lock_time);
  w.Key("rows_affected");
  w.Uint64(affected_rows);
  w.Key("rows_examined");
  w.Uint64(examined_rows);
  w.Key("rows_sent");
  w.Uint64(sent_rows);
  w.Key("created_tmp_disk_tables");
  w.Uint64(created_tmp_disk_tables);
  w.Key("created_tmp_tables");
  w.Uint64(created_tmp_tables);
  w.Key("select_full_join");
  w.Uint64(select_full_join);
  w.Key("select_full_range_join");
  w.Uint64(select_full_range_join);
  w.Key("select_range");
  w.Uint64(select_range);
  w.Key("select_range_check");
  w.Uint64(select_range_check);
  w.Key("select_scan");
  w.Uint64(select_scan);
  w.Key("sort_merge_passes");
  w.Uint64(sort_merge_passes);
  w.Key("sort_range");
  w.Uint64(sort_range);
  w.Key("sort_rows");
  w.Uint64(sort_rows);
  w.Key("sort_scan");
  w.Uint64(sort_scan);

  w.Key("no_index_used");
  w.Uint64(no_index_used);
  w.Key("no_good_index_used");
  w.Uint64(no_good_index_used);
  w.EndObject();

  if (sb.GetSize() >= buf_size) {
    static const char kTruncated[] = "{\"error\":\"json truncated\"}";
    return kTruncated;
  }

  return sb.GetString();
}
