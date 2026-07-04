#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

TARGET="${TARGET:-macos-$(uname -m)}"
case "${TARGET}" in
    macos-arm64) TARGET="macos-aarch64" ;;
esac

QTBASE_SOURCE_ARCHIVE="${QTBASE_SOURCE_ARCHIVE:-}"
QTTOOLS_SOURCE_ARCHIVE="${QTTOOLS_SOURCE_ARCHIVE:-}"
OUTPUT_ROOT="${OUTPUT_ROOT:-${PROJECT_DIR}/dist/qt-sdk-build}"
BUILD_DIR="${BUILD_DIR:-}"
PREFIX="${PREFIX:-}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 2)}"
CLEAN="${CLEAN:-0}"
ARCHIVE="${ARCHIVE:-0}"
UPLOAD="${UPLOAD:-0}"
SET_SECRET="${SET_SECRET:-0}"
UPLOAD_PROXY="${UPLOAD_PROXY:-}"
REPO="${REPO:-thb1314/openai-reasoning-guard}"
RELEASE_TAG="${RELEASE_TAG:-}"

usage() {
    cat <<EOF
Usage: $(basename "$0") --qtbase-source-archive /path/to/qtbase.tar.xz --qttools-source-archive /path/to/qttools.tar.xz --target macos-x86_64 [--archive] [--upload] [--set-secret]

Build a reusable Qt 5.15.x macOS SDK from qtbase and qttools source. Run this on the matching
macOS host:
  macos-x86_64   on Intel macOS runner/host
  macos-aarch64  on Apple Silicon macOS runner/host

Use a Qt 5 source tree that supports the requested CPU architecture.

Examples:
  scripts/build-qt5-macos-sdk.sh --target macos-x86_64 --qtbase-source-archive ~/qtbase-everywhere-src-5.15.2.tar.xz --qttools-source-archive ~/qttools-everywhere-src-5.15.2.tar.xz --archive
  scripts/build-qt5-macos-sdk.sh --target macos-aarch64 --qtbase-source-archive ~/qtbase-everywhere-src-5.15.x.tar.xz --qttools-source-archive ~/qttools-everywhere-src-5.15.x.tar.xz --archive --upload --set-secret

Environment overrides:
  QTBASE_SOURCE_ARCHIVE=${QTBASE_SOURCE_ARCHIVE}
  QTTOOLS_SOURCE_ARCHIVE=${QTTOOLS_SOURCE_ARCHIVE}
  REPO=${REPO}
  RELEASE_TAG=qt-sdk-macos-x86_64
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
        --qttools-source-archive)
            shift
            QTTOOLS_SOURCE_ARCHIVE="${1:?missing qttools source archive}"
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
        --jobs)
            shift
            JOBS="${1:?missing jobs}"
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
    macos-arm64) TARGET="macos-aarch64" ;;
    macos-x86_64|macos-aarch64) ;;
    *)
        echo "unsupported target: ${TARGET}" >&2
        exit 2
        ;;
esac
BUILD_DIR="${BUILD_DIR:-${OUTPUT_ROOT}/build-${TARGET}}"
PREFIX="${PREFIX:-${OUTPUT_ROOT}/qt5-${TARGET}}"
RELEASE_TAG="${RELEASE_TAG:-qt-sdk-${TARGET}}"

if [[ -z "${QTBASE_SOURCE_ARCHIVE}" || ! -f "${QTBASE_SOURCE_ARCHIVE}" ]]; then
    echo "--qtbase-source-archive is required" >&2
    exit 2
fi
if [[ -z "${QTTOOLS_SOURCE_ARCHIVE}" || ! -f "${QTTOOLS_SOURCE_ARCHIVE}" ]]; then
    echo "--qttools-source-archive is required because macOS packaging needs macdeployqt" >&2
    exit 2
fi

if ((CLEAN == 1)); then
    rm -rf "${BUILD_DIR}" "${PREFIX}"
fi
mkdir -p "${BUILD_DIR}" "$(dirname -- "${PREFIX}")"

mac_arch_for_target() {
    case "$1" in
        macos-x86_64) echo x86_64 ;;
        macos-aarch64) echo arm64 ;;
        *)
            echo "unknown macOS target: $1" >&2
            exit 2
            ;;
    esac
}

extract_one() {
    local archive="$1"
    local dest="$2"
    rm -rf "${dest}"
    mkdir -p "${dest}"
    tar -xf "${archive}" -C "${dest}" --strip-components=1
}

