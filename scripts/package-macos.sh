#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"

PACKAGE_ID="${PACKAGE_ID:-openai-reasoning-guard}"
APP_NAME="${APP_NAME:-OpenAI Reasoning Guard}"
GUI_COMMAND="${GUI_COMMAND:-${PACKAGE_ID}-gui}"
CLI_COMMAND="${CLI_COMMAND:-${PACKAGE_ID}-cli}"
BUNDLE_ID="${BUNDLE_ID:-io.github.thb1314.openai-reasoning-guard}"
ICON_SOURCE="${ICON_SOURCE:-${PROJECT_DIR}/assets/openai-reasoning-guard-icon-1024.png}"
VERSION="${VERSION:-$(sed -n 's/^project([^ ]* VERSION \([^ ]*\).*/\1/p' "${PROJECT_DIR}/CMakeLists.txt")}"
VERSION="${VERSION:-0.1.0}"
ARCH="${PACKAGE_ARCH:-$(uname -m)}"
BUILD_DIR="${BUILD_DIR:-${PROJECT_DIR}/build-package-macos-${ARCH}}"
DIST_DIR="${DIST_DIR:-${PROJECT_DIR}/dist}"
WORK_DIR="${WORK_DIR:-${PROJECT_DIR}/.package-work/macos-${ARCH}}"
QT_ROOT="${QT_ROOT:-}"
BUILD_TESTS="${BUILD_TESTS:-OFF}"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 2)}"
SKIP_BUILD="${SKIP_BUILD:-0}"
MACOS_CODESIGN="${MACOS_CODESIGN:-1}"
MACOS_CODESIGN_IDENTITY="${MACOS_CODESIGN_IDENTITY:--}"
MACOS_CODESIGN_TIMESTAMP="${MACOS_CODESIGN_TIMESTAMP:-none}"
CLEAN=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [--arch x86_64|aarch64] [--skip-build] [--clean]

Build a macOS dmg for ${PACKAGE_ID}.

Environment overrides:
  QT_ROOT=/path/to/qt5
  PACKAGE_ARCH=x86_64
  CMAKE_OSX_ARCHITECTURES=x86_64
  BUILD_DIR=${BUILD_DIR}
  DIST_DIR=${DIST_DIR}
  WORK_DIR=${WORK_DIR}
  MACDEPLOYQT=/path/to/macdeployqt
  MACOS_CODESIGN=1
  MACOS_CODESIGN_IDENTITY=-          # ad-hoc signing by default
  MACOS_CODESIGN_TIMESTAMP=none
EOF
}

while (($# > 0)); do
    case "$1" in
        --arch)
            shift
            ARCH="${1:?missing arch}"
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

case "${ARCH}" in
    arm64) ARCH="aarch64" ;;
esac

cmake_arch_for() {
    case "$1" in
        aarch64) echo arm64 ;;
        x86_64) echo x86_64 ;;
        *) echo "$1" ;;
    esac
}

minimum_macos_version_for() {
    if [[ -n "${MACOSX_DEPLOYMENT_TARGET:-}" ]]; then
        echo "${MACOSX_DEPLOYMENT_TARGET}"
        return
    fi
    case "$1" in
        aarch64) echo "11.0" ;;
        *) echo "10.13" ;;
    esac
}

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
    echo "QT_ROOT is required for macOS packaging" >&2
    exit 2
}

find_built_binary() {
    local name="$1"
    local candidate
    for candidate in \
        "${BUILD_DIR}/${name}" \
        "${BUILD_DIR}/Release/${name}" \
        "${BUILD_DIR}/src/${name}" \
        "${BUILD_DIR}/src/Release/${name}"; do
        if [[ -x "${candidate}" ]]; then
            printf '%s\n' "${candidate}"
            return
        fi
    done
    echo "built binary not found: ${name} under ${BUILD_DIR}" >&2
    exit 2
}

