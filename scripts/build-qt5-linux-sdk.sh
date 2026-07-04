#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

TARGET="${TARGET:-linux-x86_64}"
QTBASE_SOURCE_ARCHIVE="${QTBASE_SOURCE_ARCHIVE:-/mnt/data/qt-2080ti-sync/archives/qtbase-everywhere-src-5.15.2.tar.xz}"
OPENSSL_SOURCE_ARCHIVE="${OPENSSL_SOURCE_ARCHIVE:-/mnt/data/qt-2080ti-sync/archives/openssl-1.1.1w.tar.gz}"
OUTPUT_ROOT="${OUTPUT_ROOT:-${PROJECT_DIR}/dist/qt-sdk-build}"
BUILD_DIR="${BUILD_DIR:-}"
PREFIX="${PREFIX:-}"
ARCHIVE="${ARCHIVE:-0}"
UPLOAD="${UPLOAD:-0}"
SET_SECRET="${SET_SECRET:-0}"
UPLOAD_PROXY="${UPLOAD_PROXY:-}"
REPO="${REPO:-thb1314/openai-reasoning-guard}"
RELEASE_TAG="${RELEASE_TAG:-}"
SECRET_NAME="${SECRET_NAME:-}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
DOCKER="${DOCKER:-0}"
DOCKER_IMAGE="${DOCKER_IMAGE:-}"
SKIP_DEPS="${SKIP_DEPS:-0}"
CLEAN="${CLEAN:-0}"

usage() {
    cat <<EOF
Usage: $(basename "$0") --target linux-x86_64 [--docker] [--archive] [--upload] [--set-secret]

Build a reusable Qt 5.15.x Linux SDK from qtbase source. For non-native Linux
targets, use --docker; it runs the same build inside a target-architecture
Linux container through Docker/QEMU.

Targets:
  linux-x86_64   Docker platform linux/amd64, secret QT_LINUX_X86_64_URL
  linux-x86_32   Docker platform linux/386, secret QT_LINUX_X86_32_URL
  linux-arm64    Docker platform linux/arm64, secret QT_LINUX_ARM64_URL
  linux-arm32    Docker platform linux/arm/v7, secret QT_LINUX_ARM32_URL

Examples:
  scripts/build-qt5-linux-sdk.sh --target linux-x86_64 --archive
  scripts/build-qt5-linux-sdk.sh --target linux-arm64 --docker --archive
  scripts/build-qt5-linux-sdk.sh --target linux-x86_64 --archive --upload --set-secret --upload-proxy http://127.0.0.1:7890

Environment overrides:
  QTBASE_SOURCE_ARCHIVE=${QTBASE_SOURCE_ARCHIVE}
  OPENSSL_SOURCE_ARCHIVE=${OPENSSL_SOURCE_ARCHIVE}
  OUTPUT_ROOT=${OUTPUT_ROOT}
  BUILD_DIR=${BUILD_DIR}
  PREFIX=${PREFIX}
  JOBS=${JOBS}
  DOCKER_IMAGE=${DOCKER_IMAGE:-target-specific Debian bookworm image}
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
        --jobs)
            shift
            JOBS="${1:?missing jobs}"
            ;;
        --docker)
            DOCKER=1
            ;;
        --docker-image)
            shift
            DOCKER_IMAGE="${1:?missing docker image}"
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

platform_for_target() {
    case "$1" in
        linux-x86_64) echo linux/amd64 ;;
        linux-x86_32) echo linux/386 ;;
        linux-arm64) echo linux/arm64 ;;
        linux-arm32) echo linux/arm/v7 ;;
        *)
            echo "unknown target: $1" >&2
            exit 2
            ;;
    esac
}

docker_image_for_target() {
    case "$1" in
        linux-x86_64) echo debian:bookworm ;;
        linux-x86_32) echo i386/debian:bookworm ;;
        linux-arm64) echo arm64v8/debian:bookworm ;;
        linux-arm32) echo arm32v7/debian:bookworm ;;
        *)
            echo "unknown target: $1" >&2
            exit 2
            ;;
    esac
}

secret_for_target() {
    case "$1" in
        linux-x86_64) echo QT_LINUX_X86_64_URL ;;
        linux-x86_32) echo QT_LINUX_X86_32_URL ;;
        linux-arm64) echo QT_LINUX_ARM64_URL ;;
        linux-arm32) echo QT_LINUX_ARM32_URL ;;
        *)
            echo "unknown target: $1" >&2
            exit 2
            ;;
    esac
}

