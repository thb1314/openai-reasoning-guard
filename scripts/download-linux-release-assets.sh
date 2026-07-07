#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

REPO="${REPO:-thb1314/openai-reasoning-guard}"
RELEASE_TAG="${RELEASE_TAG:-nightly}"
PACKAGE_ID="${PACKAGE_ID:-openai-reasoning-guard}"
VERSION="${VERSION:-$(sed -n 's/^project([^ ]* VERSION \([^ ]*\).*/\1/p' "${PROJECT_DIR}/CMakeLists.txt")}"
VERSION="${VERSION:-0.1.0}"
RPM_RELEASE="${RPM_RELEASE:-1}"
ARCH="all"
OUT_DIR="${OUT_DIR:-${PROJECT_DIR}/.package-work/release-smoke}"
CLEAN=0
RETRIES="${RETRIES:-3}"
DOWNLOAD_PROXY="${DOWNLOAD_PROXY:-}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--arch x86_64|x86_32|arm64|arm32|all] [--release-tag nightly] [--out-dir dir] [--clean]

Download Linux release assets and verify each file size against GitHub release
metadata before local Docker smoke tests.

Environment overrides:
  REPO=${REPO}
  RELEASE_TAG=${RELEASE_TAG}
  PACKAGE_ID=${PACKAGE_ID}
  VERSION=${VERSION}
  RPM_RELEASE=${RPM_RELEASE}
  OUT_DIR=${OUT_DIR}
  RETRIES=${RETRIES}
  DOWNLOAD_PROXY=http://127.0.0.1:7890
EOF
}

while (($# > 0)); do
    case "$1" in
        --arch)
            shift
            ARCH="${1:?missing arch}"
            ;;
        --repo)
            shift
            REPO="${1:?missing repo}"
            ;;
        --release-tag)
            shift
            RELEASE_TAG="${1:?missing release tag}"
            ;;
        --out-dir)
            shift
            OUT_DIR="${1:?missing output dir}"
            ;;
        --clean)
            CLEAN=1
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

require_tool() {
    local tool="$1"
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "required tool not found: ${tool}" >&2
        exit 2
    fi
}

deb_arch_for() {
    case "$1" in
        x86_64) echo amd64 ;;
        x86_32) echo i386 ;;
        arm64) echo arm64 ;;
        arm32) echo armhf ;;
        *)
            echo "unsupported arch: $1" >&2
            exit 2
            ;;
    esac
}

rpm_arch_for() {
    case "$1" in
        x86_64) echo x86_64 ;;
        x86_32) echo i686 ;;
        arm64) echo aarch64 ;;
        arm32) echo armv7hl ;;
        *)
            echo "unsupported arch: $1" >&2
            exit 2
            ;;
    esac
}

appimage_arch_for() {
    case "$1" in
        x86_64) echo x86_64 ;;
        x86_32) echo i686 ;;
        arm64) echo aarch64 ;;
        arm32) echo armhf ;;
        *)
            echo "unsupported arch: $1" >&2
            exit 2
            ;;
    esac
}

arch_list() {
    case "${ARCH}" in
        all) printf '%s\n' x86_64 x86_32 arm64 arm32 ;;
        x86_64|x86_32|arm64|arm32) printf '%s\n' "${ARCH}" ;;
        *)
            echo "unsupported arch: ${ARCH}" >&2
            exit 2
            ;;
    esac
}

asset_names_for_arch() {
    local arch="$1"
    local deb_arch
    local rpm_arch
    local appimage_arch
    deb_arch="$(deb_arch_for "${arch}")"
    rpm_arch="$(rpm_arch_for "${arch}")"
    appimage_arch="$(appimage_arch_for "${arch}")"
    printf '%s\n' \
        "${PACKAGE_ID}_${VERSION}_${deb_arch}.deb" \
        "${PACKAGE_ID}-${VERSION}-${RPM_RELEASE}.${rpm_arch}.rpm" \
        "${PACKAGE_ID}-gui-${VERSION}-${appimage_arch}.AppImage" \
        "${PACKAGE_ID}-cli-${VERSION}-${appimage_arch}.AppImage"
}

file_size() {
    local path="$1"
    if stat -c '%s' "${path}" >/dev/null 2>&1; then
        stat -c '%s' "${path}"
    else
        wc -c < "${path}" | tr -d ' '
    fi
}

release_json() {
    gh api "repos/${REPO}/releases/tags/${RELEASE_TAG}" --paginate
}

asset_record() {
    local name="$1"
    printf '%s' "${RELEASE_JSON}" | jq -r --arg name "${name}" '
        .assets[]
        | select(.name == $name)
        | [.name, .size, .browser_download_url]
        | @tsv
    '
}

download_asset() {
    local arch="$1"
    local name="$2"
    local record
    local expected_size
    local url
    local dest_dir
    local dest
    local tmp
    local attempt

    record="$(asset_record "${name}")"
    if [[ -z "${record}" ]]; then
        echo "release asset missing: ${name}" >&2
        exit 1
    fi

    IFS=$'\t' read -r _ expected_size url <<< "${record}"
    dest_dir="${OUT_DIR}/${arch}"
    dest="${dest_dir}/${name}"
    tmp="${dest}.tmp"
    mkdir -p "${dest_dir}"

    if [[ -f "${dest}" && "$(file_size "${dest}")" == "${expected_size}" ]]; then
        echo "Already verified: ${dest}"
        return
    fi
    rm -f "${dest}" "${tmp}"

    for ((attempt = 1; attempt <= RETRIES; attempt++)); do
        local curl_args=(-L --fail --connect-timeout 30 --retry 3 --retry-delay 2 -o "${tmp}")
        if [[ -n "${DOWNLOAD_PROXY}" ]]; then
            curl_args+=(--proxy "${DOWNLOAD_PROXY}")
        fi
        echo "Downloading ${name} (${attempt}/${RETRIES})"
        if curl "${curl_args[@]}" "${url}"; then
            local actual_size
            actual_size="$(file_size "${tmp}")"
            if [[ "${actual_size}" == "${expected_size}" ]]; then
                mv "${tmp}" "${dest}"
                chmod +x "${dest}" 2>/dev/null || true
                echo "Verified: ${dest}"
                return
            fi
            echo "size mismatch for ${name}: expected ${expected_size}, got ${actual_size}" >&2
        fi
        rm -f "${tmp}"
    done

    echo "failed to download verified asset: ${name}" >&2
    exit 1
}

require_tool curl
require_tool gh
require_tool jq

if [[ -n "${DOWNLOAD_PROXY}" ]]; then
    export HTTP_PROXY="${DOWNLOAD_PROXY}"
    export HTTPS_PROXY="${DOWNLOAD_PROXY}"
    export ALL_PROXY="${DOWNLOAD_PROXY}"
fi

if ((CLEAN == 1)); then
    case "${ARCH}" in
        all) rm -rf "${OUT_DIR}" ;;
        *) rm -rf "${OUT_DIR}/${ARCH}" ;;
    esac
fi
mkdir -p "${OUT_DIR}"

RELEASE_JSON="$(release_json)"

while IFS= read -r arch; do
    while IFS= read -r asset_name; do
        download_asset "${arch}" "${asset_name}"
    done < <(asset_names_for_arch "${arch}")
done < <(arch_list)