make_icns() {
    local source_png="$1"
    local out_icns="$2"
    local iconset="${WORK_DIR}/icon.iconset"
    if [[ ! -f "${source_png}" ]]; then
        echo "icon source missing: ${source_png}" >&2
        exit 2
    fi
    if ! command -v sips >/dev/null 2>&1 || ! command -v iconutil >/dev/null 2>&1; then
        echo "sips and iconutil are required to build the macOS app icon" >&2
        exit 2
    fi
    rm -rf "${iconset}"
    mkdir -p "${iconset}"
    sips -z 16 16     "${source_png}" --out "${iconset}/icon_16x16.png" >/dev/null
    sips -z 32 32     "${source_png}" --out "${iconset}/icon_16x16@2x.png" >/dev/null
    sips -z 32 32     "${source_png}" --out "${iconset}/icon_32x32.png" >/dev/null
    sips -z 64 64     "${source_png}" --out "${iconset}/icon_32x32@2x.png" >/dev/null
    sips -z 128 128   "${source_png}" --out "${iconset}/icon_128x128.png" >/dev/null
    sips -z 256 256   "${source_png}" --out "${iconset}/icon_128x128@2x.png" >/dev/null
    sips -z 256 256   "${source_png}" --out "${iconset}/icon_256x256.png" >/dev/null
    sips -z 512 512   "${source_png}" --out "${iconset}/icon_256x256@2x.png" >/dev/null
    sips -z 512 512   "${source_png}" --out "${iconset}/icon_512x512.png" >/dev/null
    cp "${source_png}" "${iconset}/icon_512x512@2x.png"
    iconutil -c icns "${iconset}" -o "${out_icns}"
}

build_project() {
    if [[ "${SKIP_BUILD}" == "1" ]]; then
        echo "Skipping build because SKIP_BUILD=1"
        return
    fi
    local cmake_arch="${CMAKE_OSX_ARCHITECTURES:-$(cmake_arch_for "${ARCH}")}"
    local deployment_target
    deployment_target="$(minimum_macos_version_for "${ARCH}")"
    cmake -S "${PROJECT_DIR}" -B "${BUILD_DIR}" \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="${cmake_arch}" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET="${deployment_target}" \
        -DNET_TUNNEL_BUILD_TESTS="${BUILD_TESTS}" \
        -DNET_TUNNEL_QT_SDK_ROOT="${QT_ROOT_RESOLVED}"
    cmake --build "${BUILD_DIR}" -j"${JOBS}"
}

stage_app_bundle() {
    local app_bundle="$1"
    local gui_binary
    local cli_binary
    gui_binary="$(find_built_binary net-tunnel-gui)"
    cli_binary="$(find_built_binary net-tunnel-cli)"

    rm -rf "${app_bundle}"
    mkdir -p "${app_bundle}/Contents/MacOS" \
             "${app_bundle}/Contents/Resources/bin"

    cp -a "${gui_binary}" "${app_bundle}/Contents/MacOS/${GUI_COMMAND}"
    cp -a "${cli_binary}" "${app_bundle}/Contents/Resources/bin/${CLI_COMMAND}"
    chmod +x "${app_bundle}/Contents/MacOS/${GUI_COMMAND}" \
             "${app_bundle}/Contents/Resources/bin/${CLI_COMMAND}"

    cp -a "${PROJECT_DIR}/config.example.json" "${app_bundle}/Contents/Resources/config.example.json"
    make_icns "${ICON_SOURCE}" "${app_bundle}/Contents/Resources/openai-reasoning-guard.icns"

    cat > "${app_bundle}/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "https://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key>
  <string>en</string>
  <key>CFBundleExecutable</key>
  <string>${GUI_COMMAND}</string>
  <key>CFBundleIdentifier</key>
  <string>${BUNDLE_ID}</string>
  <key>CFBundleDisplayName</key>
  <string>${APP_NAME}</string>
  <key>CFBundleName</key>
  <string>${APP_NAME}</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>${VERSION}</string>
  <key>CFBundleVersion</key>
  <string>${VERSION}</string>
  <key>CFBundleIconFile</key>
  <string>openai-reasoning-guard</string>
  <key>LSMinimumSystemVersion</key>
  <string>$(minimum_macos_version_for "${ARCH}")</string>
  <key>NSHighResolutionCapable</key>
  <true/>
</dict>
</plist>
EOF
    printf 'APPL????' > "${app_bundle}/Contents/PkgInfo"
}

deploy_qt() {
    local app_bundle="$1"
    local macdeployqt="${MACDEPLOYQT:-${QT_ROOT_RESOLVED}/bin/macdeployqt}"
    if [[ ! -x "${macdeployqt}" ]]; then
        echo "macdeployqt not found or not executable: ${macdeployqt}" >&2
        exit 2
    fi
    "${macdeployqt}" "${app_bundle}" \
        -always-overwrite \
        -verbose=1 \
        "-executable=${app_bundle}/Contents/Resources/bin/${CLI_COMMAND}"
}

