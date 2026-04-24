#!/usr/bin/env bash
set -euo pipefail

MYSQL_FLAVOR="${MYSQL_FLAVOR:?}"
MYSQL_VERSION="${MYSQL_VERSION:?}"
ARTIFACT_DIR="${ARTIFACT_DIR:-artifacts}"
LOG_DIR="${LOG_DIR:-/tmp/test-logs}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MYSQL_DATADIR="/tmp/mysql-data"
MYSQL_SOCK="/tmp/mysql-test.sock"
MYSQL_PORT=13306

export MYSQL_SOCK

mkdir -p "$LOG_DIR"

MYSQL_ERROR_LOG="$MYSQL_DATADIR/error.log"

collect_logs() {
  cp "$MYSQL_ERROR_LOG" "$LOG_DIR/" 2>/dev/null || true
  chmod -R a+r "$LOG_DIR" 2>/dev/null || true
  echo "MySQL logs saved to $LOG_DIR"
}
trap collect_logs EXIT

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

case "$MYSQL_FLAVOR" in
  oracle)  install_oracle_mysql ;;
  *)       echo "Unknown MYSQL_FLAVOR for component: $MYSQL_FLAVOR" >&2; exit 1 ;;
esac

DEBIAN_FRONTEND=noninteractive apt-get install -y python3-pip python3-venv

PLUGIN_DIR=$(mysqld --verbose --help 2>&1 | grep '^plugin-dir' | awk '{print $2}' || true)
if [ -z "$PLUGIN_DIR" ]; then
  PLUGIN_DIR="/usr/lib/mysql/plugin"
fi

echo "Copying component to $PLUGIN_DIR"
cp "${ARTIFACT_DIR}/component_workload_instrumentation.so" "$PLUGIN_DIR/"

mkdir -p "$MYSQL_DATADIR"
chown mysql:mysql "$MYSQL_DATADIR"
mysqld --user=mysql --initialize-insecure --datadir="$MYSQL_DATADIR"

mysqld --user=mysql \
  --datadir="$MYSQL_DATADIR" \
  --port="$MYSQL_PORT" \
  --socket="$MYSQL_SOCK" \
  --mysqlx=OFF \
  --log-error="$MYSQL_ERROR_LOG" \
  &

for i in $(seq 1 30); do
  if mysql -u root -S "$MYSQL_SOCK" -e "SELECT 1" &>/dev/null; then
    break
  fi
  sleep 1
done

mysql -u root -S "$MYSQL_SOCK" -e "SELECT VERSION()"

mysql -u root -S "$MYSQL_SOCK" -e "CREATE DATABASE IF NOT EXISTS testdb"
mysql -u root -S "$MYSQL_SOCK" testdb -e '
  CREATE TABLE IF NOT EXISTS test_table (
    id bigint unsigned NOT NULL AUTO_INCREMENT,
    content char(5) DEFAULT NULL,
    PRIMARY KEY (id)
  ) ENGINE=InnoDB
'

python3 -m venv /tmp/tests-venv
. /tmp/tests-venv/bin/activate
pip3 install "mysql-connector-python==${MAJOR_VERSION}"

echo "Running component integration tests..."
python3 -m unittest -v "${SCRIPT_DIR}/integration_tests.py"
