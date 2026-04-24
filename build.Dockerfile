ARG UBUNTU_VERSION=22.04
FROM ubuntu:${UBUNTU_VERSION}

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake git wget ca-certificates pkg-config \
        libssl-dev libncurses-dev bison libtirpc-dev \
        libcurl4-openssl-dev libudev-dev libsasl2-dev \
        libldap2-dev libnuma-dev libreadline-dev zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

ARG MYSQL_FLAVOR=oracle
ARG MYSQL_VERSION=8.0.42
ARG MYSQL_SRC=/opt/mysql-server

RUN set -eux; \
    case "${MYSQL_FLAVOR}" in \
        oracle) \
            git clone --depth 1 --branch "mysql-${MYSQL_VERSION}" \
                https://github.com/mysql/mysql-server.git "${MYSQL_SRC}" ;; \
        percona) \
            git clone --depth 1 --branch "Percona-Server-${MYSQL_VERSION}" \
                https://github.com/percona/percona-server.git "${MYSQL_SRC}" && \
            cd "${MYSQL_SRC}" && \
            git submodule update --init --depth 1 || true ;; \
        *) echo "Unknown MYSQL_FLAVOR: ${MYSQL_FLAVOR}" && exit 1 ;; \
    esac

ENV MYSQL_FLAVOR=${MYSQL_FLAVOR}
ENV MYSQL_VERSION=${MYSQL_VERSION}
ENV MYSQL_SRC=${MYSQL_SRC}
ENV MYSQL_BUILD=${MYSQL_SRC}/build
ENV WITH_BOOST=/tmp/boost

WORKDIR /repo