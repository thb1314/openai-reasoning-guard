#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

TARGET="${TARGET:-windows-x86_64}"
QTBASE_SOURCE_ARCHIVE="${QTBASE_SOURCE_ARCHIVE:-/mnt/data/qt-2080ti-sync/archives/qtbase-everywhere-src-5.15.2.tar.xz}"
OPENSSL_SOURCE_ARCHIVE="${OPENSSL_SOURCE_ARCHIVE:-/mnt/data/qt-2080ti-sync/archives/openssl-1.1.1w.tar.gz}"
OUTPUT_ROOT="${OUTPUT_ROOT:-${PROJECT_DIR}/dist/qt-sdk-build}"
BUILD_DIR="${BUILD_DIR:-}"
PREFIX="${PREFIX:-}"
MINGW_TRIPLE="${MINGW_TRIPLE:-}"
MINGW_BIN_DIR="${MINGW_BIN_DIR:-}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
CLEAN="${CLEAN:-0}"
SKIP_DEPS="${SKIP_DEPS:-0}"
ARCHIVE="${ARCHIVE:-0}"
UPLOAD="${UPLOAD:-0}"
SET_SECRET="${SET_SECRET:-0}"
UPLOAD_PROXY="${UPLOAD_PROXY:-}"
REPO="${REPO:-thb1314/openai-reasoning-guard}"
RELEASE_TAG="${RELEASE_TAG:-}"
SECRET_NAME="${SECRET_NAME:-}"
ORIGINAL_PATH="${PATH}"

usage() {
    cat <<EOF
Usage: $(basename "$0") --target windows-x86_64 [--archive] [--upload] [--set-secret]

Build a Windows MinGW Qt 5.15.x SDK from qtbase source on Linux. The resulting SDK
contains Linux host tools (moc/rcc/uic) and Windows target DLL/import libs, so
GitHub Actions can cross-build Windows packages from an Ubuntu runner.

Targets:
  windows-x86_64  MinGW triple x86_64-w64-mingw32, secret QT_WINDOWS_X86_64_URL
  windows-x86_32  MinGW triple i686-w64-mingw32, secret QT_WINDOWS_X86_32_URL

Examples:
  scripts/build-qt5-windows-mingw-sdk.sh --target windows-x86_64 --archive
  scripts/build-qt5-windows-mingw-sdk.sh --target windows-x86_64 --archive --upload --set-secret --upload-proxy http://127.0.0.1:7890

Environment overrides:
  QTBASE_SOURCE_ARCHIVE=${QTBASE_SOURCE_ARCHIVE}
  OPENSSL_SOURCE_ARCHIVE=${OPENSSL_SOURCE_ARCHIVE}
  MINGW_BIN_DIR=/path/to/mingw/bin
  MINGW_TRIPLE=x86_64-w64-mingw32
  OUTPUT_ROOT=${OUTPUT_ROOT}
  BUILD_DIR=${BUILD_DIR}
  PREFIX=${PREFIX}
  JOBS=${JOBS}
EOF
}

while (($# > 0)); do
    case "$1" in
        --target)
            shift
            TARGET="${1:?missing target}"
            ;;
        --qtbase-source-archive)
            shift
            QTBASE_SOURCE_ARCHIVE="${1:?missing qtbase source archive}"
            ;;
        --openssl-source-archive)
            shift
            OPENSSL_SOURCE_ARCHIVE="${1:?missing openssl source archive}"
            ;;
        --output-root)
            shift
            OUTPUT_ROOT="${1:?missing output root}"
            ;;
        --build-dir)
            shift
            BUILD_DIR="${1:?missing build dir}"
            ;;
        --prefix)
            shift
            PREFIX="${1:?missing prefix}"
            ;;
        --mingw-bin-dir)
            shift
            MINGW_BIN_DIR="${1:?missing mingw bin dir}"
            ;;
        --mingw-triple)
            shift
            MINGW_TRIPLE="${1:?missing mingw triple}"
            ;;
        --jobs)
            shift
            JOBS="${1:?missing jobs}"
            ;;
        --skip-deps)
            SKIP_DEPS=1
            ;;
        --clean)
            CLEAN=1
            ;;
        --archive)
            ARCHIVE=1
            ;;
        --upload)
            UPLOAD=1
            ;;
        --set-secret)
            SET_SECRET=1
            ;;
        --upload-proxy)
            shift
            UPLOAD_PROXY="${1:?missing upload proxy}"
            ;;
        --repo)
            shift
            REPO="${1:?missing repo}"
            ;;
        --release-tag)
            shift
            RELEASE_TAG="${1:?missing release tag}"
            ;;
        --secret-name)
            shift
            SECRET_NAME="${1:?missing secret name}"
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

