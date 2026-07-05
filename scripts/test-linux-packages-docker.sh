#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

ARCH=""
INPUT_DIR=""
JOBS="${JOBS:-1}"
TEST_DOCKER_IMAGE="${TEST_DOCKER_IMAGE:-debian:bookworm}"
TEST_DOCKER_ENTRYPOINT="${TEST_DOCKER_ENTRYPOINT:-}"

usage() {
    cat <<EOF
Usage: $(basename "$0") --arch <x86_64|x86_32|arm64|arm32> --input-dir <dir>

Smoke-test final Linux packages inside Docker/QEMU. This script installs the
produced .deb and .rpm packages, runs the CLI, and starts the GUI/AppImage in
offscreen mode long enough to catch missing runtime dependencies.

Environment overrides:
  TEST_DOCKER_IMAGE=<image>         Override the runtime test image.
  TEST_DOCKER_ENTRYPOINT=<path>     Override the image entrypoint.
EOF
}

while (($# > 0)); do
    case "$1" in
        --arch)
            shift
            ARCH="${1:?missing arch}"
            ;;
        --input-dir)
            shift
            INPUT_DIR="${1:?missing input dir}"
            ;;
        --jobs)
            shift
            JOBS="${1:?missing jobs}"
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

if [[ -z "${ARCH}" || -z "${INPUT_DIR}" ]]; then
    usage >&2
    exit 2
fi

require_file() {
    local path="$1"
    if [[ ! -f "${path}" ]]; then
        echo "required file missing: ${path}" >&2
        exit 2
    fi
}

platform_for_arch() {
    case "$1" in
        x86_64) echo linux/amd64 ;;
        x86_32) echo linux/386 ;;
        arm64) echo linux/arm64 ;;
        arm32) echo linux/arm/v7 ;;
        *)
            echo "unsupported arch: $1" >&2
            exit 2
            ;;
    esac
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

INPUT_DIR="$(cd -- "${INPUT_DIR}" && pwd)"
PLATFORM="$(platform_for_arch "${ARCH}")"
DEB_ARCH="$(deb_arch_for "${ARCH}")"
RPM_ARCH="$(rpm_arch_for "${ARCH}")"
APPIMAGE_ARCH="$(appimage_arch_for "${ARCH}")"

DEB_PATH="$(find "${INPUT_DIR}" -maxdepth 1 -type f -name "*_${DEB_ARCH}.deb" -print -quit)"
RPM_PATH="$(find "${INPUT_DIR}" -maxdepth 1 -type f -name "*.${RPM_ARCH}.rpm" -print -quit)"
GUI_APPIMAGE="$(find "${INPUT_DIR}" -maxdepth 1 -type f -name "*-gui-*-${APPIMAGE_ARCH}.AppImage" -print -quit)"
CLI_APPIMAGE="$(find "${INPUT_DIR}" -maxdepth 1 -type f -name "*-cli-*-${APPIMAGE_ARCH}.AppImage" -print -quit)"

require_file "${DEB_PATH}"
require_file "${RPM_PATH}"
require_file "${GUI_APPIMAGE}"
require_file "${CLI_APPIMAGE}"

run_case() {
    local name="$1"
    shift
    echo "[test] ${name}"
    "$@"
}

run_in_container() {
    local name="$1"
    local script="$2"
    local -a docker_args=(
        run --rm
        --platform "${PLATFORM}"
        -v "${INPUT_DIR}:/packages:ro"
    )
    if [[ -n "${TEST_DOCKER_ENTRYPOINT}" ]]; then
        docker_args+=(--entrypoint "${TEST_DOCKER_ENTRYPOINT}")
        docker_args+=("${TEST_DOCKER_IMAGE}" -lc "${script}")
    else
        docker_args+=("${TEST_DOCKER_IMAGE}" bash -lc "${script}")
    fi
    docker "${docker_args[@]}"
    echo "[pass] ${name}"
}

runtime_apt_install='
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
  bash \
  ca-certificates \
  file \
  rpm \
  libfontconfig1 \
  libfreetype6 \
  libglib2.0-0 \
  libice6 \
  libsm6 \
  libx11-6 \
  libx11-xcb1 \
  libxau6 \
  libxi6 \
  libxcb1 \
  libxcb-glx0 \
  libxcb-icccm4 \
  libxcb-image0 \
  libxcb-keysyms1 \
  libxcb-randr0 \
  libxcb-render0 \
  libxcb-render-util0 \
  libxcb-shape0 \
  libxcb-shm0 \
  libxcb-sync1 \
  libxcb-util1 \
  libxcb-xfixes0 \
  libxcb-xinerama0 \
  libxcb-xkb1 \
  libxdmcp6 \
  libxext6 \
  libxkbcommon0 \
  libxkbcommon-x11-0 \
  patchelf \
  xauth \
  xvfb \
  xz-utils
'

deb_test_script="${runtime_apt_install}
dpkg -i /packages/$(basename "${DEB_PATH}")
command -v openai-reasoning-guard-cli
command -v openai-reasoning-guard-gui
openai-reasoning-guard-cli --help >/tmp/cli-help.txt
timeout 5 xvfb-run -a openai-reasoning-guard-gui >/tmp/gui.log 2>&1 || code=\$?
if [[ \${code:-0} -ne 0 && \${code:-0} -ne 124 ]]; then
  cat /tmp/gui.log >&2
  exit \${code}
fi
test -s /tmp/cli-help.txt
"

rpm_test_script="${runtime_apt_install}
rpm -i --nodeps /packages/$(basename "${RPM_PATH}")
test -x /usr/bin/openai-reasoning-guard-cli
test -x /usr/bin/openai-reasoning-guard-gui
/usr/bin/openai-reasoning-guard-cli --help >/tmp/cli-help.txt
timeout 5 xvfb-run -a /usr/bin/openai-reasoning-guard-gui >/tmp/gui.log 2>&1 || code=\$?
if [[ \${code:-0} -ne 0 && \${code:-0} -ne 124 ]]; then
  cat /tmp/gui.log >&2
  exit \${code}
fi
test -s /tmp/cli-help.txt
"

cli_appimage_test_script="${runtime_apt_install}
cp /packages/$(basename "${CLI_APPIMAGE}") /tmp/cli-appimage
chmod +x /tmp/cli-appimage
APPIMAGE_EXTRACT_AND_RUN=1 /tmp/cli-appimage --help >/tmp/cli-appimage-help.txt
test -s /tmp/cli-appimage-help.txt
"

gui_appimage_test_script="${runtime_apt_install}
cp /packages/$(basename "${GUI_APPIMAGE}") /tmp/gui-appimage
chmod +x /tmp/gui-appimage
timeout 5 env APPIMAGE_EXTRACT_AND_RUN=1 xvfb-run -a /tmp/gui-appimage >/tmp/gui-appimage.log 2>&1 || code=\$?
if [[ \${code:-0} -ne 0 && \${code:-0} -ne 124 ]]; then
  cat /tmp/gui-appimage.log >&2
  exit \${code}
fi
"

run_case "deb ${ARCH}" run_in_container "deb-${ARCH}" "${deb_test_script}"
run_case "rpm ${ARCH}" run_in_container "rpm-${ARCH}" "${rpm_test_script}"
run_case "cli AppImage ${ARCH}" run_in_container "cli-appimage-${ARCH}" "${cli_appimage_test_script}"
run_case "gui AppImage ${ARCH}" run_in_container "gui-appimage-${ARCH}" "${gui_appimage_test_script}"

echo "All Linux package smoke tests passed for ${ARCH} from ${INPUT_DIR}"
