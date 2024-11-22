//
// Created by Eduardo Ortega on 2024-07-09.
//

#ifndef MYSQL_WORKLOAD_INSTRUMENTATION_THD_STATS_H
#define MYSQL_WORKLOAD_INSTRUMENTATION_THD_STATS_H

#include <sys/time.h>

class THD;

struct thread_stats {
  unsigned long long int rows_examined;
  unsigned long long int rows_sent;
  unsigned long long int rows_affected;
  timeval * start_time;
  unsigned long long int ustart_time;
};

thread_stats * get_thd_row_stats(THD *thread);

#endif  // MYSQL_WORKLOAD_INSTRUMENTATION_THD_STATS_H
