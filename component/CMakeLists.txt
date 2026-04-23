DISABLE_MISSING_PROFILE_WARNING()

MYSQL_ADD_COMPONENT(workload_instrumentation
        workload_instrumentation.cc
        workload_instrumentation_thd_stats.cc
        workload_instrumentation_pfs.cc
        MODULE_ONLY
)