case "${TARGET}" in
    windows-x86_64)
        MINGW_TRIPLE="${MINGW_TRIPLE:-x86_64-w64-mingw32}"
        APT_MINGW_PACKAGE="g++-mingw-w64-x86-64-posix"
        OPENSSL_TARGET="mingw64"
        ;;
    windows-x86_32)
        MINGW_TRIPLE="${MINGW_TRIPLE:-i686-w64-mingw32}"
        APT_MINGW_PACKAGE="g++-mingw-w64-i686-posix"
        OPENSSL_TARGET="mingw"
        ;;
    *)
        echo "unsupported target: ${TARGET}" >&2
        exit 2
        ;;
esac

BUILD_DIR="${BUILD_DIR:-${OUTPUT_ROOT}/build-${TARGET}}"
PREFIX="${PREFIX:-${OUTPUT_ROOT}/qt5-${TARGET}}"
OPENSSL_PREFIX="${BUILD_DIR}/openssl-install"
RELEASE_TAG="${RELEASE_TAG:-qt-sdk-${TARGET}}"

require_file() {
    local path="$1"
    if [[ ! -f "${path}" ]]; then
        echo "required file missing: ${path}" >&2
        exit 2
    fi
}

install_deps() {
    if ((SKIP_DEPS == 1)); then
        return
    fi
    if ! command -v apt-get >/dev/null 2>&1; then
        echo "apt-get not available; install build deps manually or pass --skip-deps" >&2
        exit 2
    fi
    export DEBIAN_FRONTEND=noninteractive
    local sudo_cmd=()
    if [[ "${EUID}" -ne 0 ]]; then
        sudo_cmd=(sudo)
    fi
    "${sudo_cmd[@]}" apt-get update
    "${sudo_cmd[@]}" apt-get install -y --no-install-recommends \
        bash \
        build-essential \
        ca-certificates \
        gperf \
        "${APT_MINGW_PACKAGE}" \
        make \
        patch \
        perl \
        python3 \
        ruby \
        tar \
        xz-utils \
        zlib1g-dev
}

find_tool() {
    local name="$1"
    if [[ -n "${MINGW_BIN_DIR}" && -x "${MINGW_BIN_DIR}/${MINGW_TRIPLE}-${name}" ]]; then
        printf '%s\n' "${MINGW_BIN_DIR}/${MINGW_TRIPLE}-${name}"
        return
    fi
    if command -v "${MINGW_TRIPLE}-${name}" >/dev/null 2>&1; then
        command -v "${MINGW_TRIPLE}-${name}"
        return
    fi
    return 1
}

find_tool_any() {
    local name
    for name in "$@"; do
        if find_tool "${name}"; then
            return 0
        fi
    done
    return 1
}

prepare_cross_prefix() {
    CROSS_BIN_DIR="${BUILD_DIR}/cross-bin"
    rm -rf "${CROSS_BIN_DIR}"
    mkdir -p "${CROSS_BIN_DIR}"

    local tool source
    for tool in gcc g++ cc c++ as ar ranlib windres strip nm objcopy objdump dlltool ld; do
        case "${tool}" in
            gcc|cc) source="$(find_tool_any gcc-posix gcc || true)" ;;
            g++|c++) source="$(find_tool_any g++-posix g++ || true)" ;;
            *) source="$(find_tool "${tool}" || true)" ;;
        esac
        if [[ -z "${source}" ]]; then
            echo "Unable to find MinGW tool for ${tool}" >&2
            exit 2
        fi
        ln -sf "${source}" "${CROSS_BIN_DIR}/${tool}"
        ln -sf "${source}" "${CROSS_BIN_DIR}/${MINGW_TRIPLE}-${tool}"
    done
    MINGW_PREFIX="${CROSS_BIN_DIR}/${MINGW_TRIPLE}-"
    export PATH="${CROSS_BIN_DIR}:${PATH}"
}

