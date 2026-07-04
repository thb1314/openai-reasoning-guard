#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

TARGET="${TARGET:-macos-$(uname -m)}"
case "${TARGET}" in
    macos-arm64) TARGET="macos-aarch64" ;;
esac

QTBASE_SOURCE_ARCHIVE="${QTBASE_SOURCE_ARCHIVE:-}"
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
Usage: $(basename "$0") --qtbase-source-archive /path/to/qtbase.tar.xz --target macos-x86_64 [--archive] [--upload] [--set-secret]

Build a reusable Qt 5.9.6 macOS SDK from qtbase source. Run this on the
matching macOS host:
  macos-x86_64   on Intel macOS runner/host
  macos-aarch64  on Apple Silicon macOS runner/host

Examples:
  scripts/build-qt5-macos-sdk.sh --target macos-x86_64 --qtbase-source-archive ~/qtbase-opensource-src-5.9.6.tar.xz --archive
  scripts/build-qt5-macos-sdk.sh --target macos-aarch64 --qtbase-source-archive ~/qtbase-opensource-src-5.9.6.tar.xz --archive --upload --set-secret
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

if ((CLEAN == 1)); then
    rm -rf "${BUILD_DIR}" "${PREFIX}"
fi
mkdir -p "${BUILD_DIR}" "$(dirname -- "${PREFIX}")"

source_dir="${BUILD_DIR}/qtbase-src"
qt_build="${BUILD_DIR}/qtbase-build"
rm -rf "${source_dir}" "${qt_build}"
mkdir -p "${source_dir}" "${qt_build}"
tar -xf "${QTBASE_SOURCE_ARCHIVE}" -C "${source_dir}" --strip-components=1

(
    cd "${qt_build}"
    QMAKE_MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-10.13}" \
    "${source_dir}/configure" \
        -prefix "${PREFIX}" \
        -opensource \
        -confirm-license \
        -release \
        -shared \
        -framework \
        -nomake examples \
        -nomake tests \
        -make libs \
        -make tools \
        -no-dbus \
        -no-cups \
        -no-icu \
        -securetransport
    make -j"${JOBS}"
    make install
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
