#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

ARCH=""
SUITE="${SUITE:-bookworm}"
MIRROR="${MIRROR:-http://deb.debian.org/debian}"
IMAGE_TAG="${IMAGE_TAG:-}"
WORK_DIR="${WORK_DIR:-${PROJECT_DIR}/.docker-rootfs}"
ROOTFS_DIR="${ROOTFS_DIR:-}"
KEEP_ARTIFACTS="${KEEP_ARTIFACTS:-0}"
SUDO="${SUDO:-sudo}"

usage() {
    cat <<EOF
Usage: $(basename "$0") --arch <x86_64|x86_32|arm64|arm32>

Create a local Debian rootfs image for Linux package smoke tests when Docker Hub
pulls are unavailable. Cross-architecture second-stage bootstrap uses qemu-user-static
when needed. Requires host packages:
  debootstrap qemu-user-static binfmt-support

Environment overrides:
  SUITE=${SUITE}
  MIRROR=${MIRROR}
  IMAGE_TAG=<override final docker image tag>
  WORK_DIR=${WORK_DIR}
  ROOTFS_DIR=<override extracted rootfs directory>
EOF
}

while (($# > 0)); do
    case "$1" in
        --arch)
            shift
            ARCH="${1:?missing arch}"
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

if [[ -z "${ARCH}" ]]; then
    usage >&2
    exit 2
fi

case "${ARCH}" in
    x86_64)
        DEB_ARCH="amd64"
        QEMU_BIN=""
        DEFAULT_TAG="local-debian:${SUITE}-amd64"
        DOCKER_PLATFORM="linux/amd64"
        BOOTSTRAP_MODE="native"
        ;;
    x86_32)
        DEB_ARCH="i386"
        QEMU_BIN=""
        DEFAULT_TAG="local-debian:${SUITE}-i386"
        DOCKER_PLATFORM="linux/386"
        BOOTSTRAP_MODE="native"
        ;;
    arm64)
        DEB_ARCH="arm64"
        QEMU_BIN="qemu-aarch64-static"
        DEFAULT_TAG="local-debian:${SUITE}-arm64"
        DOCKER_PLATFORM="linux/arm64"
        BOOTSTRAP_MODE="foreign"
        ;;
    arm32)
        DEB_ARCH="armhf"
        QEMU_BIN="qemu-arm-static"
        DEFAULT_TAG="local-debian:${SUITE}-arm32"
        DOCKER_PLATFORM="linux/arm/v7"
        BOOTSTRAP_MODE="foreign"
        ;;
    *)
        echo "unsupported arch: ${ARCH}" >&2
        exit 2
        ;;
esac

IMAGE_TAG="${IMAGE_TAG:-${DEFAULT_TAG}}"
ROOTFS_DIR="${ROOTFS_DIR:-${WORK_DIR}/${SUITE}-${DEB_ARCH}}"
TAR_PATH="${WORK_DIR}/${SUITE}-${DEB_ARCH}.tar"

require_tool() {
    local tool="$1"
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "required tool not found: ${tool}" >&2
        exit 2
    fi
}

validate_rootfs_bootstrap() {
    local rootfs="$1"
    local missing=0

    if [[ ! -s "${rootfs}/etc/passwd" ]]; then
        echo "bootstrap validation failed: missing ${rootfs}/etc/passwd" >&2
        missing=1
    fi
    if [[ ! -s "${rootfs}/etc/group" ]]; then
        echo "bootstrap validation failed: missing ${rootfs}/etc/group" >&2
        missing=1
    fi
    if [[ ! -s "${rootfs}/var/lib/dpkg/status" ]]; then
        echo "bootstrap validation failed: missing ${rootfs}/var/lib/dpkg/status" >&2
        missing=1
    fi

    if ((missing == 1)); then
        exit 2
    fi
}

require_tool debootstrap
require_tool docker

if [[ -n "${QEMU_BIN}" ]]; then
    if [[ ! -x "/usr/bin/${QEMU_BIN}" ]]; then
        echo "required emulator missing: /usr/bin/${QEMU_BIN}" >&2
        echo "install qemu-user-static first" >&2
        exit 2
    fi

    if [[ ! -d /proc/sys/fs/binfmt_misc ]]; then
        echo "binfmt_misc is unavailable on this host" >&2
        exit 2
    fi
fi

mkdir -p "${WORK_DIR}"
${SUDO} rm -rf "${ROOTFS_DIR}" "${TAR_PATH}"

if [[ "${BOOTSTRAP_MODE}" == "foreign" ]]; then
    ${SUDO} debootstrap --foreign --arch "${DEB_ARCH}" "${SUITE}" "${ROOTFS_DIR}" "${MIRROR}"
    ${SUDO} install -m 0755 "/usr/bin/${QEMU_BIN}" "${ROOTFS_DIR}/usr/bin/${QEMU_BIN}"
    ${SUDO} chroot "${ROOTFS_DIR}" "/usr/bin/${QEMU_BIN}" /bin/sh /debootstrap/debootstrap --second-stage
else
    ${SUDO} debootstrap --arch "${DEB_ARCH}" "${SUITE}" "${ROOTFS_DIR}" "${MIRROR}"
fi

validate_rootfs_bootstrap "${ROOTFS_DIR}"

${SUDO} tee "${ROOTFS_DIR}/etc/apt/sources.list" >/dev/null <<EOF
deb ${MIRROR} ${SUITE} main
deb ${MIRROR} ${SUITE}-updates main
deb http://security.debian.org/debian-security ${SUITE}-security main
EOF

${SUDO} chroot "${ROOTFS_DIR}" apt-get update
${SUDO} chroot "${ROOTFS_DIR}" apt-get install -y --no-install-recommends \
    apt \
    bash \
    ca-certificates \
    coreutils \
    dpkg \
    libc-bin \
    procps
${SUDO} chroot "${ROOTFS_DIR}" apt-get clean

${SUDO} tar -C "${ROOTFS_DIR}" -cf "${TAR_PATH}" .
${SUDO} chown -R "$(id -u):$(id -g)" "${WORK_DIR}"
cat "${TAR_PATH}" | docker import --platform "${DOCKER_PLATFORM}" - "${IMAGE_TAG}" >/dev/null

if ((KEEP_ARTIFACTS == 0)); then
    ${SUDO} rm -rf "${ROOTFS_DIR}" "${TAR_PATH}"
fi

echo "Created Docker image: ${IMAGE_TAG}"