patch_qtbase_for_modern_cpp() {
    local source_dir="$1"
    local qglobal_header="${source_dir}/src/corelib/global/qglobal.h"
    if [[ -f "${qglobal_header}" ]] && ! grep -q '#include <limits>' "${qglobal_header}"; then
        sed -i '' '/#include <algorithm>/a\
#include <limits>
' "${qglobal_header}"
    fi

    local qfloat16_header="${source_dir}/src/corelib/global/qfloat16.h"
    if [[ -f "${qfloat16_header}" ]] && ! grep -q '#include <limits>' "${qfloat16_header}"; then
        sed -i '' '/#include <QtCore\/qglobal.h>/a\
#include <limits>
' "${qfloat16_header}"
    fi

    local qbytearraymatcher_header="${source_dir}/src/corelib/text/qbytearraymatcher.h"
    if [[ -f "${qbytearraymatcher_header}" ]] && ! grep -q '#include <limits>' "${qbytearraymatcher_header}"; then
        sed -i '' '/#include <QtCore\/qbytearray.h>/a\
#include <limits>
' "${qbytearraymatcher_header}"
    fi

    local qiosurface_header="${source_dir}/src/plugins/platforms/cocoa/qiosurfacegraphicsbuffer.h"
    if [[ -f "${qiosurface_header}" ]] && ! grep -q 'CoreGraphics/CGColorSpace.h' "${qiosurface_header}"; then
        perl -0pi -e 's/(#include <QtCore\/qglobal.h>\n)/$1#include <CoreGraphics\/CGColorSpace.h>\n/' "${qiosurface_header}"
        if ! grep -q 'CoreGraphics/CGColorSpace.h' "${qiosurface_header}"; then
            perl -0pi -e 's/(#pragma once\n)/$1#include <CoreGraphics\/CGColorSpace.h>\n/' "${qiosurface_header}"
        fi
        if ! grep -q 'CoreGraphics/CGColorSpace.h' "${qiosurface_header}"; then
            perl -0pi -e 's/\A/#include <CoreGraphics\/CGColorSpace.h>\n/' "${qiosurface_header}"
        fi
    fi

    local configure_script="${source_dir}/configure"
    if [[ -f "${configure_script}" ]] && ! grep -q 'relpathMangled/qtbase.pro' "${configure_script}"; then
        perl -0pi -e 's/"\$relpathMangled" --/"\$relpathMangled\/qtbase.pro" --/g' "${configure_script}"
    fi
}

source_dir="${BUILD_DIR}/qtbase-src"
qttools_source_dir="${BUILD_DIR}/qttools-src"
qt_build="${BUILD_DIR}/qtbase-build"
qttools_build="${BUILD_DIR}/qttools-macdeployqt-build"
extract_one "${QTBASE_SOURCE_ARCHIVE}" "${source_dir}"
extract_one "${QTTOOLS_SOURCE_ARCHIVE}" "${qttools_source_dir}"
patch_qtbase_for_modern_cpp "${source_dir}"
rm -rf "${qt_build}" "${qttools_build}"
mkdir -p "${qt_build}" "${qttools_build}"

deployment_target="${MACOSX_DEPLOYMENT_TARGET:-10.13}"
if [[ "${TARGET}" == "macos-aarch64" && -z "${MACOSX_DEPLOYMENT_TARGET:-}" ]]; then
    deployment_target="11.0"
fi
mac_arch="$(mac_arch_for_target "${TARGET}")"

(
    cd "${qt_build}"
    export MACOSX_DEPLOYMENT_TARGET="${deployment_target}"
    export QMAKE_MACOSX_DEPLOYMENT_TARGET="${deployment_target}"
    export QMAKE_APPLE_DEVICE_ARCHS="${mac_arch}"
    "${source_dir}/configure" \
        -prefix "${PREFIX}" \
        -opensource \
        -confirm-license \
        -release \
        -shared \
        -framework \
        -no-pch \
        -nomake examples \
        -nomake tests \
        -make libs \
        -make tools \
        -no-opengl \
        -no-dbus \
        -no-cups \
        -no-icu \
        -securetransport \
        QMAKE_MACOSX_DEPLOYMENT_TARGET="${deployment_target}" \
        QMAKE_APPLE_DEVICE_ARCHS="${mac_arch}"
    make -j"${JOBS}"
    make install
)

(
    cd "${qttools_build}"
    export MACOSX_DEPLOYMENT_TARGET="${deployment_target}"
    export QMAKE_MACOSX_DEPLOYMENT_TARGET="${deployment_target}"
    export QMAKE_APPLE_DEVICE_ARCHS="${mac_arch}"
    "${PREFIX}/bin/qmake" "${qttools_source_dir}/src/macdeployqt/macdeployqt/macdeployqt.pro" \
        QMAKE_MACOSX_DEPLOYMENT_TARGET="${deployment_target}" \
        QMAKE_APPLE_DEVICE_ARCHS="${mac_arch}" \
        DESTDIR="${qttools_build}/bin"
    make -j"${JOBS}"
    make install || true
    if [[ ! -x "${PREFIX}/bin/macdeployqt" ]]; then
        macdeployqt_bin="$(find "${qttools_build}" -type f -name macdeployqt -perm -111 -print -quit)"
        if [[ -n "${macdeployqt_bin}" ]]; then
            cp -f "${macdeployqt_bin}" "${PREFIX}/bin/macdeployqt"
            chmod +x "${PREFIX}/bin/macdeployqt"
        fi
    fi
)

for path in \
    "${PREFIX}/bin/moc" \
    "${PREFIX}/bin/macdeployqt" \
    "${PREFIX}/lib/cmake/Qt5/Qt5Config.cmake"; do
    if [[ ! -e "${path}" ]]; then
        echo "built SDK is missing required artifact: ${path}" >&2
        exit 2
    fi
done

if ((ARCHIVE == 1 || UPLOAD == 1 || SET_SECRET == 1)); then
    args=(--qt-root "${PREFIX}" --target "${TARGET}")
    if ((UPLOAD == 1)); then
        args+=(--upload)
    fi
    if ((SET_SECRET == 1)); then
        args+=(--set-secret)
    fi
    if [[ -n "${UPLOAD_PROXY}" ]]; then
        args+=(--upload-proxy "${UPLOAD_PROXY}")
    fi
    REPO="${REPO}" RELEASE_TAG="${RELEASE_TAG}" "${SCRIPT_DIR}/archive-qt-sdk.sh" "${args[@]}"
fi

echo "Built Qt SDK: ${PREFIX}"
