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

usage() {
    cat <<EOF
Usage: $(basename "$0") [--repo owner/name] [--release-tag tag]

Check that a GitHub Release contains all package artifacts expected from the
multi-platform package workflow.

Environment overrides:
  REPO=${REPO}
  RELEASE_TAG=${RELEASE_TAG}
  VERSION=${VERSION}
  PACKAGE_ID=${PACKAGE_ID}
EOF
}

while (($# > 0)); do
    case "$1" in
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

if ! command -v gh >/dev/null 2>&1; then
    echo "gh CLI is required" >&2
    exit 2
fi

expected_assets=(
    "${PACKAGE_ID}_${VERSION}_amd64.deb"
    "${PACKAGE_ID}_${VERSION}_i386.deb"
    "${PACKAGE_ID}_${VERSION}_arm64.deb"
    "${PACKAGE_ID}_${VERSION}_armhf.deb"
    "${PACKAGE_ID}-${VERSION}-${RPM_RELEASE}.x86_64.rpm"
    "${PACKAGE_ID}-${VERSION}-${RPM_RELEASE}.i686.rpm"
    "${PACKAGE_ID}-${VERSION}-${RPM_RELEASE}.aarch64.rpm"
    "${PACKAGE_ID}-${VERSION}-${RPM_RELEASE}.armv7hl.rpm"
    "${PACKAGE_ID}-gui-${VERSION}-x86_64.AppImage"
    "${PACKAGE_ID}-gui-${VERSION}-i686.AppImage"
    "${PACKAGE_ID}-gui-${VERSION}-aarch64.AppImage"
    "${PACKAGE_ID}-gui-${VERSION}-armhf.AppImage"
    "${PACKAGE_ID}-cli-${VERSION}-x86_64.AppImage"
    "${PACKAGE_ID}-cli-${VERSION}-i686.AppImage"
    "${PACKAGE_ID}-cli-${VERSION}-aarch64.AppImage"
    "${PACKAGE_ID}-cli-${VERSION}-armhf.AppImage"
    "${PACKAGE_ID}-windows-x86_64-${VERSION}-installer.exe"
    "${PACKAGE_ID}-windows-x86_64-${VERSION}-portable.zip"
    "${PACKAGE_ID}-windows-x86_32-${VERSION}-installer.exe"
    "${PACKAGE_ID}-windows-x86_32-${VERSION}-portable.zip"
    "${PACKAGE_ID}-windows-arm64-${VERSION}-installer.exe"
    "${PACKAGE_ID}-windows-arm64-${VERSION}-portable.zip"
    "${PACKAGE_ID}-macos-x86_64-${VERSION}.dmg"
    "${PACKAGE_ID}-macos-aarch64-${VERSION}.dmg"
)

mapfile -t actual_assets < <(
    gh release view "${RELEASE_TAG}" -R "${REPO}" --json assets --jq '.assets[].name' | sort
)

missing=()
for asset in "${expected_assets[@]}"; do
    if ! printf '%s\n' "${actual_assets[@]}" | grep -Fxq "${asset}"; then
        missing+=("${asset}")
    fi
done

if ((${#missing[@]} > 0)); then
    echo "Missing ${#missing[@]} release asset(s) from ${REPO}@${RELEASE_TAG}:"
    printf '  %s\n' "${missing[@]}"
    exit 1
fi

echo "All ${#expected_assets[@]} expected release assets are present in ${REPO}@${RELEASE_TAG}."