extract_one() {
    local archive="$1"
    local dest="$2"
    rm -rf "${dest}"
    mkdir -p "${dest}"
    tar -xf "${archive}" -C "${dest}" --strip-components=1
}

patch_qtbase_for_modern_mingw() {
    local source_dir="$1"
    local qfloat16_header="${source_dir}/src/corelib/global/qfloat16.h"
    if [[ -f "${qfloat16_header}" ]] && ! grep -q '#include <limits>' "${qfloat16_header}"; then
        sed -i '/#include <QtCore\/qglobal.h>/a #include <limits>' "${qfloat16_header}"
    fi

    local qt_mouse_src="${source_dir}/src/plugins/platforms/windows/qwindowsmousehandler.cpp"
    if [[ -f "${qt_mouse_src}" ]]; then
        sed -i 's/#if defined(Q_CC_MINGW) || !defined(TOUCHEVENTF_MOVE)/#if !defined(TOUCHEVENTF_MOVE)/' "${qt_mouse_src}"
        sed -i 's/#endif \/\/ if defined(Q_CC_MINGW) || !defined(TOUCHEVENTF_MOVE)/#endif \/\/ !defined(TOUCHEVENTF_MOVE)/' "${qt_mouse_src}"
    fi
}

build_openssl() {
    local source_dir="${BUILD_DIR}/openssl-src"
    extract_one "${OPENSSL_SOURCE_ARCHIVE}" "${source_dir}"
    (
        cd "${source_dir}"
        CROSS_COMPILE="${MINGW_PREFIX}" ./Configure "${OPENSSL_TARGET}" \
            --prefix="${OPENSSL_PREFIX}" \
            --openssldir="${OPENSSL_PREFIX}/ssl" \
            shared \
            no-ssl3 \
            no-comp
        make -j"${JOBS}"
        make install_sw
    )
}

build_qtbase() {
    local source_dir="${BUILD_DIR}/qtbase-src"
    local qt_build="${BUILD_DIR}/qtbase-build"
    extract_one "${QTBASE_SOURCE_ARCHIVE}" "${source_dir}"
    patch_qtbase_for_modern_mingw "${source_dir}"

    local mingw_libdir=""
    if [[ -n "${MINGW_BIN_DIR}" ]]; then
        if [[ -d "$(cd -- "${MINGW_BIN_DIR}/.." && pwd)/${MINGW_TRIPLE}/lib" ]]; then
            mingw_libdir="$(cd -- "${MINGW_BIN_DIR}/../${MINGW_TRIPLE}/lib" && pwd)"
        fi
    fi
    if [[ -z "${mingw_libdir}" && -d "/usr/${MINGW_TRIPLE}/lib" ]]; then
        mingw_libdir="/usr/${MINGW_TRIPLE}/lib"
    fi
    if [[ -n "${mingw_libdir}" && -f "${mingw_libdir}/libversion.a" && ! -e "${mingw_libdir}/libVersion.a" ]]; then
        ln -s libversion.a "${mingw_libdir}/libVersion.a" || true
    fi

    rm -rf "${qt_build}"
    mkdir -p "${qt_build}"
    (
        cd "${qt_build}"
        export PATH="${ORIGINAL_PATH}:${CROSS_BIN_DIR}"
        "${source_dir}/configure" \
            -opensource \
            -confirm-license \
            -release \
            -shared \
            -no-pch \
            -c++std c++11 \
            -opengl desktop \
            -prefix "${PREFIX}" \
            -platform linux-g++ \
            -xplatform win32-g++ \
            -device-option CROSS_COMPILE="${MINGW_PREFIX}" \
            -nomake tests \
            -nomake examples \
            -make libs \
            -make tools \
            -no-icu \
            -qt-zlib \
            -qt-pcre \
            -qt-libpng \
            -qt-libjpeg \
            -qt-freetype \
            -qt-harfbuzz \
            -openssl-runtime \
            -I "${OPENSSL_PREFIX}/include" \
            -L "${OPENSSL_PREFIX}/lib"
        make -j"${JOBS}"
        make install
    )
}

