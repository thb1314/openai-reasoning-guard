#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
REPO_DIR="$(cd -- "${PROJECT_DIR}/.." && pwd)"

PACKAGE_ID="${PACKAGE_ID:-openai-reasoning-guard}"
APP_NAME="${APP_NAME:-OpenAI Reasoning Guard}"
GUI_COMMAND="${GUI_COMMAND:-${PACKAGE_ID}-gui}"
CLI_COMMAND="${CLI_COMMAND:-${PACKAGE_ID}-cli}"
DESKTOP_ID="${DESKTOP_ID:-${PACKAGE_ID}}"
CONFIG_DIR_NAME="${CONFIG_DIR_NAME:-${PACKAGE_ID}}"
LEGACY_CONFIG_DIR_NAME="${LEGACY_CONFIG_DIR_NAME:-net-tunnel-cpp-client}"
ICON_SOURCE="${ICON_SOURCE:-${PROJECT_DIR}/assets/openai-reasoning-guard-icon-1024.png}"
VERSION="${VERSION:-$(sed -n 's/^project([^ ]* VERSION \([^ ]*\).*/\1/p' "${PROJECT_DIR}/CMakeLists.txt")}"
VERSION="${VERSION:-0.1.0}"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build-package}"
DIST_DIR="${DIST_DIR:-${PROJECT_DIR}/dist}"
WORK_DIR="${WORK_DIR:-${PROJECT_DIR}/.package-work}"
TOOL_DIR="${TOOL_DIR:-${HOME}/.cache/${PACKAGE_ID}/package-tools}"
INSTALL_PREFIX="${INSTALL_PREFIX:-/opt/${PACKAGE_ID}}"
QT_ROOT="${QT_ROOT:-}"
LOCAL_QT_BASE="${LOCAL_QT_BASE:-/mnt/data/qt-2080ti-sync}"
OPENSSL_ROOT="${OPENSSL_ROOT:-}"
RPM_RELEASE="${RPM_RELEASE:-1}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
SKIP_BUILD="${SKIP_BUILD:-0}"
BUILD_TESTS="${BUILD_TESTS:-OFF}"
APPIMAGE_STAGE_ONLY="${APPIMAGE_STAGE_ONLY:-0}"

usage() {
    cat <<EOF
Usage: $(basename "$0") [--deb] [--rpm] [--appimage] [--all] [--skip-build] [--clean]

Build Linux packages for ${PACKAGE_ID}.

Outputs:
  ${DIST_DIR}/${PACKAGE_ID}_${VERSION}_<arch>.deb
  ${DIST_DIR}/${PACKAGE_ID}-${VERSION}-${RPM_RELEASE}.<arch>.rpm
  ${DIST_DIR}/${PACKAGE_ID}-gui-${VERSION}-<appimage-arch>.AppImage
  ${DIST_DIR}/${PACKAGE_ID}-cli-${VERSION}-<appimage-arch>.AppImage

Environment overrides:
  VERSION=0.1.0
  PACKAGE_ID=${PACKAGE_ID}
  APP_NAME="${APP_NAME}"
  GUI_COMMAND=${GUI_COMMAND}
  CLI_COMMAND=${CLI_COMMAND}
  ICON_SOURCE=${ICON_SOURCE}
  BUILD_DIR=${BUILD_DIR}
  DIST_DIR=${DIST_DIR}
  WORK_DIR=${WORK_DIR}
  TOOL_DIR=${TOOL_DIR}
  QT_ROOT=/path/to/qt5
  DEB_ARCH=amd64
  RPM_ARCH=x86_64
  RPM_RELEASE=${RPM_RELEASE}
  APPIMAGE_ARCH=x86_64
  APPIMAGETOOL=/path/to/appimagetool
  LOCAL_QT_BASE=${LOCAL_QT_BASE}
  OPENSSL_ROOT=/path/to/openssl
  JOBS=${JOBS}
  SKIP_BUILD=1
  APPIMAGE_STAGE_ONLY=1
  DOWNLOAD_PROXY=http://127.0.0.1:7890
EOF
}

BUILD_DEB=0
BUILD_RPM=0
BUILD_APPIMAGE=0
CLEAN=0

