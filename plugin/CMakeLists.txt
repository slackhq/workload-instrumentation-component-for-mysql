MYSQL_ADD_PLUGIN(workload_instrumentation
        workload_instrumentation.cc
        workload_instrumentation_table.cc
        workload_instrumentation_vars.cc
        workload_instrumentation_workload.cc
        workload_instrumentation_socket.cc
        query_stats.cc
  MODULE_ONLY
  MODULE_OUTPUT_NAME "workload_instrumentation"
  LINK_LIBRARIES extra::rapidjson
  )