#!/usr/bin/env bash

set -exo pipefail

SCRIPTS_PATH=$(dirname $(realpath "$0"))
source "${SCRIPTS_PATH}/common.sh"

# Install requirements
apt-get update
DEBIAN_FRONTEND="noninteractive" apt-get install -y build-essential cmake git wget lsb-release libssl-dev libncurses5-dev pkg-config libtirpc-dev bison libz-dev

# Install Latest MySQL
install_mysql "${MYSQLD_MAJOR_VERSION}"

# Get the exact minor version so we can download the corresponding source code
MYSQLD_MINOR_VERSION=$(mysqld --version | sed -r 's/.* ([0-9]+\.[0-9]+\.[0-9]+) .*/\1/g')

# Get source code corresponding to the version we just installed so we can compile the component against it
git clone --depth 1 --branch "mysql-${MYSQLD_MINOR_VERSION}" https://github.com/mysql/mysql-server.git /tmp/mysql-server
ln -s "${PWD}/workload_instrumentation/" /tmp/mysql-server/components/
ORIGINAL_DIR=`pwd`
cd /tmp/mysql-server

# Compile
cmake . -DDOWNLOAD_BOOST=1 -DWITH_BOOST=build/boost -DBISON_EXECUTABLE=/usr/bin/bison  -B build
cmake  --build build/  -t component_workload_instrumentation -j 4

# Store the resulting library
cp /tmp/mysql-server/build/plugin_output_directory/component_workload_instrumentation.so "${ORIGINAL_DIR}"/build