if (($# == 0)); then
    BUILD_DEB=1
    BUILD_RPM=1
    BUILD_APPIMAGE=1
fi

while (($# > 0)); do
    case "$1" in
        --deb)
            BUILD_DEB=1
            ;;
        --rpm)
            BUILD_RPM=1
            ;;
        --appimage)
            BUILD_APPIMAGE=1
            ;;
        --all)
            BUILD_DEB=1
            BUILD_RPM=1
            BUILD_APPIMAGE=1
            ;;
        --skip-build)
            SKIP_BUILD=1
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

if ((BUILD_DEB == 0 && BUILD_RPM == 0 && BUILD_APPIMAGE == 0)); then
    BUILD_DEB=1
    BUILD_RPM=1
    BUILD_APPIMAGE=1
fi

require_tool() {
    local tool="$1"
    if ! command -v "${tool}" >/dev/null 2>&1; then
        echo "required tool not found: ${tool}" >&2
        exit 2
    fi
}

detect_qt_root() {
    if [[ -n "${QT_ROOT}" ]]; then
        printf '%s\n' "${QT_ROOT}"
        return
    fi
    # Local builds intentionally default to the self-built Qt tree, not the
    # distro Qt5 under /usr. CI must pass QT_ROOT explicitly.
    if [[ -f "${BUILD_DIR}/CMakeCache.txt" ]]; then
        local cached
        cached="$(sed -n 's/^NET_TUNNEL_QT_SDK_ROOT:PATH=//p' "${BUILD_DIR}/CMakeCache.txt" | tail -n 1)"
        if [[ -n "${cached}" && "${cached}" == "${LOCAL_QT_BASE}/"* && -f "${cached}/lib/libQt5Core.so.5" ]]; then
            printf '%s\n' "${cached}"
            return
        fi
    fi
    for candidate in \
        "${LOCAL_QT_BASE}/qt5-openssl" \
        "${LOCAL_QT_BASE}/qt-5.9.6-linux-gcc"; do
        if [[ -f "${candidate}/lib/libQt5Core.so.5" ]]; then
            printf '%s\n' "${candidate}"
            return
        fi
    done
    echo "unable to find self-built Qt root under ${LOCAL_QT_BASE}; set QT_ROOT=/path/to/qt5" >&2
    exit 2
}

dpkg_arch() {
    if command -v dpkg >/dev/null 2>&1; then
        dpkg --print-architecture
    else
        case "$(uname -m)" in
            x86_64) echo amd64 ;;
            i386|i486|i586|i686) echo i386 ;;
            aarch64|arm64) echo arm64 ;;
            armv7l|armv7*) echo armhf ;;
            *) uname -m ;;
        esac
    fi
}

rpm_arch_for() {
    case "$1" in
        amd64|x86_64) echo x86_64 ;;
        i386|i486|i586|i686) echo i686 ;;
        arm64|aarch64) echo aarch64 ;;
        armhf|armv7l|armv7*) echo armv7hl ;;
        armel|armv6l|armv6*) echo armv6hl ;;
        *) echo "$1" ;;
    esac
}

appimage_arch_for() {
    case "$1" in
        amd64|x86_64) echo x86_64 ;;
        i386|i486|i586|i686) echo i686 ;;
        arm64|aarch64) echo aarch64 ;;
        armhf|armv7l|armv7*) echo armhf ;;
        *) echo "$1" ;;
    esac
}

download_appimagetool() {
    local appimage_arch="$1"
    if [[ -n "${APPIMAGETOOL:-}" ]]; then
        if [[ -x "${APPIMAGETOOL}" ]]; then
            printf '%s\n' "${APPIMAGETOOL}"
            return
        fi
        echo "APPIMAGETOOL is not executable: ${APPIMAGETOOL}" >&2
        exit 2
    fi

    local tool="${TOOL_DIR}/appimagetool-${appimage_arch}.AppImage"
    mkdir -p "${TOOL_DIR}"
    if [[ ! -x "${tool}" ]]; then
        local url="https://github.com/AppImage/AppImageKit/releases/download/continuous/appimagetool-${appimage_arch}.AppImage"
        echo "Downloading appimagetool: ${url}" >&2
        if command -v curl >/dev/null 2>&1; then
            local curl_args=(-L --fail --connect-timeout 30 -o "${tool}")
            if [[ -n "${DOWNLOAD_PROXY:-}" ]]; then
                curl_args+=(--proxy "${DOWNLOAD_PROXY}")
            fi
            curl "${curl_args[@]}" "${url}" || {
                rm -f "${tool}"
                echo "failed to download appimagetool" >&2
                exit 2
            }
        elif command -v wget >/dev/null 2>&1; then
            local wget_args=(-O "${tool}")
            if [[ -n "${DOWNLOAD_PROXY:-}" ]]; then
                wget_args+=(--proxy=on -e "https_proxy=${DOWNLOAD_PROXY}" -e "http_proxy=${DOWNLOAD_PROXY}")
            fi
            wget "${wget_args[@]}" "${url}" || {
                rm -f "${tool}"
                echo "failed to download appimagetool" >&2
                exit 2
            }
        else
            echo "curl or wget is required to download appimagetool" >&2
            exit 2
        fi
        chmod +x "${tool}"
    fi
    printf '%s\n' "${tool}"
}

