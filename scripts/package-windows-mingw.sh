#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

PACKAGE_ID="${PACKAGE_ID:-openai-reasoning-guard}"
APP_NAME="${APP_NAME:-OpenAI Reasoning Guard}"
VERSION="${VERSION:-$(sed -n 's/^project([^ ]* VERSION \([^ ]*\).*/\1/p' "${PROJECT_DIR}/CMakeLists.txt")}"
VERSION="${VERSION:-0.1.0}"
PACKAGE_ARCH="${PACKAGE_ARCH:-x86_64}"
QT_ROOT="${QT_ROOT:-}"
MINGW_TRIPLE="${MINGW_TRIPLE:-}"
MINGW_BIN_DIR="${MINGW_BIN_DIR:-}"
MINGW_SYSROOT="${MINGW_SYSROOT:-}"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build-package-windows-mingw-${PACKAGE_ARCH}}"
DIST_DIR="${DIST_DIR:-${PROJECT_DIR}/dist}"
WORK_DIR="${WORK_DIR:-${PROJECT_DIR}/.package-work/windows-mingw-${PACKAGE_ARCH}}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
BUILD_TESTS="${BUILD_TESTS:-OFF}"
SKIP_BUILD="${SKIP_BUILD:-0}"
CLEAN="${CLEAN:-0}"
BUILD_INSTALLER="${BUILD_INSTALLER:-1}"
MAKENSIS="${MAKENSIS:-}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--arch x86_64|x86_32|arm64] [--qt-root /path/to/qt] [--clean] [--skip-build]

Cross-build Windows portable zip and installer exe packages with MinGW from Linux.

The Qt SDK must be a MinGW target SDK with Linux host Qt tools:
  bin/moc, bin/rcc, bin/uic
  bin/Qt5Core.dll, bin/Qt5Network.dll, bin/Qt5Gui.dll, bin/Qt5Widgets.dll
  plugins/platforms/qwindows.dll
  lib/cmake/Qt5/Qt5Config.cmake

Environment overrides:
  PACKAGE_ID=${PACKAGE_ID}
  VERSION=${VERSION}
  PACKAGE_ARCH=${PACKAGE_ARCH}
  QT_ROOT=/path/to/qt5-mingw
  MINGW_TRIPLE=x86_64-w64-mingw32
  MINGW_BIN_DIR=/path/to/mingw/bin
  MINGW_SYSROOT=/path/to/mingw/sysroot
  BUILD_DIR=${BUILD_DIR}
  DIST_DIR=${DIST_DIR}
  WORK_DIR=${WORK_DIR}
  BUILD_TESTS=OFF
  BUILD_INSTALLER=1
  MAKENSIS=/path/to/makensis
EOF
}

while (($# > 0)); do
    case "$1" in
        --arch)
            shift
            PACKAGE_ARCH="${1:?missing arch}"
            ;;
        --qt-root)
            shift
            QT_ROOT="${1:?missing Qt root}"
            ;;
        --build-dir)
            shift
            BUILD_DIR="${1:?missing build dir}"
            ;;
        --dist-dir)
            shift
            DIST_DIR="${1:?missing dist dir}"
            ;;
        --work-dir)
            shift
            WORK_DIR="${1:?missing work dir}"
            ;;
        --clean)
            CLEAN=1
            ;;
        --skip-build)
            SKIP_BUILD=1
            ;;
        --no-installer)
            BUILD_INSTALLER=0
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

case "${PACKAGE_ARCH}" in
    x86_64)
        MINGW_TRIPLE="${MINGW_TRIPLE:-x86_64-w64-mingw32}"
        MINGW_RUNTIME_DLLS=(libgcc_s_seh-1.dll libstdc++-6.dll libwinpthread-1.dll)
        ;;
    x86_32)
        MINGW_TRIPLE="${MINGW_TRIPLE:-i686-w64-mingw32}"
        MINGW_RUNTIME_DLLS=(libgcc_s_sjlj-1.dll libgcc_s_dw2-1.dll libstdc++-6.dll libwinpthread-1.dll)
        ;;
    arm64)
        MINGW_TRIPLE="${MINGW_TRIPLE:-aarch64-w64-mingw32}"
        MINGW_RUNTIME_DLLS=(libc++.dll libc++abi.dll libunwind.dll libwinpthread-1.dll libgcc_s_seh-1.dll libstdc++-6.dll)
        ;;
    *)
        echo "unsupported Windows package arch: ${PACKAGE_ARCH}" >&2
        exit 2
        ;;
