#!/usr/bin/env bash
set -euo pipefail

BUILD_TARGET="${BUILD_TARGET:?}"
MYSQL_FLAVOR="${MYSQL_FLAVOR:?}"
MYSQL_VERSION="${MYSQL_VERSION:?}"
ARTIFACT_DIR="${ARTIFACT_DIR:-artifacts}"

if [ "$BUILD_TARGET" = "component" ]; then
  echo "Skipping plugin test for component build target."
  exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MYSQL_DATADIR="/tmp/mysql-data"
MYSQL_SOCK="/tmp/mysql-test.sock"
MYSQL_PORT=13306

export MYSQL_SOCK

MAJOR_VERSION=$(echo "$MYSQL_VERSION" | sed 's/^\([0-9]*\.[0-9]*\).*/\1/')

stop_system_mysql() {
  mysql -u root -e "SHUTDOWN" 2>/dev/null || true
  sleep 2
}

install_oracle_mysql() {
  apt-get update
  apt-get install -y gnupg wget lsb-release curl

  local codename lts_component
  codename=$(lsb_release -cs)

  if [ "$MAJOR_VERSION" = "8.0" ]; then
    lts_component="mysql-8.0"
  else
    lts_component="mysql-${MAJOR_VERSION}-lts"
  fi

  cat > /etc/apt/sources.list.d/mysql.list <<EOSRC
deb [trusted=yes] http://repo.mysql.com/apt/ubuntu/ ${codename} mysql-apt-config
deb [trusted=yes] http://repo.mysql.com/apt/ubuntu/ ${codename} ${lts_component}
deb [trusted=yes] http://repo.mysql.com/apt/ubuntu/ ${codename} mysql-tools
EOSRC

  apt-get update

  local pkg_version
  pkg_version=$(apt-cache madison mysql-server \
    | awk -F'|' -v ver="$MYSQL_VERSION" '$2 ~ ver {gsub(/^ +| +$/,"",$2); print $2; exit}')

  if [ -z "$pkg_version" ]; then
    echo "ERROR: Could not find mysql-server version matching $MYSQL_VERSION" >&2
    echo "Available versions:" >&2
    apt-cache madison mysql-server >&2
    exit 1
  fi

  echo "Installing mysql-server=$pkg_version"
  DEBIAN_FRONTEND=noninteractive apt-get install -y "mysql-server=${pkg_version}" "mysql-client=${pkg_version}"
  stop_system_mysql
}

install_percona_mysql() {
  apt-get update
  apt-get install -y gnupg2 curl lsb-release
  curl -fsSL https://repo.percona.com/apt/percona-release_latest.generic_all.deb -o /tmp/percona-release.deb
  DEBIAN_FRONTEND=noninteractive apt-get install -y /tmp/percona-release.deb

  local ps_series
  ps_series=$(echo "$MAJOR_VERSION" | tr -d '.')
  percona-release setup "ps${ps_series}"
  apt-get update
  DEBIAN_FRONTEND=noninteractive PERCONA_TELEMETRY_DISABLE=1 \
    apt-get install -y "percona-server-server"
  stop_system_mysql
}

install_sysbench() {
  curl -fsSL https://packagecloud.io/install/repositories/akopytov/sysbench/script.deb.sh | bash
  apt-get install -y sysbench
}

case "$MYSQL_FLAVOR" in
  oracle)  install_oracle_mysql ;;
  percona) install_percona_mysql ;;
  *)       echo "Unknown MYSQL_FLAVOR: $MYSQL_FLAVOR" >&2; exit 1 ;;
esac

install_sysbench

PLUGIN_DIR=$(mysqld --verbose --help 2>&1 | grep '^plugin-dir' | awk '{print $2}' || true)
if [ -z "$PLUGIN_DIR" ]; then
  PLUGIN_DIR="/usr/lib/mysql/plugin"
fi

echo "Copying plugin to $PLUGIN_DIR"
cp "${ARTIFACT_DIR}/workload_instrumentation.so" "$PLUGIN_DIR/"

mkdir -p "$MYSQL_DATADIR"
chown mysql:mysql "$MYSQL_DATADIR"
mysqld --user=mysql --initialize-insecure --datadir="$MYSQL_DATADIR"

mysqld --user=mysql \
  --datadir="$MYSQL_DATADIR" \
  --port="$MYSQL_PORT" \
  --socket="$MYSQL_SOCK" \
  --mysqlx=OFF \
  &

for i in $(seq 1 30); do
  if mysql -u root -S "$MYSQL_SOCK" -e "SELECT 1" &>/dev/null; then
    break
  fi
  sleep 1
done

mysql -u root -S "$MYSQL_SOCK" -e "SELECT VERSION()"

mysql -u root -S "$MYSQL_SOCK" <<'SQL'
INSTALL PLUGIN WORKLOAD_INSTRUMENTATION SONAME 'workload_instrumentation.so';
SET GLOBAL workload_instrumentation_table_size = 10;
SET GLOBAL workload_instrumentation_warnings_enabled = OFF;
SQL

mysql -u root -S "$MYSQL_SOCK" -e "CREATE DATABASE IF NOT EXISTS sbtest"

mysql -u root -S "$MYSQL_SOCK" -e "SET GLOBAL workload_instrumentation_enabled = OFF"

echo "Running sysbench prepare..."
sysbench "${SCRIPT_DIR}/sysbench_workloads.lua" \
  --mysql-socket="$MYSQL_SOCK" \
  --mysql-user=root \
  prepare

mysql -u root -S "$MYSQL_SOCK" <<'SQL'
SET GLOBAL workload_instrumentation_enabled = ON;
TRUNCATE TABLE performance_schema.workload_instrumentation;
SQL

echo "Running sysbench run..."
sysbench "${SCRIPT_DIR}/sysbench_workloads.lua" \
  --mysql-socket="$MYSQL_SOCK" \
  --mysql-user=root \
  --mysql-db=sbtest \
  --threads=1 \
  --events=1 \
  --report-interval=0 \
  run

mysql -u root -S "$MYSQL_SOCK" -e "SET GLOBAL workload_instrumentation_enabled = OFF"

echo "Verifying results..."
bash "${SCRIPT_DIR}/verify_results.sh"