require_file() {
    local path="$1"
    if [[ ! -f "${path}" ]]; then
        echo "required file missing: ${path}" >&2
        exit 2
    fi
}

install_linux_deps() {
    if ((SKIP_DEPS == 1)); then
        return
    fi
    if ! command -v apt-get >/dev/null 2>&1; then
        echo "apt-get not available; install Qt build dependencies manually or pass --skip-deps" >&2
        exit 2
    fi
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    apt-get install -y --no-install-recommends \
        bash \
        bison \
        build-essential \
        ca-certificates \
        flex \
        gperf \
        libfontconfig1-dev \
        libfreetype6-dev \
        libglib2.0-dev \
        libx11-dev \
        libx11-xcb-dev \
        libxau-dev \
        libxcb1-dev \
        libxcb-render0-dev \
        libxcb-render-util0-dev \
        libxcb-shape0-dev \
        libxcb-shm0-dev \
        libxcb-sync-dev \
        libxcb-xfixes0-dev \
        libxcb-xinerama0-dev \
        libxcb-xkb-dev \
        libxext-dev \
        libxrender-dev \
        libxkbcommon-dev \
        libxkbcommon-x11-dev \
        make \
        patch \
        perl \
        pkg-config \
        python3 \
        ruby \
        tar \
        xz-utils \
        zlib1g-dev
}

run_in_docker() {
    require_file "${QTBASE_SOURCE_ARCHIVE}"
    require_file "${OPENSSL_SOURCE_ARCHIVE}"
    local platform
    platform="$(platform_for_target "${TARGET}")"
    local image="${DOCKER_IMAGE:-$(docker_image_for_target "${TARGET}")}"

    local staged_sources="${PROJECT_DIR}/.package-work/qt-sdk-source-${TARGET}"
    rm -rf "${staged_sources}"
    mkdir -p "${staged_sources}"
    cp -a "${QTBASE_SOURCE_ARCHIVE}" "${staged_sources}/qtbase.tar.xz"
    cp -a "${OPENSSL_SOURCE_ARCHIVE}" "${staged_sources}/openssl.tar.gz"
    mkdir -p "${OUTPUT_ROOT}"

    docker run --rm \
        --platform "${platform}" \
        -v "${PROJECT_DIR}:/workspace" \
        -v "${staged_sources}:/qt-sources:ro" \
        -v "${OUTPUT_ROOT}:/qt-output" \
        -w /workspace \
        "${image}" \
        bash -lc "set -euo pipefail; /workspace/scripts/build-qt5-linux-sdk.sh --target '${TARGET}' --qtbase-source-archive /qt-sources/qtbase.tar.xz --openssl-source-archive /qt-sources/openssl.tar.gz --output-root /qt-output --build-dir /qt-output/build-${TARGET} --prefix /qt-output/qt5-${TARGET} --jobs '${JOBS}' --archive"

    if ((UPLOAD == 1 || SET_SECRET == 1)); then
        ARCHIVE=1
        PREFIX="${OUTPUT_ROOT}/qt5-${TARGET}"
        BUILD_DIR="${OUTPUT_ROOT}/build-${TARGET}"
        archive_and_maybe_upload
    fi
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
    local qfloat16_header="${source_dir}/src/corelib/global/qfloat16.h"
    if [[ -f "${qfloat16_header}" ]] && ! grep -q '#include <limits>' "${qfloat16_header}"; then
        sed -i '/#include <QtCore\/qglobal.h>/a #include <limits>' "${qfloat16_header}"
    fi
}

build_openssl() {
    local source_dir="${BUILD_DIR}/openssl-src"
    extract_one "${OPENSSL_SOURCE_ARCHIVE}" "${source_dir}"
    (
        cd "${source_dir}"
        if [[ "${TARGET}" == "linux-arm32" ]]; then
            ./Configure linux-generic32 no-asm \
                --prefix="${PREFIX}" \
                --openssldir="${PREFIX}/ssl" \
                shared \
                no-ssl3 \
                no-comp
        else
            ./config --prefix="${PREFIX}" --openssldir="${PREFIX}/ssl" shared no-ssl3 no-comp
        fi
        make -j"${JOBS}"
        make install_sw
    )
}

