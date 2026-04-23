#!/usr/bin/env bash
set -euo pipefail

BUILD_TARGET="${BUILD_TARGET:?Set BUILD_TARGET to 'plugin' or 'component'}"
MYSQL_FLAVOR="${MYSQL_FLAVOR:?Set MYSQL_FLAVOR to 'oracle' or 'percona'}"
MYSQL_VERSION="${MYSQL_VERSION:?Set MYSQL_VERSION (e.g. 8.0.42, 8.4.5)}"
MYSQL_SRC="${MYSQL_SRC:?Set MYSQL_SRC to the MySQL source tree root}"
MYSQL_BUILD="${MYSQL_BUILD:-${MYSQL_SRC}/build}"
CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-RelWithDebInfo}"
NPROC="${NPROC:-$(nproc)}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

case "${BUILD_TARGET}" in
    plugin)
        LINK_SRC="${REPO_ROOT}/plugin"
        LINK_DST="${MYSQL_SRC}/plugin/query_metrics"
        MAKE_TARGET="query_metrics"
        SO_NAME="query_metrics.so"
        ;;
    component)
        LINK_SRC="${REPO_ROOT}/component"
        LINK_DST="${MYSQL_SRC}/components/workload_instrumentation"
        MAKE_TARGET="component_workload_instrumentation"
        SO_NAME="component_workload_instrumentation.so"
        ;;
    *)
        echo "ERROR: BUILD_TARGET must be 'plugin' or 'component'" >&2
        exit 1
        ;;
esac

if [ ! -e "${LINK_DST}" ]; then
    ln -sfn "${LINK_SRC}" "${LINK_DST}"
fi

BOOST_DIR="${WITH_BOOST:-/tmp/boost}"
BOOST_PKG=$(sed -n 's/^SET(BOOST_PACKAGE_NAME "\(.*\)")/\1/p' "${MYSQL_SRC}/cmake/boost.cmake")
if [ -n "${BOOST_PKG}" ] && [ ! -d "${BOOST_DIR}/${BOOST_PKG}" ]; then
    mkdir -p "${BOOST_DIR}"
    BOOST_VER=$(echo "${BOOST_PKG}" | sed 's/boost_//;s/_/./g')
    BOOST_URL="https://archives.boost.io/release/${BOOST_VER}/source/${BOOST_PKG}.tar.bz2"
    echo "Pre-downloading ${BOOST_PKG} from ${BOOST_URL}"
    wget -q -O "${BOOST_DIR}/${BOOST_PKG}.tar.bz2" "${BOOST_URL}"
    tar xf "${BOOST_DIR}/${BOOST_PKG}.tar.bz2" -C "${BOOST_DIR}"
fi

mkdir -p "${MYSQL_BUILD}"
cd "${MYSQL_BUILD}"

if [ ! -f "Makefile" ]; then
    cmake_args=(
        "${MYSQL_SRC}"
        -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE}"
        -DWITH_BOOST="${BOOST_DIR}"
        -DWITHOUT_SERVER=OFF
    )

    if [ "${MYSQL_FLAVOR}" = "percona" ]; then
        cmake_args+=(
            -DWITH_TOKUDB=OFF
            -DWITH_ROCKSDB=OFF
            -DWITH_AUTHENTICATION_LDAP=OFF
            -DWITH_AUTHENTICATION_KERBEROS=OFF
        )
    fi

    cmake "${cmake_args[@]}"
fi

make -j"${NPROC}" "${MAKE_TARGET}"

echo "Built: ${MYSQL_BUILD}/plugin_output_directory/${SO_NAME}"