#include "plugin/workload_instrumentation/workload_instrumentation_table.h"

#include <cstring>

#include "mysql/components/services/registry.h"
#include "plugin/workload_instrumentation/workload_instrumentation.h"

SERVICE_TYPE(pfs_plugin_table_v1) *wi_table_svc = nullptr;
SERVICE_TYPE(pfs_plugin_column_bigint_v1) *wi_col_bigint_svc = nullptr;
SERVICE_TYPE(pfs_plugin_column_string_v2) *wi_col_string_svc = nullptr;

static my_h_service h_table_svc = nullptr;
static my_h_service h_col_bigint_svc = nullptr;
static my_h_service h_col_string_svc = nullptr;

/* PFS table share proxy instance for the workload_instrumentation table. */
PFS_engine_table_share_proxy wi_st_share;

/* Array of share pointers passed to add_tables()/delete_tables(). */
PFS_engine_table_share_proxy *wi_share_list[1] = {nullptr};
unsigned int wi_share_list_count = 1;

/*
  Global array of WI_Record slots. Heap-allocated during plugin init with
  workload_instrumentation_table_size entries. Each slot holds cumulative stats for one
  distinct workload_name.
*/
WI_Record *wi_records_array = nullptr;

/*
  Hash map from workload_name string -> array index. Protected by wi_rwlock.
  Readers (existing workload lookup) take a shared/read lock; writers (new
  workload insertion) take an exclusive/write lock.
*/
std::unordered_map<std::string, unsigned int> wi_workload_map;
mysql_rwlock_t wi_rwlock;

static const char *WI_OVERFLOW_NAME = "<overflow>";
static const unsigned int WI_OVERFLOW_NAME_LEN = 10;
static const unsigned int WI_OVERFLOW_SLOT = 0;

/* Tracks how many slots currently have m_exist == true. */
static unsigned int wi_rows_in_table = 0;

/* Next available index for a new record slot. */
static unsigned int wi_next_available_index = 0;

static void wi_init_overflow_record(WI_Record *rec) {
  memcpy(rec->workload_name, WI_OVERFLOW_NAME, WI_OVERFLOW_NAME_LEN);
  rec->workload_name[WI_OVERFLOW_NAME_LEN] = '\0';
  rec->workload_name_length = WI_OVERFLOW_NAME_LEN;
  rec->m_exist.store(true, std::memory_order_relaxed);
}

/*
  WI_POS::has_more() — returns true if the cursor index is still within
  the bounds of the array. Implemented here (not inline in the header)
  because it reads the extern workload_instrumentation_table_size.
*/
bool WI_POS::has_more() { return m_index < workload_instrumentation_table_size; }

static WI_Record *wi_find_or_create_record_wlocked(
    const char *workload_name, unsigned int name_len, std::string key);

static void do_aggregate(WI_Record *rec, const query_stats_t &stats,
                         ulonglong wall_time_ps, ulonglong cpu_time_ps,
                         ulonglong lock_time_ps) {
  rec->count_star.fetch_add(1, std::memory_order_relaxed);
  rec->sum_timer_wait.fetch_add(wall_time_ps, std::memory_order_relaxed);
  rec->sum_cpu_time.fetch_add(cpu_time_ps, std::memory_order_relaxed);
  rec->sum_lock_time.fetch_add(lock_time_ps, std::memory_order_relaxed);
  rec->sum_rows_affected.fetch_add(stats.affected_rows,
                                   std::memory_order_relaxed);
  rec->sum_rows_examined.fetch_add(stats.examined_rows,
                                   std::memory_order_relaxed);
  rec->sum_rows_sent.fetch_add(stats.sent_rows, std::memory_order_relaxed);
  rec->sum_created_tmp_disk_tables.fetch_add(stats.created_tmp_disk_tables,
                                             std::memory_order_relaxed);
  rec->sum_created_tmp_tables.fetch_add(stats.created_tmp_tables,
                                        std::memory_order_relaxed);
  rec->sum_select_full_join.fetch_add(stats.select_full_join,
                                      std::memory_order_relaxed);
  rec->sum_select_full_range_join.fetch_add(stats.select_full_range_join,
                                            std::memory_order_relaxed);
  rec->sum_select_range.fetch_add(stats.select_range,
                                  std::memory_order_relaxed);
  rec->sum_select_range_check.fetch_add(stats.select_range_check,
                                        std::memory_order_relaxed);
  rec->sum_select_scan.fetch_add(stats.select_scan, std::memory_order_relaxed);
  rec->sum_sort_merge_passes.fetch_add(stats.sort_merge_passes,
                                       std::memory_order_relaxed);
  rec->sum_sort_range.fetch_add(stats.sort_range, std::memory_order_relaxed);
  rec->sum_sort_rows.fetch_add(stats.sort_rows, std::memory_order_relaxed);
  rec->sum_sort_scan.fetch_add(stats.sort_scan, std::memory_order_relaxed);
  rec->sum_no_index_used.fetch_add(stats.no_index_used,
                                   std::memory_order_relaxed);
  rec->sum_no_good_index_used.fetch_add(stats.no_good_index_used,
                                        std::memory_order_relaxed);
}