copy_qt_runtime() {
    local root="$1"
    local qt_root="$2"
    local app_root="${root}${INSTALL_PREFIX}"
    local qt_lib_dir="${qt_root}/lib"

    mkdir -p "${app_root}/qt/lib" \
             "${app_root}/qt/plugins/platforms" \
             "${app_root}/qt/fonts" \
             "${app_root}/share"

    local libs=(
        libQt5Core.so.5
        libQt5Network.so.5
        libQt5Gui.so.5
        libQt5Widgets.so.5
        libQt5XcbQpa.so.5
    )
    local lib
    for lib in "${libs[@]}"; do
        if [[ -e "${qt_lib_dir}/${lib}" ]]; then
            cp -a "${qt_lib_dir}/${lib}"* "${app_root}/qt/lib/"
        else
            echo "missing Qt runtime library: ${qt_lib_dir}/${lib}" >&2
            exit 2
        fi
    done

    local openssl_dirs=()
    if [[ -n "${OPENSSL_ROOT}" ]]; then
        openssl_dirs+=("${OPENSSL_ROOT}/lib" "${OPENSSL_ROOT}/lib64")
    fi
    openssl_dirs+=("${qt_lib_dir}")

    local patterns=("libssl.so*" "libcrypto.so*")
    local pattern
    local copied_openssl=0
    shopt -s nullglob
    for pattern in "${patterns[@]}"; do
        local openssl_dir
        for openssl_dir in "${openssl_dirs[@]}"; do
            local matches=("${openssl_dir}"/${pattern})
            if ((${#matches[@]} > 0)); then
                cp -a "${matches[@]}" "${app_root}/qt/lib/"
                copied_openssl=1
                break
            fi
        done
    done
    shopt -u nullglob
    if ((copied_openssl == 0)); then
        echo "warning: no OpenSSL runtime libraries found under Qt root; relying on target system libraries" >&2
    fi

    cp -a "${qt_root}/plugins/platforms/libqxcb.so" "${app_root}/qt/plugins/platforms/"
    if compgen -G "${PROJECT_DIR}/third_party/fonts/*.ttf" >/dev/null; then
        cp -a "${PROJECT_DIR}/third_party/fonts/"*.ttf "${app_root}/qt/fonts/"
    fi

    cat > "${app_root}/qt/qt.conf" <<'EOF'
[Paths]
Prefix = .
Plugins = plugins
Libraries = lib
EOF
}

write_fallback_svg_icon() {
    local path="$1"
    mkdir -p "$(dirname -- "${path}")"
    cat > "${path}" <<'EOF'
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 256 256">
  <rect width="256" height="256" rx="48" fill="#10243d"/>
  <path d="M54 151c18 40 57 61 108 46 28-9 45-29 50-59 4-26-5-53-25-70-22-19-54-23-82-11-25 11-42 32-47 58" fill="none" stroke="#63d6ff" stroke-width="18" stroke-linecap="round"/>
  <path d="M82 96h92M82 128h62M82 160h92" stroke="#e8fbff" stroke-width="16" stroke-linecap="round"/>
  <circle cx="183" cy="82" r="18" fill="#ffcc4d"/>
</svg>
EOF
}

write_png_icon_size() {
    local source="$1"
    local target="$2"
    local size="$3"
    mkdir -p "$(dirname -- "${target}")"
    if python3 - "${source}" "${target}" "${size}" <<'PY'
import sys
try:
    from PIL import Image
except Exception:
    sys.exit(2)
src, dst, size = sys.argv[1], sys.argv[2], int(sys.argv[3])
Image.open(src).convert("RGBA").resize((size, size), Image.LANCZOS).save(dst)
PY
    then
        return
    fi
    cp -a "${source}" "${target}"
}

write_icon_assets() {
    local root="$1"
    local svg_dir="${root}/usr/share/icons/hicolor/scalable/apps"
    local size
    mkdir -p "${svg_dir}"
    if [[ -f "${ICON_SOURCE}" ]]; then
        for size in 16 24 32 48 64 128 256 512 1024; do
            local png_dir="${root}/usr/share/icons/hicolor/${size}x${size}/apps"
            write_png_icon_size "${ICON_SOURCE}" "${png_dir}/${DESKTOP_ID}.png" "${size}"
        done
    else
        write_fallback_svg_icon "${svg_dir}/${DESKTOP_ID}.svg"
    fi
}

appimage_icon_path() {
    local appdir="$1"
    if [[ -f "${appdir}/usr/share/icons/hicolor/256x256/apps/${DESKTOP_ID}.png" ]]; then
        printf '%s\n' "${appdir}/usr/share/icons/hicolor/256x256/apps/${DESKTOP_ID}.png"
    elif [[ -f "${appdir}/usr/share/icons/hicolor/1024x1024/apps/${DESKTOP_ID}.png" ]]; then
        printf '%s\n' "${appdir}/usr/share/icons/hicolor/1024x1024/apps/${DESKTOP_ID}.png"
    else
        printf '%s\n' "${appdir}/usr/share/icons/hicolor/scalable/apps/${DESKTOP_ID}.svg"
    fi
}

write_desktop_file() {
    local path="$1"
    local exec_value="$2"
    mkdir -p "$(dirname -- "${path}")"
    cat > "${path}" <<EOF
[Desktop Entry]
Type=Application
Name=${APP_NAME}
Comment=Local OpenAI-compatible reasoning guard proxy
Exec=${exec_value}
Icon=${DESKTOP_ID}
Terminal=false
Categories=Network;Qt;
StartupNotify=true
StartupWMClass=${GUI_COMMAND}
EOF
}

validate_desktop_files() {
    local root="$1"
    local app_dir="${root}/usr/share/applications"
    local expected="${app_dir}/${DESKTOP_ID}.desktop"
    local desktop_files=()

    mapfile -t desktop_files < <(find "${app_dir}" -maxdepth 1 -type f -name '*.desktop' -print | sort)
    if ((${#desktop_files[@]} != 1)) || [[ "${desktop_files[0]:-}" != "${expected}" ]]; then
        echo "expected exactly one desktop entry: ${expected}" >&2
        printf 'found desktop entry: %s\n' "${desktop_files[@]}" >&2
        return 1
    fi
    grep -Fqx "Exec=${GUI_COMMAND}" "${expected}"
    grep -Fqx "StartupWMClass=${GUI_COMMAND}" "${expected}"
}

write_wrappers() {
    local root="$1"
    local app_root="${root}${INSTALL_PREFIX}"
    mkdir -p "${app_root}/bin" "${root}/usr/bin"

    cat > "${app_root}/bin/${GUI_COMMAND}" <<EOF
#!/usr/bin/env bash
set -e
SELF="\$(readlink -f "\$0")"
APP_ROOT="\$(cd -- "\$(dirname -- "\$SELF")/.." && pwd)"
CONFIG_BASE="\${XDG_CONFIG_HOME:-\${HOME}/.config}"
CONFIG_DIR="\${CONFIG_BASE}/${CONFIG_DIR_NAME}"
LEGACY_CONFIG_DIR="\${CONFIG_BASE}/${LEGACY_CONFIG_DIR_NAME}"
mkdir -p "\$CONFIG_DIR"
if [[ ! -f "\$CONFIG_DIR/config.json" && -f "\$LEGACY_CONFIG_DIR/config.json" ]]; then
    cp "\$LEGACY_CONFIG_DIR/config.json" "\$CONFIG_DIR/config.json"
fi
if [[ ! -f "\$CONFIG_DIR/config.json" && -f "\$APP_ROOT/share/config.example.json" ]]; then
    cp "\$APP_ROOT/share/config.example.json" "\$CONFIG_DIR/config.json"
fi
export NET_TUNNEL_CONFIG="\${NET_TUNNEL_CONFIG:-\$CONFIG_DIR/config.json}"
export LD_LIBRARY_PATH="\$APP_ROOT/qt/lib:\${LD_LIBRARY_PATH:-}"
export QT_PLUGIN_PATH="\$APP_ROOT/qt/plugins:\${QT_PLUGIN_PATH:-}"
export QT_QPA_PLATFORM_PLUGIN_PATH="\$APP_ROOT/qt/plugins/platforms"
export QT_QPA_FONTDIR="\$APP_ROOT/qt/fonts"
exec "\$APP_ROOT/bin/${GUI_COMMAND}.real" "\$@"
EOF

    cat > "${app_root}/bin/${CLI_COMMAND}" <<EOF
#!/usr/bin/env bash
set -e
SELF="\$(readlink -f "\$0")"
APP_ROOT="\$(cd -- "\$(dirname -- "\$SELF")/.." && pwd)"
CONFIG_BASE="\${XDG_CONFIG_HOME:-\${HOME}/.config}"
CONFIG_DIR="\${CONFIG_BASE}/${CONFIG_DIR_NAME}"
LEGACY_CONFIG_DIR="\${CONFIG_BASE}/${LEGACY_CONFIG_DIR_NAME}"
mkdir -p "\$CONFIG_DIR"
if [[ ! -f "\$CONFIG_DIR/config.json" && -f "\$LEGACY_CONFIG_DIR/config.json" ]]; then
    cp "\$LEGACY_CONFIG_DIR/config.json" "\$CONFIG_DIR/config.json"
fi
if [[ ! -f "\$CONFIG_DIR/config.json" && -f "\$APP_ROOT/share/config.example.json" ]]; then
    cp "\$APP_ROOT/share/config.example.json" "\$CONFIG_DIR/config.json"
fi
export NET_TUNNEL_CONFIG="\${NET_TUNNEL_CONFIG:-\$CONFIG_DIR/config.json}"
export LD_LIBRARY_PATH="\$APP_ROOT/qt/lib:\${LD_LIBRARY_PATH:-}"
export QT_PLUGIN_PATH="\$APP_ROOT/qt/plugins:\${QT_PLUGIN_PATH:-}"
export QT_QPA_PLATFORM_PLUGIN_PATH="\$APP_ROOT/qt/plugins/platforms"
export QT_QPA_FONTDIR="\$APP_ROOT/qt/fonts"
exec "\$APP_ROOT/bin/${CLI_COMMAND}.real" "\$@"
EOF

    chmod +x "${app_root}/bin/${GUI_COMMAND}" "${app_root}/bin/${CLI_COMMAND}"
    ln -s "${INSTALL_PREFIX}/bin/${GUI_COMMAND}" "${root}/usr/bin/${GUI_COMMAND}"
    ln -s "${INSTALL_PREFIX}/bin/${CLI_COMMAND}" "${root}/usr/bin/${CLI_COMMAND}"
    ln -s "${INSTALL_PREFIX}/bin/${GUI_COMMAND}" "${root}/usr/bin/net-tunnel-gui"
    ln -s "${INSTALL_PREFIX}/bin/${CLI_COMMAND}" "${root}/usr/bin/net-tunnel-cli"
}

stage_common_root() {
    local root="$1"
    local qt_root="$2"
    rm -rf "${root}"
    mkdir -p "${root}${INSTALL_PREFIX}/bin" \
             "${root}${INSTALL_PREFIX}/share" \
             "${root}/usr/share/applications" \
             "${root}/usr/share/icons/hicolor/scalable/apps" \
             "${root}/usr/share/icons/hicolor/1024x1024/apps"

    cp -a "${BUILD_DIR}/net-tunnel-gui" "${root}${INSTALL_PREFIX}/bin/${GUI_COMMAND}.real"
    cp -a "${BUILD_DIR}/net-tunnel-cli" "${root}${INSTALL_PREFIX}/bin/${CLI_COMMAND}.real"
    cp -a "${PROJECT_DIR}/config.example.json" "${root}${INSTALL_PREFIX}/share/config.example.json"
    cp -a "${PROJECT_DIR}/README.md" "${root}${INSTALL_PREFIX}/share/README.md"

    copy_qt_runtime "${root}" "${qt_root}"
    write_wrappers "${root}"
    write_icon_assets "${root}"
    write_desktop_file "${root}/usr/share/applications/${DESKTOP_ID}.desktop" "${GUI_COMMAND}"
    validate_desktop_files "${root}"

    if command -v patchelf >/dev/null 2>&1; then
        patchelf --set-rpath '$ORIGIN/../qt/lib:$ORIGIN' "${root}${INSTALL_PREFIX}/bin/${GUI_COMMAND}.real"
        patchelf --set-rpath '$ORIGIN/../qt/lib:$ORIGIN' "${root}${INSTALL_PREFIX}/bin/${CLI_COMMAND}.real"
    fi
}

build_project() {
    if [[ "${SKIP_BUILD}" == "1" ]]; then
        echo "Skipping build because SKIP_BUILD=1"
        return
    fi
    cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DNET_TUNNEL_BUILD_TESTS="${BUILD_TESTS}" \
        -DNET_TUNNEL_QT_SDK_ROOT="${QT_ROOT_RESOLVED}"
    cmake --build "${BUILD_DIR}" -j"${JOBS}"
}

build_deb() {
    require_tool dpkg-deb
    local root="${WORK_DIR}/debroot"
    local out="${DIST_DIR}/${PACKAGE_ID}_${VERSION}_${DEB_ARCH}.deb"
    stage_common_root "${root}" "${QT_ROOT_RESOLVED}"

    mkdir -p "${root}/DEBIAN"
    cat > "${root}/DEBIAN/control" <<EOF
Package: ${PACKAGE_ID}
Version: ${VERSION}
Section: net
Priority: optional
Architecture: ${DEB_ARCH}
Maintainer: OpenAI Reasoning Guard maintainers <maintainers@example.invalid>
Depends: libc6, libstdc++6, libgcc-s1 | libgcc1, zlib1g, libglib2.0-0, libx11-6, libxcb1, libxau6, libxdmcp6, libsm6, libice6, libuuid1, libbsd0, libmd0, libpcre2-8-0
Description: OpenAI-compatible reasoning degradation guard proxy
 A local Qt/C++ OpenAI-compatible proxy that guards against suspected degraded reasoning responses by inspecting usage reasoning token signals and retrying upstream requests.
EOF

    if dpkg-deb --help 2>&1 | grep -q -- '--root-owner-group'; then
        dpkg-deb --root-owner-group --build "${root}" "${out}"
    else
        fakeroot dpkg-deb --build "${root}" "${out}" 2>/dev/null || dpkg-deb --build "${root}" "${out}"
    fi
    echo "Built deb: ${out}"
}

build_rpm() {
    require_tool rpmbuild
    local root="${WORK_DIR}/rpmroot"
    local rpm_top="${WORK_DIR}/rpmbuild"
    local spec="${WORK_DIR}/${PACKAGE_ID}.spec"
    local out="${DIST_DIR}/${PACKAGE_ID}-${VERSION}-${RPM_RELEASE}.${RPM_ARCH}.rpm"

    stage_common_root "${root}" "${QT_ROOT_RESOLVED}"

    rm -rf "${rpm_top}"
    mkdir -p "${rpm_top}/BUILD" \
             "${rpm_top}/BUILDROOT" \
             "${rpm_top}/RPMS" \
             "${rpm_top}/SOURCES" \
             "${rpm_top}/SPECS" \
             "${rpm_top}/SRPMS"

    cat > "${spec}" <<EOF
Name: ${PACKAGE_ID}
Version: ${VERSION}
Release: ${RPM_RELEASE}
Summary: OpenAI-compatible reasoning degradation guard proxy
License: MIT
URL: https://github.com/thb1314/openai-reasoning-guard
Requires: glibc, libstdc++

%description
A local Qt/C++ OpenAI-compatible proxy that guards against suspected degraded
reasoning responses by inspecting usage reasoning token signals and retrying
upstream requests.

%prep

%build

%install
mkdir -p "%{buildroot}"
cp -a "%{_rpm_staging_root}/." "%{buildroot}/"

%files
%defattr(-,root,root,-)
${INSTALL_PREFIX}
/usr/bin/${GUI_COMMAND}
/usr/bin/${CLI_COMMAND}
/usr/bin/net-tunnel-gui
/usr/bin/net-tunnel-cli
/usr/share/applications/${DESKTOP_ID}.desktop
EOF
    cat >> "${spec}" <<EOF
/usr/share/icons/hicolor/*/apps/${DESKTOP_ID}.*
EOF

    rpmbuild -bb "${spec}" \
        --target "${RPM_ARCH}" \
        --define "_topdir ${rpm_top}" \
        --define "_rpm_staging_root ${root}"

    local built
    built="$(find "${rpm_top}/RPMS" -type f -name '*.rpm' -print -quit)"
    if [[ -z "${built}" ]]; then
        echo "rpmbuild completed but no rpm was produced" >&2
        exit 2
    fi
    cp -a "${built}" "${out}"
    echo "Built rpm: ${out}"
}

write_appimage_desktop_file() {
    local path="$1"
    local name="$2"
    local terminal="$3"
    mkdir -p "$(dirname -- "${path}")"
    cat > "${path}" <<EOF
[Desktop Entry]
Type=Application
Name=${name}
Comment=Local OpenAI-compatible reasoning guard proxy
Exec=AppRun
Icon=${DESKTOP_ID}
Terminal=${terminal}
Categories=Network;Qt;
StartupNotify=true
StartupWMClass=${command}
EOF
}

build_appimage_variant() {
    local variant="$1"
    local command="$2"
    local name="$3"
    local terminal="$4"
    local appimage_tool
    local appdir="${WORK_DIR}/AppDir-${variant}"
    local out="${DIST_DIR}/${PACKAGE_ID}-${variant}-${VERSION}-${APPIMAGE_ARCH}.AppImage"

    stage_common_root "${appdir}" "${QT_ROOT_RESOLVED}"
    rm -rf "${appdir}/usr/bin"
    write_appimage_desktop_file "${appdir}/${DESKTOP_ID}-${variant}.desktop" "${name}" "${terminal}"

    local icon_path
    icon_path="$(appimage_icon_path "${appdir}")"
    cp -a "${icon_path}" "${appdir}/${DESKTOP_ID}.${icon_path##*.}"
    ln -sf "${DESKTOP_ID}.${icon_path##*.}" "${appdir}/.DirIcon"

    cat > "${appdir}/AppRun" <<'EOF'
#!/usr/bin/env bash
set -e
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec "${HERE}__INSTALL_PREFIX__/bin/__APPIMAGE_COMMAND__" "$@"
EOF
    sed -i "s#__INSTALL_PREFIX__#${INSTALL_PREFIX}#g" "${appdir}/AppRun"
    sed -i "s#__APPIMAGE_COMMAND__#${command}#g" "${appdir}/AppRun"
    chmod +x "${appdir}/AppRun"

    if [[ "${APPIMAGE_STAGE_ONLY}" == "1" ]]; then
        echo "Prepared AppDir: ${appdir}"
        return
    fi

    if command -v appimagetool >/dev/null 2>&1; then
        appimage_tool="$(command -v appimagetool)"
    else
        appimage_tool="$(download_appimagetool "${APPIMAGE_ARCH}")"
    fi

    rm -f "${out}"
    ARCH="${APPIMAGE_ARCH}" APPIMAGE_EXTRACT_AND_RUN=1 "${appimage_tool}" "${appdir}" "${out}"
    chmod +x "${out}"
    echo "Built AppImage: ${out}"
}

build_appimage() {
    build_appimage_variant "gui" "${GUI_COMMAND}" "${APP_NAME}" "false"
    build_appimage_variant "cli" "${CLI_COMMAND}" "${APP_NAME} CLI" "true"
}

require_tool cmake
require_tool sed
require_tool cp
require_tool readlink

if ((CLEAN == 1)); then
    rm -rf "${WORK_DIR}" "${DIST_DIR}"
fi
mkdir -p "${DIST_DIR}" "${WORK_DIR}"

QT_ROOT_RESOLVED="$(detect_qt_root)"
DEB_ARCH="${DEB_ARCH:-$(dpkg_arch)}"
RPM_ARCH="${RPM_ARCH:-$(rpm_arch_for "${DEB_ARCH}")}"
APPIMAGE_ARCH="${APPIMAGE_ARCH:-$(appimage_arch_for "${DEB_ARCH}")}"

echo "Package: ${PACKAGE_ID}"
echo "Version: ${VERSION}"
echo "Deb arch: ${DEB_ARCH}"
echo "RPM arch: ${RPM_ARCH}"
echo "AppImage arch: ${APPIMAGE_ARCH}"
echo "Qt root: ${QT_ROOT_RESOLVED}"
echo "Build dir: ${BUILD_DIR}"
echo "Dist dir: ${DIST_DIR}"

build_project

if ((BUILD_DEB == 1)); then
    build_deb
fi
if ((BUILD_RPM == 1)); then
    build_rpm
fi
if ((BUILD_APPIMAGE == 1)); then
    build_appimage
fi
