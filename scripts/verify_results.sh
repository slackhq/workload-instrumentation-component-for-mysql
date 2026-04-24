#!/usr/bin/env bash
set -euo pipefail

MYSQL_SOCK="${MYSQL_SOCK:?}"

THREADS="${THREADS:?}"
EVENTS_PER_THREAD="${EVENTS_PER_THREAD:?}"
UNKNOWN_QUERIES="${UNKNOWN_QUERIES:?}"
SELECTS_PER_EVENT="${SELECTS_PER_EVENT:?}"
INSERTS_PER_EVENT="${INSERTS_PER_EVENT:?}"
UPDATES_PER_EVENT="${UPDATES_PER_EVENT:?}"
DELETES_PER_EVENT="${DELETES_PER_EVENT:?}"
TABLE_SIZE="${TABLE_SIZE:?}"
CONNECTION_INIT_QUERIES="${CONNECTION_INIT_QUERIES:-1}"
PHASE="${PHASE:?}"

mysql_q() {
  mysql -u root -S "$MYSQL_SOCK" -N -B -e "$1"
}

TAGGED_PER_EVENT=$((SELECTS_PER_EVENT + INSERTS_PER_EVENT + UPDATES_PER_EVENT + DELETES_PER_EVENT))
TOTAL_EVENTS=$((THREADS * EVENTS_PER_THREAD))
TOTAL_TAGGED=$((TOTAL_EVENTS * TAGGED_PER_EVENT))
TOTAL_UNKNOWN_RAW=$((TOTAL_EVENTS * UNKNOWN_QUERIES))
TOTAL_INIT=$CONNECTION_INIT_QUERIES
TOTAL_UNKNOWN=$((TOTAL_UNKNOWN_RAW + TOTAL_INIT))
TOTAL_QUERIES=$((TOTAL_TAGGED + TOTAL_UNKNOWN))

TOTAL_SELECTS=$((TOTAL_EVENTS * SELECTS_PER_EVENT))
TOTAL_INSERTS=$((TOTAL_EVENTS * INSERTS_PER_EVENT))
TOTAL_UPDATES=$((TOTAL_EVENTS * UPDATES_PER_EVENT))
TOTAL_DELETES=$((TOTAL_EVENTS * DELETES_PER_EVENT))

EXPECTED_ROWS_SENT=$((TOTAL_UNKNOWN_RAW + TOTAL_SELECTS + TOTAL_INIT))
EXPECTED_ROWS_AFFECTED=$((TOTAL_INSERTS + TOTAL_UPDATES + TOTAL_DELETES))
EXPECTED_ROWS_EXAMINED=$((TOTAL_UNKNOWN_RAW + TOTAL_SELECTS + TOTAL_UPDATES + TOTAL_DELETES + TOTAL_INIT))

errors=0

