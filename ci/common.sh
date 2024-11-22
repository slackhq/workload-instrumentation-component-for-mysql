#!/usr/bin/env bash

MYSQLD_MAJOR_VERSION="${MYSQLD_MAJOR_VERSION:-8.4}"

function install_mysql {
  mkdir -p build
  MYSQLD_MAJOR_VERSION="${1}"
  echo Installing MySQL ${MYSQLD_MAJOR_VERSION}
  apt-get update
  DEBIAN_FRONTEND="noninteractive" apt install -y gnupg lsb-release wget sudo
  apt-key adv --keyserver keyserver.ubuntu.com --recv-keys A8D3785C
  wget -c https://repo.mysql.com//mysql-apt-config.deb -P build
  echo mysql-apt-config mysql-apt-config/select-server select "mysql-${MYSQLD_MAJOR_VERSION}-lts" | debconf-set-selections
  DEBIAN_FRONTEND="noninteractive" dpkg -i build/mysql-apt-config*
  apt-get update
  DEBIAN_FRONTEND="noninteractive" apt-get install -y mysql-server mysql-client
}