sign_path() {
    local path="$1"
    if [[ ! -e "${path}" ]]; then
        return
    fi
    local args=(--force --sign "${MACOS_CODESIGN_IDENTITY}")
    if [[ -n "${MACOS_CODESIGN_TIMESTAMP}" ]]; then
        args+=("--timestamp=${MACOS_CODESIGN_TIMESTAMP}")
    fi
    codesign "${args[@]}" "${path}"
}

sign_app_bundle() {
    local app_bundle="$1"
    if [[ "${MACOS_CODESIGN}" != "1" ]]; then
        echo "Skipping macOS codesign because MACOS_CODESIGN=${MACOS_CODESIGN}"
        return
    fi
    require_tool codesign

    local path
    while IFS= read -r path; do
        sign_path "${path}"
    done < <(find \
        "${app_bundle}/Contents/PlugIns" \
        "${app_bundle}/Contents/Frameworks" \
        -type f \( -name '*.dylib' -o -name '*.so' \) \
        -print 2>/dev/null)

    while IFS= read -r path; do
        sign_path "${path}"
    done < <(find "${app_bundle}/Contents/Frameworks" -depth -type d -name '*.framework' -print 2>/dev/null)

    sign_path "${app_bundle}/Contents/Resources/bin/${CLI_COMMAND}"
    sign_path "${app_bundle}/Contents/MacOS/${GUI_COMMAND}"

    local bundle_args=(--force --sign "${MACOS_CODESIGN_IDENTITY}")
    if [[ -n "${MACOS_CODESIGN_TIMESTAMP}" ]]; then
        bundle_args+=("--timestamp=${MACOS_CODESIGN_TIMESTAMP}")
    fi
    codesign "${bundle_args[@]}" --deep "${app_bundle}"
    codesign --verify --deep --strict --verbose=2 "${app_bundle}"
}

build_dmg() {
    require_tool hdiutil
    local app_bundle="${WORK_DIR}/${APP_NAME}.app"
    local dmg_root="${WORK_DIR}/dmgroot"
    local out="${DIST_DIR}/${PACKAGE_ID}-macos-${ARCH}-${VERSION}.dmg"

    stage_app_bundle "${app_bundle}"
    deploy_qt "${app_bundle}"
    sign_app_bundle "${app_bundle}"

    rm -rf "${dmg_root}"
    mkdir -p "${dmg_root}/bin"
    cp -R "${app_bundle}" "${dmg_root}/"

    cat > "${dmg_root}/bin/${CLI_COMMAND}" <<EOF
#!/usr/bin/env bash
set -e
ROOT="\$(cd -- "\$(dirname -- "\${BASH_SOURCE[0]}")/.." && pwd)"
exec "\${ROOT}/${APP_NAME}.app/Contents/Resources/bin/${CLI_COMMAND}" "\$@"
EOF
    chmod +x "${dmg_root}/bin/${CLI_COMMAND}"

    for file in README.md LICENSE THIRD_PARTY_NOTICES.md config.example.json; do
        if [[ -f "${PROJECT_DIR}/${file}" ]]; then
            cp -a "${PROJECT_DIR}/${file}" "${dmg_root}/${file}"
        fi
    done
    ln -s /Applications "${dmg_root}/Applications"

    rm -f "${out}"
    hdiutil create -volname "${APP_NAME}" -srcfolder "${dmg_root}" -ov -format UDZO "${out}"
    echo "Built dmg: ${out}"
}

require_tool cmake

if ((CLEAN == 1)); then
    rm -rf "${WORK_DIR}"
fi
mkdir -p "${DIST_DIR}" "${WORK_DIR}"

QT_ROOT_RESOLVED="$(detect_qt_root)"
if [[ ! -x "${QT_ROOT_RESOLVED}/bin/moc" ]]; then
    echo "Qt moc not found: ${QT_ROOT_RESOLVED}/bin/moc" >&2
    exit 2
fi

echo "Package: ${PACKAGE_ID}"
echo "Version: ${VERSION}"
echo "Arch: ${ARCH}"
echo "Qt root: ${QT_ROOT_RESOLVED}"
echo "Build dir: ${BUILD_DIR}"
echo "Dist dir: ${DIST_DIR}"

build_project
build_dmg