check() {
  local label="$1" expected="$2" actual="$3"
  if [ "$actual" != "$expected" ]; then
    echo "FAIL: [$PHASE] $label — expected $expected, got $actual"
    errors=$((errors + 1))
  else
    echo "OK:   [$PHASE] $label = $actual"
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
if [ "$actual_rows" -lt 2 ] || [ "$actual_rows" -gt "$TABLE_SIZE" ]; then
  echo "FAIL: [$PHASE] number of rows — expected 2..$TABLE_SIZE, got $actual_rows"
  errors=$((errors + 1))
else
  echo "OK:   [$PHASE] number of rows = $actual_rows (in 2..$TABLE_SIZE)"
fi

actual_overflow=$(mysql_q "
  SELECT COUNT_STAR
  FROM performance_schema.workload_instrumentation
  WHERE WORKLOAD_NAME = '<overflow>'
")

actual_unknown=$(mysql_q "
  SELECT IFNULL(SUM(COUNT_STAR), 0)
  FROM performance_schema.workload_instrumentation
  WHERE WORKLOAD_NAME = '<unknown>'
")

actual_named_total=$(mysql_q "
  SELECT IFNULL(SUM(COUNT_STAR), 0)
  FROM performance_schema.workload_instrumentation
  WHERE WORKLOAD_NAME NOT IN ('<overflow>', '<unknown>')
")

named_rows=$(mysql_q "
  SELECT COUNT(*)
  FROM performance_schema.workload_instrumentation
  WHERE WORKLOAD_NAME NOT IN ('<overflow>', '<unknown>')
")

check "tagged + unknown + overflow" "$TOTAL_QUERIES" "$((actual_named_total + actual_unknown + actual_overflow))"

USABLE_SLOTS=$((TABLE_SIZE - 1))

if [ "$THREADS" -le "$((USABLE_SLOTS - 1))" ]; then
  check "named workload rows" "$THREADS" "$named_rows"
  check "<overflow> COUNT_STAR" 0 "$actual_overflow"
  check "<unknown> COUNT_STAR" "$TOTAL_UNKNOWN" "$actual_unknown"

  for i in $(seq 1 "$THREADS"); do
    wname=$(printf "workload_%02d" "$i")
    actual_wl=$(mysql_q "
      SELECT COUNT_STAR
      FROM performance_schema.workload_instrumentation
      WHERE WORKLOAD_NAME = '$wname'
    ")
    expected_wl=$((EVENTS_PER_THREAD * TAGGED_PER_EVENT))
    check "$wname COUNT_STAR" "$expected_wl" "$actual_wl"
  done
else
  echo "INFO: [$PHASE] overflow scenario — $THREADS workloads competing for $USABLE_SLOTS usable slots"
  if [ "$named_rows" -lt 1 ]; then
    echo "FAIL: [$PHASE] named workload rows — expected >= 1, got $named_rows"
    errors=$((errors + 1))
  else
    echo "OK:   [$PHASE] named workload rows = $named_rows"
  fi
  if [ "$actual_overflow" -lt 1 ]; then
    echo "FAIL: [$PHASE] <overflow> COUNT_STAR — expected >= 1, got $actual_overflow"
    errors=$((errors + 1))
  else
    echo "OK:   [$PHASE] <overflow> COUNT_STAR = $actual_overflow"
  fi

  per_named_workload=$((EVENTS_PER_THREAD * TAGGED_PER_EVENT))
  named_in_table_count=$((actual_named_total / per_named_workload))
  echo "INFO: [$PHASE] $named_in_table_count named workloads got their own slot (each with $per_named_workload queries)"

  named_that_overflow=$((THREADS - named_in_table_count))
  unknown_in_overflow=0
  if [ "$actual_unknown" -eq 0 ]; then
    unknown_in_overflow=$TOTAL_UNKNOWN
  fi
  expected_overflow=$(( (named_that_overflow * per_named_workload) + unknown_in_overflow ))
  check "<overflow> COUNT_STAR" "$expected_overflow" "$actual_overflow"
fi

actual_sent=$(mysql_q "
  SELECT SUM(SUM_ROWS_SENT)
  FROM performance_schema.workload_instrumentation
")
check "total SUM_ROWS_SENT" "$EXPECTED_ROWS_SENT" "$actual_sent"

actual_affected=$(mysql_q "
  SELECT SUM(SUM_ROWS_AFFECTED)
  FROM performance_schema.workload_instrumentation
")
check "total SUM_ROWS_AFFECTED" "$EXPECTED_ROWS_AFFECTED" "$actual_affected"

actual_examined=$(mysql_q "
  SELECT SUM(SUM_ROWS_EXAMINED)
  FROM performance_schema.workload_instrumentation
")
check "total SUM_ROWS_EXAMINED" "$EXPECTED_ROWS_EXAMINED" "$actual_examined"

if [ "$errors" -gt 0 ]; then
  echo ""
  echo "=== full table dump ==="
  mysql_q "SELECT * FROM performance_schema.workload_instrumentation"
  exit 1
fi

echo ""
echo "[$PHASE] All checks passed."