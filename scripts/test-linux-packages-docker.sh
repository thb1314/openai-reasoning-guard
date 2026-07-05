#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

ARCH=""
INPUT_DIR=""
JOBS="${JOBS:-1}"
TEST_DOCKER_IMAGE="${TEST_DOCKER_IMAGE:-debian:bookworm}"
TEST_DOCKER_ENTRYPOINT="${TEST_DOCKER_ENTRYPOINT:-}"
TEST_DOCKER_PLATFORM="${TEST_DOCKER_PLATFORM:-}"

usage() {
    cat <<EOF
Usage: $(basename "$0") --arch <x86_64|x86_32|arm64|arm32> --input-dir <dir>

Smoke-test final Linux packages inside Docker/QEMU. This script installs the
produced .deb and .rpm packages, runs the CLI, and starts the GUI/AppImage in
offscreen mode long enough to catch missing runtime dependencies.

Environment overrides:
  TEST_DOCKER_IMAGE=<image>         Override the runtime test image.
  TEST_DOCKER_ENTRYPOINT=<path>     Override the image entrypoint.
  TEST_DOCKER_PLATFORM=<platform>   Override docker run --platform.
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
if [[ -n "${TEST_DOCKER_PLATFORM}" ]]; then
    PLATFORM="${TEST_DOCKER_PLATFORM}"
fi
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

runtime_apt_install=$'set -euo pipefail\nexport DEBIAN_FRONTEND=noninteractive\n'

if [[ "${ARCH}" == "x86_32" && "${PLATFORM}" != "linux/386" ]]; then
runtime_apt_install+=$'dpkg --add-architecture i386\n'
fi

runtime_apt_install+=$'apt-get update\napt-get install -y --no-install-recommends \\\n  bash \\\n  ca-certificates \\\n  file \\\n  rpm \\\n  xauth \\\n  xvfb \\\n'

if [[ "${ARCH}" == "x86_32" && "${PLATFORM}" != "linux/386" ]]; then
runtime_apt_install+=$'  libc6:i386 \\\n  libstdc++6:i386 \\\n  libgcc-s1:i386 \\\n  zlib1g:i386 \\\n  libbsd0:i386 \\\n  libmd0:i386 \\\n  libuuid1:i386 \\\n  libpcre2-8-0:i386 \\\n  libfontconfig1:i386 \\\n  libfreetype6:i386 \\\n  libglib2.0-0:i386 \\\n  libice6:i386 \\\n  libsm6:i386 \\\n  libx11-6:i386 \\\n  libx11-xcb1:i386 \\\n  libxau6:i386 \\\n  libxi6:i386 \\\n  libxcb1:i386 \\\n  libxcb-glx0:i386 \\\n  libxcb-icccm4:i386 \\\n  libxcb-image0:i386 \\\n  libxcb-keysyms1:i386 \\\n  libxcb-randr0:i386 \\\n  libxcb-render0:i386 \\\n  libxcb-render-util0:i386 \\\n  libxcb-shape0:i386 \\\n  libxcb-shm0:i386 \\\n  libxcb-sync1:i386 \\\n  libxcb-util1:i386 \\\n  libxcb-xfixes0:i386 \\\n  libxcb-xinerama0:i386 \\\n  libxcb-xkb1:i386 \\\n  libxdmcp6:i386 \\\n  libxext6:i386 \\\n  libxkbcommon0:i386 \\\n  libxkbcommon-x11-0:i386 \\\n'
else
runtime_apt_install+=$'  libfontconfig1 \\\n  libfreetype6 \\\n  libglib2.0-0 \\\n  libice6 \\\n  libsm6 \\\n  libx11-6 \\\n  libx11-xcb1 \\\n  libxau6 \\\n  libxi6 \\\n  libxcb1 \\\n  libxcb-glx0 \\\n  libxcb-icccm4 \\\n  libxcb-image0 \\\n  libxcb-keysyms1 \\\n  libxcb-randr0 \\\n  libxcb-render0 \\\n  libxcb-render-util0 \\\n  libxcb-shape0 \\\n  libxcb-shm0 \\\n  libxcb-sync1 \\\n  libxcb-util1 \\\n  libxcb-xfixes0 \\\n  libxcb-xinerama0 \\\n  libxcb-xkb1 \\\n  libxdmcp6 \\\n  libxext6 \\\n  libxkbcommon0 \\\n  libxkbcommon-x11-0 \\\n'
fi

runtime_apt_install+=$'  patchelf \\\n  xz-utils\n'

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