build_qtbase() {
    local source_dir="${BUILD_DIR}/qtbase-src"
    local qt_build="${BUILD_DIR}/qtbase-build"
    extract_one "${QTBASE_SOURCE_ARCHIVE}" "${source_dir}"
    patch_qtbase_for_modern_cpp "${source_dir}"
    rm -rf "${qt_build}"
    mkdir -p "${qt_build}"
    (
        cd "${qt_build}"
        "${source_dir}/configure" \
            -prefix "${PREFIX}" \
            -opensource \
            -confirm-license \
            -release \
            -shared \
            -no-pch \
            -nomake examples \
            -nomake tests \
            -make libs \
            -make tools \
            -no-dbus \
            -no-gtk \
            -no-cups \
            -no-eglfs \
            -no-linuxfb \
            -no-opengl \
            -no-icu \
            -qt-zlib \
            -qt-pcre \
            -qt-libpng \
            -qt-libjpeg \
            -qt-freetype \
            -qt-harfbuzz \
            -qt-xcb \
            -openssl-runtime \
            -I "${PREFIX}/include" \
            -L "${PREFIX}/lib"
        make -j"${JOBS}"
        make install
    )
}

validate_sdk() {
    local required=(
        "${PREFIX}/bin/moc"
        "${PREFIX}/lib/libQt5Core.so.5"
        "${PREFIX}/lib/libQt5Network.so.5"
        "${PREFIX}/lib/libQt5Gui.so.5"
        "${PREFIX}/lib/libQt5Widgets.so.5"
        "${PREFIX}/lib/libQt5Test.so.5"
        "${PREFIX}/plugins/platforms/libqxcb.so"
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

    local archive_dir="${OUTPUT_ROOT}/archives"
    local archive="${archive_dir}/qt5-${TARGET}.tar.xz"
    mkdir -p "${archive_dir}"
    tar -C "$(dirname -- "${PREFIX}")" -cJf "${archive}" "$(basename -- "${PREFIX}")"
    echo "Built Qt SDK archive: ${archive}"
    ls -lh "${archive}"

    if ((UPLOAD == 1 || SET_SECRET == 1)); then
        if ! command -v gh >/dev/null 2>&1; then
            echo "gh CLI is required for --upload or --set-secret" >&2
            exit 2
        fi
        if [[ -n "${UPLOAD_PROXY}" ]]; then
            export HTTP_PROXY="${UPLOAD_PROXY}"
            export HTTPS_PROXY="${UPLOAD_PROXY}"
            export ALL_PROXY="${UPLOAD_PROXY}"
        fi
    fi

    if ((UPLOAD == 1)); then
        if gh release view "${RELEASE_TAG}" -R "${REPO}" >/dev/null 2>&1; then
            gh release upload "${RELEASE_TAG}" "${archive}" -R "${REPO}" --clobber
        else
            gh release create "${RELEASE_TAG}" "${archive}" \
                -R "${REPO}" \
                --title "Qt SDK ${TARGET}" \
                --notes "Qt SDK archive for ${TARGET} GitHub Actions builds." \
                --prerelease
        fi
    fi

    if ((SET_SECRET == 1)); then
        SECRET_NAME="${SECRET_NAME:-$(secret_for_target "${TARGET}")}"
        local asset_url="https://github.com/${REPO}/releases/download/${RELEASE_TAG}/$(basename -- "${archive}")"
        printf '%s' "${asset_url}" | gh secret set "${SECRET_NAME}" -R "${REPO}"
        echo "${SECRET_NAME}=${asset_url}"
    fi
}

if ((DOCKER == 1)); then
    BUILD_DIR="${BUILD_DIR:-${OUTPUT_ROOT}/build-${TARGET}}"
    PREFIX="${PREFIX:-${OUTPUT_ROOT}/qt5-${TARGET}}"
    RELEASE_TAG="${RELEASE_TAG:-qt-sdk-${TARGET}}"
    run_in_docker
    exit 0
fi

require_file "${QTBASE_SOURCE_ARCHIVE}"
require_file "${OPENSSL_SOURCE_ARCHIVE}"
BUILD_DIR="${BUILD_DIR:-${OUTPUT_ROOT}/build-${TARGET}}"
PREFIX="${PREFIX:-${OUTPUT_ROOT}/qt5-${TARGET}}"
RELEASE_TAG="${RELEASE_TAG:-qt-sdk-${TARGET}}"

if ((CLEAN == 1)); then
    rm -rf "${BUILD_DIR}" "${PREFIX}"
fi
mkdir -p "${BUILD_DIR}" "$(dirname -- "${PREFIX}")"

install_linux_deps
build_openssl
build_qtbase
archive_and_maybe_upload

echo "Built Qt SDK: ${PREFIX}"