esac

require_file() {
    local path="$1"
    if [[ ! -f "${path}" ]]; then
        echo "required file missing: ${path}" >&2
        exit 2
    fi
}

require_dir() {
    local path="$1"
    if [[ ! -d "${path}" ]]; then
        echo "required directory missing: ${path}" >&2
        exit 2
    fi
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

find_required_tool() {
    local name="$1"
    if ! find_tool "${name}"; then
        echo "Unable to find MinGW tool: ${MINGW_TRIPLE}-${name}" >&2
        exit 2
    fi
}

find_makensis() {
    if [[ -n "${MAKENSIS}" && -x "${MAKENSIS}" ]]; then
        printf '%s\n' "${MAKENSIS}"
        return 0
    fi
    if command -v makensis >/dev/null 2>&1; then
        command -v makensis
        return 0
    fi
    return 1
}

prepare_cross_path() {
    local cross_bin="${WORK_DIR}/cross-bin"
    mkdir -p "${cross_bin}"

    local tool source
    for tool in gcc g++ cc c++ as ar ranlib windres strip nm objcopy objdump dlltool ld; do
        case "${tool}" in
            gcc|cc) source="$(find_tool_any gcc-posix gcc || true)" ;;
            g++|c++) source="$(find_tool_any g++-posix g++ || true)" ;;
            *) source="$(find_tool "${tool}" || true)" ;;
        esac
        if [[ -n "${source}" ]]; then
            ln -sf "${source}" "${cross_bin}/${tool}"
            ln -sf "${source}" "${cross_bin}/${MINGW_TRIPLE}-${tool}"
        fi
    done

    export PATH="${cross_bin}:${PATH}"
}

