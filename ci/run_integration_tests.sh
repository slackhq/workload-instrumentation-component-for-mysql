#!/usr/bin/env bash

set -exo pipefail

SCRIPTS_PATH=$(dirname $(realpath "$0"))
source "${SCRIPTS_PATH}/common.sh"

function deploy_component {
  cp build/component_workload_instrumentation.so /usr/lib/mysql/plugin/
}

function init_mysql {
  sudo -u mysql mkdir -p /tmp/data
  sudo -u mysql mysqld --datadir /tmp/data --initialize-insecure
}

function run_mysql {
  sudo -u mysql mysqld --datadir /tmp/data --port 1234 --socket /tmp/data/mysql.sock
}

function create_test_data {
  mysql -u root -S /tmp/data/mysql.sock -e "CREATE DATABASE IF NOT EXISTS testdb;"
  mysql -u root -S /tmp/data/mysql.sock testdb -e 'CREATE TABLE IF NOT EXISTS `test_table` (
    `id` bigint unsigned NOT NULL AUTO_INCREMENT,
    `content` char(5) DEFAULT NULL,
    PRIMARY KEY (`id`)
  ) ENGINE=InnoDB'

  for I in $(seq 1 14); do
    mysql -u root -S /tmp/data/mysql.sock testdb -e "INSERT INTO test_table VALUES (${I}, 'cc')"
  done

  mysql -u root -S /tmp/data/mysql.sock testdb -e "UPDATE test_table SET content='dd' WHERE id BETWEEN 6 AND 9"
}

install_mysql "${MYSQLD_MAJOR_VERSION}"
DEBIAN_FRONTEND="noninteractive" apt-get install -y python3-pip python3-venv
init_mysql
deploy_component
run_mysql &
sleep 10s
create_test_data

python3 -m venv /tmp/tests-venv/
source /tmp/tests-venv/bin/activate
pip3 install mysql-connector-python=="${MYSQLD_MAJOR_VERSION}"

python3 "${SCRIPTS_PATH}/integration_tests.py"