void wi_aggregate_stats(const char *workload_name,
                         const query_stats_t &stats, ulonglong wall_time_ps,
                         ulonglong cpu_time_ps, ulonglong lock_time_ps) {
  unsigned int name_len = strlen(workload_name);
  if (name_len >= WI_WORKLOAD_NAME_CHARSET_LEN)
    name_len = WI_WORKLOAD_NAME_CHARSET_LEN - 1;

  std::string key;
  key.assign(workload_name, name_len);

  mysql_rwlock_rdlock(&wi_rwlock);
  auto it = wi_workload_map.find(key);
  if (it != wi_workload_map.end()) {
    WI_Record *rec = &wi_records_array[it->second];
    do_aggregate(rec, stats, wall_time_ps, cpu_time_ps, lock_time_ps);
    mysql_rwlock_unlock(&wi_rwlock);
    return;
  }
  mysql_rwlock_unlock(&wi_rwlock);

  mysql_rwlock_wrlock(&wi_rwlock);
  WI_Record *rec = wi_find_or_create_record_wlocked(workload_name, name_len,
                                                     std::move(key));
  do_aggregate(rec, stats, wall_time_ps, cpu_time_ps, lock_time_ps);
  mysql_rwlock_unlock(&wi_rwlock);
}

/*
  copy_record() — snapshot a live WI_Record into a plain WI_Row_Copy.

  Each atomic counter is loaded with relaxed ordering and stored into the
  corresponding PSI_ubigint field (with is_null = false). This gives a
  consistent-enough snapshot for display purposes: individual counters are
  read atomically, but the set of counters is not read under a single lock,
  so slight cross-counter skew is possible under concurrent writes.
*/
static void copy_record(WI_Row_Copy *dest, WI_Record *source) {
  unsigned int len = source->workload_name_length;
  if (len >= WI_WORKLOAD_NAME_CHARSET_LEN)
    len = WI_WORKLOAD_NAME_CHARSET_LEN - 1;
  dest->workload_name_length = len;
  memcpy(dest->workload_name, source->workload_name, len);
  dest->workload_name[len] = '\0';

  dest->count_star = {source->count_star.load(std::memory_order_relaxed),
                      false};
  dest->sum_timer_wait = {
      source->sum_timer_wait.load(std::memory_order_relaxed), false};
  dest->sum_cpu_time = {source->sum_cpu_time.load(std::memory_order_relaxed),
                        false};
  dest->sum_lock_time = {
      source->sum_lock_time.load(std::memory_order_relaxed), false};
  dest->sum_rows_affected = {
      source->sum_rows_affected.load(std::memory_order_relaxed), false};
  dest->sum_rows_examined = {
      source->sum_rows_examined.load(std::memory_order_relaxed), false};
  dest->sum_rows_sent = {
      source->sum_rows_sent.load(std::memory_order_relaxed), false};

  dest->sum_created_tmp_disk_tables = {
      source->sum_created_tmp_disk_tables.load(std::memory_order_relaxed),
      false};
  dest->sum_created_tmp_tables = {
      source->sum_created_tmp_tables.load(std::memory_order_relaxed), false};
  dest->sum_select_full_join = {
      source->sum_select_full_join.load(std::memory_order_relaxed), false};
  dest->sum_select_full_range_join = {
      source->sum_select_full_range_join.load(std::memory_order_relaxed),
      false};
  dest->sum_select_range = {
      source->sum_select_range.load(std::memory_order_relaxed), false};
  dest->sum_select_range_check = {
      source->sum_select_range_check.load(std::memory_order_relaxed), false};
  dest->sum_select_scan = {
      source->sum_select_scan.load(std::memory_order_relaxed), false};
  dest->sum_sort_merge_passes = {
      source->sum_sort_merge_passes.load(std::memory_order_relaxed), false};
  dest->sum_sort_range = {
      source->sum_sort_range.load(std::memory_order_relaxed), false};
  dest->sum_sort_rows = {
      source->sum_sort_rows.load(std::memory_order_relaxed), false};
  dest->sum_sort_scan = {
      source->sum_sort_scan.load(std::memory_order_relaxed), false};
  dest->sum_no_index_used = {
      source->sum_no_index_used.load(std::memory_order_relaxed), false};
  dest->sum_no_good_index_used = {
      source->sum_no_good_index_used.load(std::memory_order_relaxed), false};
}