copy_glob_optional() {
    local destination="$1"
    shift
    local pattern
    shopt -s nullglob
    for pattern in "$@"; do
        if [[ "${pattern}" == *[\*\?\[]* ]]; then
            local files=(${pattern})
            if ((${#files[@]} > 0)); then
                cp -f "${files[@]}" "${destination}/"
            fi
        elif [[ -f "${pattern}" ]]; then
            cp -f "${pattern}" "${destination}/"
        fi
    done
    shopt -u nullglob
}

copy_first_runtime_dll() {
    local dll="$1"
    local destination="$2"
    local candidates=()
    candidates+=("${QT_ROOT}/runtime/mingw/${dll}")
    candidates+=("${QT_ROOT}/bin/${dll}")

    local compiler
    compiler="$(find_tool g++-posix || find_tool g++ || true)"
    if [[ -n "${compiler}" ]]; then
        local libgcc_dir
        libgcc_dir="$("${compiler}" -print-libgcc-file-name 2>/dev/null || true)"
        if [[ -n "${libgcc_dir}" ]]; then
            candidates+=("$(dirname -- "${libgcc_dir}")/${dll}")
        fi
    fi

    if [[ -n "${MINGW_SYSROOT}" ]]; then
        candidates+=("${MINGW_SYSROOT}/bin/${dll}")
        candidates+=("${MINGW_SYSROOT}/lib/${dll}")
    fi
    candidates+=("/usr/${MINGW_TRIPLE}/bin/${dll}")
    candidates+=("/usr/${MINGW_TRIPLE}/lib/${dll}")

    local candidate
    for candidate in "${candidates[@]}"; do
        if [[ -f "${candidate}" ]]; then
            cp -f "${candidate}" "${destination}/"
            return 0
        fi
    done
    return 1
}

if [[ -z "${QT_ROOT}" ]]; then
    echo "QT_ROOT or --qt-root is required" >&2
    exit 2
fi
QT_ROOT="$(cd -- "${QT_ROOT}" && pwd)"
QT_BIN="${QT_ROOT}/bin"
QT_PLUGINS="${QT_ROOT}/plugins"

require_file "${QT_BIN}/moc"
require_file "${QT_BIN}/rcc"
require_file "${QT_BIN}/uic"
require_file "${QT_BIN}/Qt5Core.dll"
require_file "${QT_BIN}/Qt5Network.dll"
require_file "${QT_BIN}/Qt5Gui.dll"
require_file "${QT_BIN}/Qt5Widgets.dll"
require_file "${QT_PLUGINS}/platforms/qwindows.dll"
require_file "${QT_ROOT}/lib/cmake/Qt5/Qt5Config.cmake"

if [[ -z "${MINGW_BIN_DIR}" ]]; then
    cc_path="$(find_tool gcc-posix || find_tool gcc || true)"
    if [[ -n "${cc_path}" ]]; then
        MINGW_BIN_DIR="$(dirname -- "${cc_path}")"
    fi
fi
if [[ -n "${MINGW_BIN_DIR}" ]]; then
    MINGW_BIN_DIR="$(cd -- "${MINGW_BIN_DIR}" && pwd)"
    export PATH="${MINGW_BIN_DIR}:${PATH}"
fi

CC="$(find_tool_any gcc-posix gcc || true)"
CXX="$(find_tool_any g++-posix g++ || true)"
if [[ -z "${CC}" || -z "${CXX}" ]]; then
    echo "MinGW compiler not found for ${MINGW_TRIPLE}" >&2
    exit 2
fi
export CC CXX MINGW_TRIPLE MINGW_BIN_DIR MINGW_SYSROOT QT_ROOT

find_required_tool windres >/dev/null
find_required_tool ar >/dev/null
find_required_tool ranlib >/dev/null
find_required_tool strip >/dev/null

if ((CLEAN == 1)); then
    rm -rf "${BUILD_DIR}" "${WORK_DIR}"
fi
mkdir -p "${BUILD_DIR}" "${DIST_DIR}" "${WORK_DIR}"
prepare_cross_path

if ((SKIP_BUILD == 0)); then
    cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
        -G "Unix Makefiles" \
        -DCMAKE_TOOLCHAIN_FILE="${PROJECT_DIR}/scripts/toolchain-mingw.cmake" \
        -DCMAKE_BUILD_TYPE=Release \
        -DNET_TUNNEL_QT_SDK_ROOT="${QT_ROOT}" \
        -DNET_TUNNEL_BUILD_TESTS="${BUILD_TESTS}"
    cmake --build "${BUILD_DIR}" -- -j"${JOBS}"
fi

stage_dir="${WORK_DIR}/${PACKAGE_ID}-windows-${PACKAGE_ARCH}"
zip_path="${DIST_DIR}/${PACKAGE_ID}-windows-${PACKAGE_ARCH}-${VERSION}-portable.zip"
installer_path="${DIST_DIR}/${PACKAGE_ID}-windows-${PACKAGE_ARCH}-${VERSION}-installer.exe"
rm -rf "${stage_dir}"
mkdir -p "${stage_dir}/plugins/platforms" "${stage_dir}/fonts"

require_file "${BUILD_DIR}/net-tunnel-gui.exe"
require_file "${BUILD_DIR}/net-tunnel-cli.exe"
cp -f "${BUILD_DIR}/net-tunnel-gui.exe" "${stage_dir}/${PACKAGE_ID}-gui.exe"
cp -f "${BUILD_DIR}/net-tunnel-cli.exe" "${stage_dir}/${PACKAGE_ID}-cli.exe"

for file in config.example.json README.md LICENSE THIRD_PARTY_NOTICES.md; do
    if [[ -f "${PROJECT_DIR}/${file}" ]]; then
        cp -f "${PROJECT_DIR}/${file}" "${stage_dir}/"
    fi
done

for dll in Qt5Core.dll Qt5Network.dll Qt5Gui.dll Qt5Widgets.dll; do
    cp -f "${QT_BIN}/${dll}" "${stage_dir}/"
done
copy_glob_optional "${stage_dir}" \
    "${QT_BIN}/libssl*.dll" \
    "${QT_BIN}/libcrypto*.dll" \
    "${QT_BIN}/ssleay32.dll" \
    "${QT_BIN}/libeay32.dll" \
    "${QT_BIN}/zlib*.dll" \
    "${QT_BIN}/icu*.dll"

cp -f "${QT_PLUGINS}/platforms/qwindows.dll" "${stage_dir}/plugins/platforms/"
if [[ -d "${QT_PLUGINS}/imageformats" ]]; then
    mkdir -p "${stage_dir}/plugins/imageformats"
    copy_glob_optional "${stage_dir}/plugins/imageformats" "${QT_PLUGINS}/imageformats/*.dll"
fi

for runtime_dll in "${MINGW_RUNTIME_DLLS[@]}"; do
    copy_first_runtime_dll "${runtime_dll}" "${stage_dir}" || true
done
if [[ ! -f "${stage_dir}/libstdc++-6.dll" || ! -f "${stage_dir}/libwinpthread-1.dll" ]]; then
    echo "warning: MinGW C++ runtime DLLs were not fully found; Windows package may need external runtime DLLs" >&2
fi

font_dir="${PROJECT_DIR}/third_party/fonts"
if [[ -d "${font_dir}" ]]; then
    copy_glob_optional "${stage_dir}/fonts" "${font_dir}"/*.ttf "${font_dir}"/*.ttc
fi

cat > "${stage_dir}/qt.conf" <<'EOF'
[Paths]
Prefix = .
Plugins = plugins
EOF

rm -f "${zip_path}"
(
    cd "${stage_dir}"
    zip -qr "${zip_path}" .
)

if ((BUILD_INSTALLER == 1)); then
    if makensis_path="$(find_makensis)"; then
        nsis_script="${WORK_DIR}/${PACKAGE_ID}-${PACKAGE_ARCH}.nsi"
        nsis_install_dir='$PROGRAMFILES'
        if [[ "${PACKAGE_ARCH}" == "x86_64" || "${PACKAGE_ARCH}" == "arm64" ]]; then
            nsis_install_dir='$PROGRAMFILES64'
        fi
        cat > "${nsis_script}" <<EOF
Unicode true
Name "${APP_NAME}"
OutFile "${installer_path}"
InstallDir "${nsis_install_dir}\\${APP_NAME}"
RequestExecutionLevel admin
SetCompressor /SOLID lzma

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Install"
  SetOutPath "\$INSTDIR"
  File /r "${stage_dir}/*"
  CreateDirectory "\$SMPROGRAMS\\${APP_NAME}"
  CreateShortCut "\$SMPROGRAMS\\${APP_NAME}\\${APP_NAME}.lnk" "\$INSTDIR\\${PACKAGE_ID}-gui.exe"
  CreateShortCut "\$SMPROGRAMS\\${APP_NAME}\\CLI.lnk" "\$INSTDIR\\${PACKAGE_ID}-cli.exe"
  WriteUninstaller "\$INSTDIR\\Uninstall.exe"
SectionEnd

Section "Uninstall"
  Delete "\$SMPROGRAMS\\${APP_NAME}\\${APP_NAME}.lnk"
  Delete "\$SMPROGRAMS\\${APP_NAME}\\CLI.lnk"
  RMDir "\$SMPROGRAMS\\${APP_NAME}"
  RMDir /r "\$INSTDIR"
SectionEnd
EOF
        "${makensis_path}" "${nsis_script}"
    else
        echo "warning: makensis not found; installer exe was not built" >&2
    fi
fi

echo "Package: ${PACKAGE_ID}"
echo "App name: ${APP_NAME}"
echo "Version: ${VERSION}"
echo "Arch: ${PACKAGE_ARCH}"
echo "Qt root: ${QT_ROOT}"
echo "MinGW triple: ${MINGW_TRIPLE}"
echo "Built Windows portable package: ${zip_path}"
if [[ -f "${installer_path}" ]]; then
    echo "Built Windows installer: ${installer_path}"
fi
