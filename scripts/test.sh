#!/usr/bin/env bash
#
# Integration tests for the workload_instrumentation PLUGIN.
#
# This is the main test entry point called by CI and `make test`.
# For component builds it delegates to test_component.sh; for plugin builds
# it installs MySQL + sysbench, loads the plugin, and runs three test phases
# with different thread counts, table sizes, and DML mixes.
#
# Required env vars: BUILD_TARGET, MYSQL_FLAVOR, MYSQL_VERSION
# Optional env vars: ARTIFACT_DIR (default: artifacts), LOG_DIR (default: /tmp/test-logs)
#
set -euo pipefail

BUILD_TARGET="${BUILD_TARGET:?}"

# Component builds use a separate test script with Python-based tests.
if [ "$BUILD_TARGET" = "component" ]; then
  exec bash "$(cd "$(dirname "$0")" && pwd)/test_component.sh"
fi

# shellcheck source=scripts/common.sh
source "$(cd "$(dirname "$0")" && pwd)/common.sh"

# ---- Install MySQL and sysbench ----

case "$MYSQL_FLAVOR" in
  oracle)  install_oracle_mysql ;;
  percona) install_percona_mysql ;;
  *)       echo "Unknown MYSQL_FLAVOR: $MYSQL_FLAVOR" >&2; exit 1 ;;
esac

curl -fsSL https://packagecloud.io/install/repositories/akopytov/sysbench/script.deb.sh | bash
apt-get install -y sysbench

# ---- Deploy plugin and start mysqld ----

detect_plugin_dir

echo "Copying plugin to $PLUGIN_DIR"
cp "${ARTIFACT_DIR}/workload_instrumentation.so" "$PLUGIN_DIR/"

start_mysqld

# Install the plugin but keep it disabled until each phase enables it.
mysql -u root -S "$MYSQL_SOCK" <<'SQL'
INSTALL PLUGIN WORKLOAD_INSTRUMENTATION SONAME 'workload_instrumentation.so';
SET GLOBAL workload_instrumentation_warnings_enabled = OFF;
SET GLOBAL workload_instrumentation_enabled = OFF;
SQL

mysql -u root -S "$MYSQL_SOCK" -e "CREATE DATABASE IF NOT EXISTS sbtest"

echo "Running sysbench prepare..."
sysbench "${SCRIPT_DIR}/sysbench_workloads.lua" \
  --mysql-socket="$MYSQL_SOCK" \
  --mysql-user=root \
  prepare

COMMON_SB_OPTS=(
  "${SCRIPT_DIR}/sysbench_workloads.lua"
  --mysql-socket="$MYSQL_SOCK"
  --mysql-user=root
  --mysql-db=sbtest
  --report-interval=0
)

# Run a single test phase: re-seed the table, configure the plugin,
# execute the sysbench workload, then call verify_results.sh to check
# that all PFS counters match the expected values.
run_phase() {
  local phase="$1"
  local threads="$2"
  local events="$3"
  local table_size="$4"
  local unknown_queries="$5"
  local selects="$6"
  local inserts="$7"
  local updates="$8"
  local deletes="$9"

  echo ""
  echo "============================================"
  echo "Phase: $phase"
  echo "  threads=$threads  events=$events  table_size=$table_size"
  echo "  unknown=$unknown_queries select=$selects insert=$inserts update=$updates delete=$deletes"
  echo "============================================"

  # Re-seed the table with 1000 known rows while the plugin is disabled,
  # so every phase starts from a clean, deterministic state.
  mysql -u root -S "$MYSQL_SOCK" -e "SET GLOBAL workload_instrumentation_enabled = OFF"
  mysql -u root -S "$MYSQL_SOCK" sbtest <<'RESEED'
DELETE FROM sbtest1;
ALTER TABLE sbtest1 AUTO_INCREMENT = 1;
RESEED
  local insert_sql=""
  local i
  for i in $(seq 1 1000); do
    insert_sql+="INSERT INTO sbtest1 (k, c, pad) VALUES ($i, 'row-$i', 'pad-$i');"
  done
  mysql -u root -S "$MYSQL_SOCK" sbtest -e "$insert_sql"

  mysql -u root -S "$MYSQL_SOCK" <<SQL
SET GLOBAL workload_instrumentation_table_size = $table_size;
SET GLOBAL workload_instrumentation_enabled = ON;
TRUNCATE TABLE performance_schema.workload_instrumentation;
SQL

  local events_per_thread=$((events / threads))

  sysbench "${COMMON_SB_OPTS[@]}" \
    --threads="$threads" \
    --events="$events" \
    --unknown-queries="$unknown_queries" \
    --selects-per-event="$selects" \
    --inserts-per-event="$inserts" \
    --updates-per-event="$updates" \
    --deletes-per-event="$deletes" \
    run

  mysql -u root -S "$MYSQL_SOCK" -e "SET GLOBAL workload_instrumentation_enabled = OFF"

  echo "Verifying $phase..."
  THREADS="$threads" \
  EVENTS_PER_THREAD="$events_per_thread" \
  UNKNOWN_QUERIES="$unknown_queries" \
  SELECTS_PER_EVENT="$selects" \
  INSERTS_PER_EVENT="$inserts" \
  UPDATES_PER_EVENT="$updates" \
  DELETES_PER_EVENT="$deletes" \
  TABLE_SIZE="$table_size" \
  PHASE="$phase" \
  bash "${SCRIPT_DIR}/verify_results.sh"
}

# ---- Test phases ----
#
# Phase 1: 4 threads, all fit in 10-slot table — verifies basic multi-thread DML.
# Phase 2: 6 threads competing for 4-slot table — verifies overflow accounting.
# Phase 3: 1 thread, heavy DML mix — verifies exact per-workload counters.
#
#            phase name            threads events tbl_sz unknown sel ins upd del
run_phase    "multi-thread-dml"    4       4      10     5       3   2   2   1
run_phase    "overflow"            6       6      4      3       2   1   1   1
run_phase    "single-thread-heavy" 1       1      20     10      5   5   3   2

echo ""
echo "All phases passed."