copy_runtime_dlls() {
    mkdir -p "${PREFIX}/runtime/mingw"
    shopt -s nullglob
    cp -f "${OPENSSL_PREFIX}/bin"/*.dll "${PREFIX}/bin/" 2>/dev/null || true

    local compiler libgcc_dir
    compiler="$(find_tool_any g++-posix g++ || true)"
    if [[ -n "${compiler}" ]]; then
        libgcc_dir="$("${compiler}" -print-libgcc-file-name 2>/dev/null || true)"
        if [[ -n "${libgcc_dir}" ]]; then
            cp -f "$(dirname -- "${libgcc_dir}")"/*.dll "${PREFIX}/runtime/mingw/" 2>/dev/null || true
        fi
    fi
    if [[ -d "/usr/${MINGW_TRIPLE}/lib" ]]; then
        cp -f "/usr/${MINGW_TRIPLE}/lib"/*.dll "${PREFIX}/runtime/mingw/" 2>/dev/null || true
    fi
    shopt -u nullglob
}

validate_sdk() {
    local required=(
        "${PREFIX}/bin/moc"
        "${PREFIX}/bin/rcc"
        "${PREFIX}/bin/uic"
        "${PREFIX}/bin/Qt5Core.dll"
        "${PREFIX}/bin/Qt5Network.dll"
        "${PREFIX}/bin/Qt5Gui.dll"
        "${PREFIX}/bin/Qt5Widgets.dll"
        "${PREFIX}/plugins/platforms/qwindows.dll"
        "${PREFIX}/lib/cmake/Qt5/Qt5Config.cmake"
    )
    local path
    for path in "${required[@]}"; do
        if [[ ! -e "${path}" ]]; then
            echo "built SDK is missing required artifact: ${path}" >&2
            exit 2
        fi
    done
}

archive_and_maybe_upload() {
    validate_sdk
    if ((ARCHIVE == 0 && UPLOAD == 0 && SET_SECRET == 0)); then
        return
    fi
    local args=(--qt-root "${PREFIX}" --target "${TARGET}")
    if [[ -d "${PREFIX}/runtime/mingw" ]]; then
        args+=(--mingw-runtime-dir "${PREFIX}/runtime/mingw")
    fi
    if ((UPLOAD == 1)); then
        args+=(--upload)
    fi
    if ((SET_SECRET == 1)); then
        args+=(--set-secret)
    fi
    if [[ -n "${UPLOAD_PROXY}" ]]; then
        args+=(--upload-proxy "${UPLOAD_PROXY}")
    fi
    if [[ -n "${SECRET_NAME}" ]]; then
        args+=(--secret-name "${SECRET_NAME}")
    fi
    REPO="${REPO}" RELEASE_TAG="${RELEASE_TAG}" "${SCRIPT_DIR}/archive-qt-sdk.sh" "${args[@]}"
}

require_file "${QTBASE_SOURCE_ARCHIVE}"
require_file "${OPENSSL_SOURCE_ARCHIVE}"
if ((CLEAN == 1)); then
    rm -rf "${BUILD_DIR}" "${PREFIX}"
fi
mkdir -p "${BUILD_DIR}" "$(dirname -- "${PREFIX}")"

install_deps
prepare_cross_prefix
build_openssl
build_qtbase
copy_runtime_dlls
archive_and_maybe_upload

echo "Built Windows MinGW Qt SDK: ${PREFIX}"