/*
  wi_allocate_records() — heap-allocate the records array with
  workload_instrumentation_table_size entries, value-initialized (all atomics start
  at 0/false). Called once during plugin init.
*/
void wi_allocate_records() {
  wi_records_array = new WI_Record[workload_instrumentation_table_size]();
  wi_init_overflow_record(&wi_records_array[WI_OVERFLOW_SLOT]);
  wi_workload_map[std::string(WI_OVERFLOW_NAME, WI_OVERFLOW_NAME_LEN)] =
      WI_OVERFLOW_SLOT;
  wi_rows_in_table = 1;
  wi_next_available_index = 1;
}

/*
  wi_free_records() — release the records array. Called during plugin deinit.
*/
void wi_free_records() {
  delete[] wi_records_array;
  wi_records_array = nullptr;
  wi_rows_in_table = 0;
  wi_next_available_index = 0;
  wi_workload_map.clear();
}

unsigned int wi_get_rows_in_table() { return wi_rows_in_table; }

void wi_resize_table(ulong new_size) {
  mysql_rwlock_wrlock(&wi_rwlock);

  auto *new_array = new WI_Record[new_size]();

  unsigned int copy_count =
      wi_rows_in_table < new_size ? wi_rows_in_table : new_size;

  wi_workload_map.clear();

  for (unsigned int i = 0; i < copy_count; i++) {
    WI_Record *src = &wi_records_array[i];
    WI_Record *dst = &new_array[i];
    memcpy(dst->workload_name, src->workload_name, src->workload_name_length);
    dst->workload_name[src->workload_name_length] = '\0';
    dst->workload_name_length = src->workload_name_length;
    dst->count_star.store(src->count_star.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
    dst->sum_timer_wait.store(
        src->sum_timer_wait.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_cpu_time.store(src->sum_cpu_time.load(std::memory_order_relaxed),
                            std::memory_order_relaxed);
    dst->sum_lock_time.store(
        src->sum_lock_time.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_rows_affected.store(
        src->sum_rows_affected.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_rows_examined.store(
        src->sum_rows_examined.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_rows_sent.store(
        src->sum_rows_sent.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_created_tmp_disk_tables.store(
        src->sum_created_tmp_disk_tables.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_created_tmp_tables.store(
        src->sum_created_tmp_tables.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_select_full_join.store(
        src->sum_select_full_join.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_select_full_range_join.store(
        src->sum_select_full_range_join.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_select_range.store(
        src->sum_select_range.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_select_range_check.store(
        src->sum_select_range_check.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_select_scan.store(
        src->sum_select_scan.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_sort_merge_passes.store(
        src->sum_sort_merge_passes.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_sort_range.store(
        src->sum_sort_range.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_sort_rows.store(
        src->sum_sort_rows.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_sort_scan.store(src->sum_sort_scan.load(std::memory_order_relaxed),
                             std::memory_order_relaxed);
    dst->sum_no_index_used.store(
        src->sum_no_index_used.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->sum_no_good_index_used.store(
        src->sum_no_good_index_used.load(std::memory_order_relaxed),
        std::memory_order_relaxed);
    dst->m_exist.store(true, std::memory_order_relaxed);

    wi_workload_map[std::string(dst->workload_name, dst->workload_name_length)] = i;
  }

  if (copy_count == 0) {
    wi_init_overflow_record(&new_array[WI_OVERFLOW_SLOT]);
    wi_workload_map[std::string(WI_OVERFLOW_NAME, WI_OVERFLOW_NAME_LEN)] =
        WI_OVERFLOW_SLOT;
    copy_count = 1;
  }

  delete[] wi_records_array;
  wi_records_array = new_array;
  workload_instrumentation_table_size = new_size;
  wi_rows_in_table = copy_count;
  wi_next_available_index = copy_count;

  mysql_rwlock_unlock(&wi_rwlock);
}

static WI_Record *wi_find_or_create_record_wlocked(
    const char *workload_name, unsigned int name_len, std::string key) {
  auto it = wi_workload_map.find(key);
  if (it != wi_workload_map.end()) {
    return &wi_records_array[it->second];
  }

  if (wi_rows_in_table >= workload_instrumentation_table_size) {
    return &wi_records_array[WI_OVERFLOW_SLOT];
  }

  unsigned int slot = wi_next_available_index;
  WI_Record *rec = &wi_records_array[slot];

  memcpy(rec->workload_name, workload_name, name_len);
  rec->workload_name[name_len] = '\0';
  rec->workload_name_length = name_len;
  rec->count_star.store(0, std::memory_order_relaxed);
  rec->sum_timer_wait.store(0, std::memory_order_relaxed);
  rec->sum_cpu_time.store(0, std::memory_order_relaxed);
  rec->sum_lock_time.store(0, std::memory_order_relaxed);
  rec->sum_rows_affected.store(0, std::memory_order_relaxed);
  rec->sum_rows_examined.store(0, std::memory_order_relaxed);
  rec->sum_rows_sent.store(0, std::memory_order_relaxed);
  rec->sum_created_tmp_disk_tables.store(0, std::memory_order_relaxed);
  rec->sum_created_tmp_tables.store(0, std::memory_order_relaxed);
  rec->sum_select_full_join.store(0, std::memory_order_relaxed);
  rec->sum_select_full_range_join.store(0, std::memory_order_relaxed);
  rec->sum_select_range.store(0, std::memory_order_relaxed);
  rec->sum_select_range_check.store(0, std::memory_order_relaxed);
  rec->sum_select_scan.store(0, std::memory_order_relaxed);
  rec->sum_sort_merge_passes.store(0, std::memory_order_relaxed);
  rec->sum_sort_range.store(0, std::memory_order_relaxed);
  rec->sum_sort_rows.store(0, std::memory_order_relaxed);
  rec->sum_sort_scan.store(0, std::memory_order_relaxed);
  rec->sum_no_index_used.store(0, std::memory_order_relaxed);
  rec->sum_no_good_index_used.store(0, std::memory_order_relaxed);
  rec->m_exist.store(true, std::memory_order_release);

  wi_workload_map[std::move(key)] = slot;
  wi_rows_in_table++;
  wi_next_available_index++;

  return rec;
}

/*
  wi_open_table() — called by PFS when a SELECT on performance_schema.workload_instrumentation
  begins. Allocates a new WI_Table_Handle (cursor state) and sets *pos to
  point at the cursor's m_pos so PFS can save/restore position for rnd_pos().
*/
PSI_table_handle *wi_open_table(PSI_pos **pos) {
  mysql_rwlock_rdlock(&wi_rwlock);
  auto *temp = new WI_Table_Handle();
  *pos = reinterpret_cast<PSI_pos *>(&temp->m_pos);
  return reinterpret_cast<PSI_table_handle *>(temp);
}

/*
  wi_close_table() — called by PFS when the SELECT completes. Frees the
  WI_Table_Handle that was allocated in wi_open_table().
*/
void wi_close_table(PSI_table_handle *handle) {
  auto *temp = reinterpret_cast<WI_Table_Handle *>(handle);
  delete temp;
  mysql_rwlock_unlock(&wi_rwlock);
}

/*
  wi_rnd_next() — advance the cursor to the next occupied slot.

  Starting from m_next_pos, scans forward through wi_records_array. For each
  slot where m_exist is true, snapshots it into current_row via copy_record(),
  sets m_next_pos to the slot after, and returns 0 (success).

  Returns PFS_HA_ERR_END_OF_FILE when no more occupied slots remain.
*/
int wi_rnd_next(PSI_table_handle *handle) {
  auto *h = reinterpret_cast<WI_Table_Handle *>(handle);

  for (h->m_pos.set_at(&h->m_next_pos); h->m_pos.has_more();
       h->m_pos.next()) {
    WI_Record *record = &wi_records_array[h->m_pos.get_index()];

    if (record->m_exist.load(std::memory_order_relaxed)) {
      copy_record(&h->current_row, record);
      h->m_next_pos.set_after(&h->m_pos);
      return 0;
    }
  }

  return PFS_HA_ERR_END_OF_FILE;
}

/*
  wi_rnd_init() — called by PFS before starting a table scan.
  No initialization needed for our simple array-based storage, so this is
  a no-op that returns 0 (success).
*/
int wi_rnd_init(PSI_table_handle *, bool) { return 0; }

/*
  wi_rnd_pos() — read the row at the position previously saved by PFS.

  PFS may call this to re-read a specific row (e.g. for UPDATE or after
  sorting). The cursor's m_pos has already been restored by PFS from the
  saved position. We snapshot that slot into current_row if it still exists.
*/
int wi_rnd_pos(PSI_table_handle *handle) {
  auto *h = reinterpret_cast<WI_Table_Handle *>(handle);
  WI_Record *record = &wi_records_array[h->m_pos.get_index()];

  if (record->m_exist.load(std::memory_order_relaxed)) {
    copy_record(&h->current_row, record);
  }

  return 0;
}

/*
  wi_index_init() — initialize a PFS index scan.

  Sets up the key structure for the WORKLOAD_NAME index (idx == 0).
  PFS passes the index number and we configure the key's name, buffer,
  and capacity so that index_read() can populate it.
*/
int wi_index_init(PSI_table_handle *handle, uint idx,
                   bool sorted [[maybe_unused]], PSI_index_handle **index) {
  auto *h = reinterpret_cast<WI_Table_Handle *>(handle);

  switch (idx) {
    case 0: {
      h->index_num = idx;
      WI_index_by_workload_name *i = &h->m_workload_name_index;
      i->m_workload_name.m_name = "WORKLOAD_NAME";
      i->m_workload_name.m_find_flags = 0;
      i->m_workload_name.m_value_buffer = i->m_workload_name_buffer;
      i->m_workload_name.m_value_buffer_capacity =
          sizeof(i->m_workload_name_buffer);
      *index = reinterpret_cast<PSI_index_handle *>(i);
    } break;
    default:
      break;
  }

  return 0;
}

/*
  wi_index_read() — read the key value from the WHERE clause.

  PFS calls this after index_init() to tell us what value the user is
  searching for. We use the col_string_svc->read_key_string() service
  to populate the PSI_plugin_key_string with the search value.
*/
int wi_index_read(PSI_index_handle *index, PSI_key_reader *reader,
                   unsigned int idx, int find_flag) {
  switch (idx) {
    case 0: {
      auto *i = reinterpret_cast<WI_index_by_workload_name *>(index);
      wi_col_string_svc->read_key_string(reader, &i->m_workload_name,
                                           find_flag);
    } break;
    default:
      break;
  }

  return 0;
}

/*
  wi_index_next() — advance through the array, returning only rows
  that match the index key.

  Scans forward from m_next_pos, checks m_exist, then calls match()
  which delegates to col_string_svc->match_key_string(). Returns 0
  on a matching row, PFS_HA_ERR_END_OF_FILE when no more matches.
*/
int wi_index_next(PSI_table_handle *handle) {
  auto *h = reinterpret_cast<WI_Table_Handle *>(handle);
  WI_index_by_workload_name *i = &h->m_workload_name_index;

  for (h->m_pos.set_at(&h->m_next_pos); h->m_pos.has_more();
       h->m_pos.next()) {
    WI_Record *record = &wi_records_array[h->m_pos.get_index()];

    if (record->m_exist.load(std::memory_order_relaxed)) {
      if (i->match(record)) {
        copy_record(&h->current_row, record);
        h->m_next_pos.set_after(&h->m_pos);
        return 0;
      }
    }
  }

  return PFS_HA_ERR_END_OF_FILE;
}

/*
  wi_reset_position() — reset both cursor positions to the beginning of
  the array. Called by PFS before starting a new scan.
*/
void wi_reset_position(PSI_table_handle *handle) {
  auto *h = reinterpret_cast<WI_Table_Handle *>(handle);
  h->m_pos.reset();
  h->m_next_pos.reset();
}

/*
  wi_read_column_value() — called by PFS once per column for the current row.

  The index parameter (0..20) corresponds to the column position in the table
  definition passed in init_wi_share(). See that function for the full list.
*/
int wi_read_column_value(PSI_table_handle *handle, PSI_field *field,
                          uint index) {
  auto *h = reinterpret_cast<WI_Table_Handle *>(handle);

  switch (index) {
    case 0:
      wi_col_string_svc->set_varchar_utf8mb4_len(
          field, h->current_row.workload_name,
          h->current_row.workload_name_length);
      break;
    case 1:
      wi_col_bigint_svc->set_unsigned(field, h->current_row.count_star);
      break;
    case 2:
      wi_col_bigint_svc->set_unsigned(field, h->current_row.sum_timer_wait);
      break;
    case 3:
      wi_col_bigint_svc->set_unsigned(field, h->current_row.sum_cpu_time);
      break;
    case 4:
      wi_col_bigint_svc->set_unsigned(field, h->current_row.sum_lock_time);
      break;
    case 5:
      wi_col_bigint_svc->set_unsigned(field,
                                       h->current_row.sum_rows_affected);
      break;
    case 6:
      wi_col_bigint_svc->set_unsigned(field,
                                       h->current_row.sum_rows_examined);
      break;
    case 7:
      wi_col_bigint_svc->set_unsigned(field, h->current_row.sum_rows_sent);
      break;
    case 8:
      wi_col_bigint_svc->set_unsigned(
          field, h->current_row.sum_created_tmp_disk_tables);
      break;
    case 9:
      wi_col_bigint_svc->set_unsigned(field,
                                       h->current_row.sum_created_tmp_tables);
      break;
    case 10:
      wi_col_bigint_svc->set_unsigned(field,
                                       h->current_row.sum_select_full_join);
      break;
    case 11:
      wi_col_bigint_svc->set_unsigned(
          field, h->current_row.sum_select_full_range_join);
      break;
    case 12:
      wi_col_bigint_svc->set_unsigned(field,
                                       h->current_row.sum_select_range);
      break;
    case 13:
      wi_col_bigint_svc->set_unsigned(field,
                                       h->current_row.sum_select_range_check);
      break;
    case 14:
      wi_col_bigint_svc->set_unsigned(field,
                                       h->current_row.sum_select_scan);
      break;
    case 15:
      wi_col_bigint_svc->set_unsigned(field,
                                       h->current_row.sum_sort_merge_passes);
      break;
    case 16:
      wi_col_bigint_svc->set_unsigned(field, h->current_row.sum_sort_range);
      break;
    case 17:
      wi_col_bigint_svc->set_unsigned(field, h->current_row.sum_sort_rows);
      break;
    case 18:
      wi_col_bigint_svc->set_unsigned(field, h->current_row.sum_sort_scan);
      break;
    case 19:
      wi_col_bigint_svc->set_unsigned(field,
                                       h->current_row.sum_no_index_used);
      break;
    case 20:
      wi_col_bigint_svc->set_unsigned(field,
                                       h->current_row.sum_no_good_index_used);
      break;
    default:
      break;
  }

  return 0;
}

/*
  wi_delete_all_rows() — implements TRUNCATE TABLE on
  performance_schema.workload_instrumentation.

  Iterates all slots, marks them as empty (m_exist = false), and zeroes
  every counter. Clears the hash map and resets wi_rows_in_table to 0.
*/
int wi_delete_all_rows(void) {
  if (wi_records_array == nullptr) return 0;

  mysql_rwlock_wrlock(&wi_rwlock);
  for (unsigned int i = 0; i < wi_next_available_index; i++) {
    wi_records_array[i].m_exist.store(false, std::memory_order_relaxed);
    wi_records_array[i].count_star.store(0, std::memory_order_relaxed);
    wi_records_array[i].sum_timer_wait.store(0, std::memory_order_relaxed);
    wi_records_array[i].sum_cpu_time.store(0, std::memory_order_relaxed);
    wi_records_array[i].sum_lock_time.store(0, std::memory_order_relaxed);
    wi_records_array[i].sum_rows_affected.store(0, std::memory_order_relaxed);
    wi_records_array[i].sum_rows_examined.store(0, std::memory_order_relaxed);
    wi_records_array[i].sum_rows_sent.store(0, std::memory_order_relaxed);
    wi_records_array[i].sum_created_tmp_disk_tables.store(
        0, std::memory_order_relaxed);
    wi_records_array[i].sum_created_tmp_tables.store(0,
                                                      std::memory_order_relaxed);
    wi_records_array[i].sum_select_full_join.store(0,
                                                    std::memory_order_relaxed);
    wi_records_array[i].sum_select_full_range_join.store(
        0, std::memory_order_relaxed);
    wi_records_array[i].sum_select_range.store(0, std::memory_order_relaxed);
    wi_records_array[i].sum_select_range_check.store(0,
                                                      std::memory_order_relaxed);
    wi_records_array[i].sum_select_scan.store(0, std::memory_order_relaxed);
    wi_records_array[i].sum_sort_merge_passes.store(0,
                                                     std::memory_order_relaxed);
    wi_records_array[i].sum_sort_range.store(0, std::memory_order_relaxed);
    wi_records_array[i].sum_sort_rows.store(0, std::memory_order_relaxed);
    wi_records_array[i].sum_sort_scan.store(0, std::memory_order_relaxed);
    wi_records_array[i].sum_no_index_used.store(0, std::memory_order_relaxed);
    wi_records_array[i].sum_no_good_index_used.store(0,
                                                      std::memory_order_relaxed);
  }
  wi_workload_map.clear();
  wi_init_overflow_record(&wi_records_array[WI_OVERFLOW_SLOT]);
  wi_workload_map[std::string(WI_OVERFLOW_NAME, WI_OVERFLOW_NAME_LEN)] =
      WI_OVERFLOW_SLOT;
  wi_rows_in_table = 1;
  wi_next_available_index = 1;
  mysql_rwlock_unlock(&wi_rwlock);

  return 0;
}

/* Returns the number of currently occupied slots in the table. */
unsigned long long wi_get_row_count(void) { return wi_rows_in_table; }

/*
  init_wi_share() — populate the PFS_engine_table_share_proxy with
  everything PFS needs to expose the performance_schema.workload_instrumentation table.

  - m_table_name / m_table_name_length: the table appears as
    performance_schema.workload_instrumentation.
  - m_table_definition: SQL-like column definitions string parsed by PFS
    to create the table's column metadata. Includes KEY(`WORKLOAD_NAME`)
    for index support.
  - m_ref_length: size of the position structure (WI_POS) so PFS can
    save/restore cursor positions.
  - m_acl = TRUNCATABLE: allows SELECT and TRUNCATE TABLE, but not
    INSERT/UPDATE/DELETE.
  - m_proxy_engine_table: callback function pointers including index callbacks.
*/
void init_wi_share(PFS_engine_table_share_proxy *share) {
  share->m_table_name = "workload_instrumentation";
  share->m_table_name_length = 24;
  share->m_table_definition =
      "WORKLOAD_NAME VARCHAR(256) NOT NULL, "
      "COUNT_STAR BIGINT UNSIGNED NOT NULL, "
      "SUM_TIMER_WAIT BIGINT UNSIGNED NOT NULL, "
      "SUM_CPU_TIME BIGINT UNSIGNED NOT NULL, "
      "SUM_LOCK_TIME BIGINT UNSIGNED NOT NULL, "
      "SUM_ROWS_AFFECTED BIGINT UNSIGNED NOT NULL, "
      "SUM_ROWS_EXAMINED BIGINT UNSIGNED NOT NULL, "
      "SUM_ROWS_SENT BIGINT UNSIGNED NOT NULL, "
      "SUM_CREATED_TMP_DISK_TABLES BIGINT UNSIGNED NOT NULL, "
      "SUM_CREATED_TMP_TABLES BIGINT UNSIGNED NOT NULL, "
      "SUM_SELECT_FULL_JOIN BIGINT UNSIGNED NOT NULL, "
      "SUM_SELECT_FULL_RANGE_JOIN BIGINT UNSIGNED NOT NULL, "
      "SUM_SELECT_RANGE BIGINT UNSIGNED NOT NULL, "
      "SUM_SELECT_RANGE_CHECK BIGINT UNSIGNED NOT NULL, "
      "SUM_SELECT_SCAN BIGINT UNSIGNED NOT NULL, "
      "SUM_SORT_MERGE_PASSES BIGINT UNSIGNED NOT NULL, "
      "SUM_SORT_RANGE BIGINT UNSIGNED NOT NULL, "
      "SUM_SORT_ROWS BIGINT UNSIGNED NOT NULL, "
      "SUM_SORT_SCAN BIGINT UNSIGNED NOT NULL, "
      "SUM_NO_INDEX_USED BIGINT UNSIGNED NOT NULL, "
      "SUM_NO_GOOD_INDEX_USED BIGINT UNSIGNED NOT NULL, "
      "KEY(`WORKLOAD_NAME`)";
  share->m_ref_length = sizeof(WI_POS);
  share->m_acl = TRUNCATABLE;
  share->get_row_count = wi_get_row_count;
  share->delete_all_rows = wi_delete_all_rows;

  share->m_proxy_engine_table = {wi_rnd_next,
                                 wi_rnd_init,
                                 wi_rnd_pos,
                                 wi_index_init,
                                 wi_index_read,
                                 wi_index_next,
                                 wi_read_column_value,
                                 wi_reset_position,
                                 nullptr,  /* write_column_value */
                                 nullptr,  /* write_row_values */
                                 nullptr,  /* update_column_value */
                                 nullptr,  /* update_row_values */
                                 nullptr,  /* delete_row_values */
                                 wi_open_table,
                                 wi_close_table};
}

bool wi_pfs_services_initialized() { return wi_table_svc != nullptr; }

int wi_pfs_services_init(SERVICE_TYPE(registry) * reg,
                          MYSQL_PLUGIN *plugin_handle) {
  my_h_service temp_service = nullptr;
  if (!reg->acquire("pfs_plugin_table_v1", &temp_service)) {
    h_table_svc = temp_service;
    wi_table_svc =
        reinterpret_cast<SERVICE_TYPE(pfs_plugin_table_v1) *>(temp_service);
  } else {
    my_plugin_log_message(plugin_handle, MY_ERROR_LEVEL,
                          "Failed to acquire pfs_plugin_table_v1 service");
    return 1;
  }

  temp_service = nullptr;
  if (!reg->acquire("pfs_plugin_column_bigint_v1", &temp_service)) {
    h_col_bigint_svc = temp_service;
    wi_col_bigint_svc =
        reinterpret_cast<SERVICE_TYPE(pfs_plugin_column_bigint_v1) *>(
            temp_service);
  } else {
    my_plugin_log_message(plugin_handle, MY_ERROR_LEVEL,
                          "Failed to acquire pfs_plugin_column_bigint_v1");
    wi_pfs_services_deinit(reg);
    return 1;
  }

  temp_service = nullptr;
  if (!reg->acquire("pfs_plugin_column_string_v2", &temp_service)) {
    h_col_string_svc = temp_service;
    wi_col_string_svc =
        reinterpret_cast<SERVICE_TYPE(pfs_plugin_column_string_v2) *>(
            temp_service);
  } else {
    my_plugin_log_message(plugin_handle, MY_ERROR_LEVEL,
                          "Failed to acquire pfs_plugin_column_string_v2");
    wi_pfs_services_deinit(reg);
    return 1;
  }

  wi_share_list[0] = &wi_st_share;
  if (wi_table_svc->add_tables(&wi_share_list[0], wi_share_list_count)) {
    my_plugin_log_message(plugin_handle, MY_ERROR_LEVEL,
                          "Failed to add workload_instrumentation PFS table");
    wi_pfs_services_deinit(reg);
    return 1;
  }

  return 0;
}

void wi_pfs_services_deinit(SERVICE_TYPE(registry) * reg) {
  if (h_table_svc != nullptr) {
    reg->release(h_table_svc);
    h_table_svc = nullptr;
    wi_table_svc = nullptr;
  }
  if (h_col_bigint_svc != nullptr) {
    reg->release(h_col_bigint_svc);
    h_col_bigint_svc = nullptr;
    wi_col_bigint_svc = nullptr;
  }
  if (h_col_string_svc != nullptr) {
    reg->release(h_col_string_svc);
    h_col_string_svc = nullptr;
    wi_col_string_svc = nullptr;
  }
}

int wi_pfs_check_uninstall() {
  if (wi_table_svc != nullptr) {
    if (wi_table_svc->delete_tables(&wi_share_list[0], wi_share_list_count)) {
      return 1;
    }
  }
  return 0;
}