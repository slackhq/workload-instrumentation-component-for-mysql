#!/usr/bin/env bash
set -euo pipefail

MYSQL_SOCK="${MYSQL_SOCK:?}"

mysql_q() {
  mysql -u root -S "$MYSQL_SOCK" -N -B -e "$1"
}

QUERIES_PER_WORKLOAD=10
NUM_NAMED_WORKLOADS=15
NUM_UNKNOWN=20
TABLE_SIZE=10
CONNECTION_INIT_QUERIES=1

TOTAL_NAMED=$((NUM_NAMED_WORKLOADS * QUERIES_PER_WORKLOAD))
TOTAL_QUERIES=$((TOTAL_NAMED + NUM_UNKNOWN + CONNECTION_INIT_QUERIES))

USABLE_SLOTS=$((TABLE_SIZE - 1))
UNKNOWN_TAKES_SLOT=1
NAMED_THAT_FIT=$((USABLE_SLOTS - UNKNOWN_TAKES_SLOT))
NAMED_THAT_OVERFLOW=$((NUM_NAMED_WORKLOADS - NAMED_THAT_FIT))
EXPECTED_OVERFLOW=$((NAMED_THAT_OVERFLOW * QUERIES_PER_WORKLOAD))
EXPECTED_UNKNOWN=$((NUM_UNKNOWN + CONNECTION_INIT_QUERIES))

errors=0

check() {
  local label="$1" expected="$2" actual="$3"
  if [ "$actual" != "$expected" ]; then
    echo "FAIL: $label — expected $expected, got $actual"
    errors=$((errors + 1))
  else
    echo "OK:   $label = $actual"
  fi
}

actual_total=$(mysql_q "
  SELECT SUM(COUNT_STAR)
  FROM performance_schema.workload_instrumentation
")
check "total COUNT_STAR" "$TOTAL_QUERIES" "$actual_total"

actual_rows=$(mysql_q "
  SELECT COUNT(*)
  FROM performance_schema.workload_instrumentation
")
check "number of rows" "$TABLE_SIZE" "$actual_rows"

actual_overflow=$(mysql_q "
  SELECT COUNT_STAR
  FROM performance_schema.workload_instrumentation
  WHERE WORKLOAD_NAME = '<overflow>'
")
check "<overflow> COUNT_STAR" "$EXPECTED_OVERFLOW" "$actual_overflow"

actual_unknown=$(mysql_q "
  SELECT COUNT_STAR
  FROM performance_schema.workload_instrumentation
  WHERE WORKLOAD_NAME = '<unknown>'
")
check "<unknown> COUNT_STAR" "$EXPECTED_UNKNOWN" "$actual_unknown"

if [ "$errors" -gt 0 ]; then
  echo ""
  echo "=== full table dump ==="
  mysql_q "SELECT * FROM performance_schema.workload_instrumentation"
  exit 1
fi

echo ""
echo "All checks passed."
