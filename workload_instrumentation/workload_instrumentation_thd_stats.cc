#include "workload_instrumentation_thd_stats.h"
#include <sql/sql_class.h>

thread_stats* get_thd_row_stats(THD *thread) {
  auto ts = new thread_stats;
  ts->rows_examined = thread->get_examined_row_count();
  ts->rows_sent = thread->get_sent_row_count();
  ts->rows_affected = thread->get_stmt_da()->affected_rows();

  ts->start_time = &thread->start_time;
  ts->ustart_time = thread->start_utime;

  return ts;
}
