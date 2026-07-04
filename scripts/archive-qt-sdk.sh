#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

QT_ROOT="${QT_ROOT:-}"
TARGET="${TARGET:-linux-x86_64}"
REPO="${REPO:-thb1314/openai-reasoning-guard}"
RELEASE_TAG="${RELEASE_TAG:-}"
SECRET_NAME="${SECRET_NAME:-}"
DIST_DIR="${DIST_DIR:-${PROJECT_DIR}/dist/qt-sdk}"
UPLOAD="${UPLOAD:-0}"
SET_SECRET="${SET_SECRET:-0}"
CLEAN="${CLEAN:-0}"
UPLOAD_PROXY="${UPLOAD_PROXY:-}"

usage() {
    cat <<EOF
Usage: $(basename "$0") --qt-root /path/to/qt5 --target linux-x86_64 [--upload] [--set-secret]

Create a Qt SDK archive for GitHub Actions. Run this on the machine or
container whose CPU/OS matches the target Qt SDK.

Targets and default secrets:
  linux-x86_64   QT_LINUX_X86_64_URL
  linux-x86_32   QT_LINUX_X86_32_URL
  linux-arm64    QT_LINUX_ARM64_URL
  linux-arm32    QT_LINUX_ARM32_URL
  macos-x86_64   QT_MACOS_X86_64_URL
  macos-aarch64  QT_MACOS_ARM64_URL

Environment overrides:
  QT_ROOT=/path/to/qt5
  TARGET=linux-x86_64
  REPO=${REPO}
  RELEASE_TAG=qt-sdk-linux-x86_64
  SECRET_NAME=QT_LINUX_X86_64_URL
  DIST_DIR=${DIST_DIR}
  UPLOAD=1
  SET_SECRET=1
  UPLOAD_PROXY=http://127.0.0.1:7890
EOF
}

while (($# > 0)); do
    case "$1" in
        --qt-root)
            shift
            QT_ROOT="${1:?missing qt root}"
            ;;
        --target)
            shift
            TARGET="${1:?missing target}"
            RELEASE_TAG="${RELEASE_TAG:-qt-sdk-${TARGET}}"
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
        --dist-dir)
            shift
            DIST_DIR="${1:?missing dist dir}"
            ;;
        --upload)
            UPLOAD=1
            ;;
        --set-secret)
            SET_SECRET=1
            ;;
        --clean)
            CLEAN=1
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

default_secret_for_target() {
    case "$1" in
        linux-x86_64) echo QT_LINUX_X86_64_URL ;;
        linux-x86_32) echo QT_LINUX_X86_32_URL ;;
        linux-arm64) echo QT_LINUX_ARM64_URL ;;
        linux-arm32) echo QT_LINUX_ARM32_URL ;;
        macos-x86_64) echo QT_MACOS_X86_64_URL ;;
        macos-aarch64) echo QT_MACOS_ARM64_URL ;;
        *)
            echo "unknown target: $1" >&2
            exit 2
            ;;
    esac
}

require_path() {
    local path="$1"
    if [[ ! -e "${path}" ]]; then
        echo "required Qt artifact missing: ${path}" >&2
        exit 2
    fi
}

if [[ -z "${QT_ROOT}" ]]; then
    echo "QT_ROOT or --qt-root is required" >&2
    exit 2
fi

QT_ROOT="$(cd -- "${QT_ROOT}" && pwd)"
SECRET_NAME="${SECRET_NAME:-$(default_secret_for_target "${TARGET}")}"
RELEASE_TAG="${RELEASE_TAG:-qt-sdk-${TARGET}}"

case "${TARGET}" in
    linux-*)
        require_path "${QT_ROOT}/bin/moc"
        require_path "${QT_ROOT}/lib/libQt5Core.so.5"
        require_path "${QT_ROOT}/plugins/platforms/libqxcb.so"
        ;;
    macos-*)
        require_path "${QT_ROOT}/bin/moc"
        require_path "${QT_ROOT}/bin/macdeployqt"
        ;;
esac

mkdir -p "${DIST_DIR}"
if ((CLEAN == 1)); then
    rm -f "${DIST_DIR}/qt5-${TARGET}.tar.xz"
fi

archive="${DIST_DIR}/qt5-${TARGET}.tar.xz"
qt_parent="$(cd -- "${QT_ROOT}/.." && pwd)"
qt_base="$(basename -- "${QT_ROOT}")"

tar -C "${qt_parent}" -cJf "${archive}" "${qt_base}"
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
    asset_url="https://github.com/${REPO}/releases/download/${RELEASE_TAG}/$(basename -- "${archive}")"
    printf '%s' "${asset_url}" | gh secret set "${SECRET_NAME}" -R "${REPO}"
    echo "${SECRET_NAME}=${asset_url}"
fi
