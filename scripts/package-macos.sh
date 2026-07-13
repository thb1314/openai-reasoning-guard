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
MACOS_CODESIGN_TIMESTAMP="${MACOS_CODESIGN_TIMESTAMP-none}"
MACOS_CODESIGN_OPTIONS="${MACOS_CODESIGN_OPTIONS:-}"
MACOS_CODESIGN_ENTITLEMENTS="${MACOS_CODESIGN_ENTITLEMENTS:-}"
MACOS_NOTARIZE="${MACOS_NOTARIZE:-0}"
MACOS_NOTARY_PROFILE="${MACOS_NOTARY_PROFILE:-}"
MACOS_NOTARY_APPLE_ID="${MACOS_NOTARY_APPLE_ID:-}"
MACOS_NOTARY_TEAM_ID="${MACOS_NOTARY_TEAM_ID:-}"
MACOS_NOTARY_PASSWORD="${MACOS_NOTARY_PASSWORD:-}"
MACOS_STAPLE="${MACOS_STAPLE:-1}"
MACOS_DMG_STYLE="${MACOS_DMG_STYLE:-1}"
MACOS_DMG_STYLE_STRICT="${MACOS_DMG_STYLE_STRICT:-0}"
MACOS_DMG_BACKGROUND="${MACOS_DMG_BACKGROUND:-1}"
MACOS_KEEP_DMG="${MACOS_KEEP_DMG:-0}"
CLEAN=0

usage() {
    cat <<EOF
Usage: $(basename "$0") [--arch x86_64|aarch64] [--skip-build] [--clean]

Build a macOS sudo shell installer for ${PACKAGE_ID}. The installer embeds a
temporary dmg payload and installs the app into /Applications.

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
  MACOS_CODESIGN_OPTIONS=runtime
  MACOS_CODESIGN_ENTITLEMENTS=/path/to/entitlements.plist
  MACOS_NOTARIZE=1
  MACOS_NOTARY_PROFILE=notary-profile
  MACOS_NOTARY_APPLE_ID=apple@example.com
  MACOS_NOTARY_TEAM_ID=TEAMID
  MACOS_NOTARY_PASSWORD=app-specific-password
  MACOS_DMG_STYLE=1
  MACOS_DMG_STYLE_STRICT=0
  MACOS_DMG_BACKGROUND=1
  MACOS_KEEP_DMG=0                  # set to 1 to also copy the inner dmg to DIST_DIR
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
    if [[ -n "${MACOS_CODESIGN_OPTIONS}" ]]; then
        args+=(--options "${MACOS_CODESIGN_OPTIONS}")
    fi
    if [[ -n "${MACOS_CODESIGN_ENTITLEMENTS}" ]]; then
        args+=(--entitlements "${MACOS_CODESIGN_ENTITLEMENTS}")
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
    if [[ -n "${MACOS_CODESIGN_ENTITLEMENTS}" && ! -f "${MACOS_CODESIGN_ENTITLEMENTS}" ]]; then
        echo "macOS codesign entitlements file not found: ${MACOS_CODESIGN_ENTITLEMENTS}" >&2
        exit 2
    fi

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
    if [[ -n "${MACOS_CODESIGN_OPTIONS}" ]]; then
        bundle_args+=(--options "${MACOS_CODESIGN_OPTIONS}")
    fi
    if [[ -n "${MACOS_CODESIGN_ENTITLEMENTS}" ]]; then
        bundle_args+=(--entitlements "${MACOS_CODESIGN_ENTITLEMENTS}")
    fi
    codesign "${bundle_args[@]}" --deep "${app_bundle}"
    codesign --verify --deep --strict --verbose=2 "${app_bundle}"
}

write_first_run_helper() {
    local dmg_root="$1"
    local helper="${dmg_root}/OpenAI Reasoning Guard - First Run.command"
    cat > "${helper}" <<EOF
#!/usr/bin/env bash
set -euo pipefail
ROOT="\$(cd -- "\$(dirname -- "\${BASH_SOURCE[0]}")" && pwd)"
SOURCE_APP="\${ROOT}/${APP_NAME}.app"
TARGET_APP="/Applications/${APP_NAME}.app"

if [[ ! -d "\${SOURCE_APP}" ]]; then
  echo "Cannot find \${SOURCE_APP}" >&2
  exit 1
fi

if [[ ! -w "/Applications" ]]; then
  TARGET_APP="\${HOME}/Applications/${APP_NAME}.app"
  mkdir -p "\${HOME}/Applications"
fi

rm -rf "\${TARGET_APP}"
/usr/bin/ditto "\${SOURCE_APP}" "\${TARGET_APP}"
/usr/bin/xattr -dr com.apple.quarantine "\${TARGET_APP}" 2>/dev/null || true
/usr/bin/open "\${TARGET_APP}"
EOF
    chmod +x "${helper}"

    cat > "${dmg_root}/README-macOS-Install.txt" <<EOF
OpenAI Reasoning Guard macOS install

Preferred install:
1. Drag "${APP_NAME}.app" to Applications.
2. Open it from Applications.

If macOS says it was not opened because the developer cannot be verified:
1. Double-click "OpenAI Reasoning Guard - First Run.command" in this disk image.
2. The helper copies the app to Applications (or ~/Applications), removes the
   quarantine flag from that local copy, and opens it.

For builds signed with a Developer ID certificate and notarized by Apple, the
helper is not needed.
EOF
}

write_dmg_background() {
    local out_png="$1"
    if [[ "${MACOS_DMG_BACKGROUND}" != "1" ]]; then
        return
    fi
    if ! command -v swift >/dev/null 2>&1; then
        echo "swift not found; skipping dmg background image generation"
        return
    fi

    local swift_source="${WORK_DIR}/dmg-background.swift"
    cat > "${swift_source}" <<'SWIFT'
import AppKit
import Darwin
import Foundation

let outPath = ProcessInfo.processInfo.environment["DMG_BACKGROUND_PATH"] ?? ""
let appName = ProcessInfo.processInfo.environment["DMG_BACKGROUND_APP_NAME"] ?? "Application"
guard !outPath.isEmpty else {
    fputs("DMG_BACKGROUND_PATH is empty\n", stderr)
    exit(2)
}

let width = 660.0
let height = 430.0
let image = NSImage(size: NSSize(width: width, height: height))
image.lockFocus()

NSColor(calibratedRed: 0.96, green: 0.97, blue: 0.98, alpha: 1.0).setFill()
NSBezierPath(rect: NSRect(x: 0, y: 0, width: width, height: height)).fill()

NSColor(calibratedRed: 0.88, green: 0.90, blue: 0.93, alpha: 1.0).setStroke()
let arrow = NSBezierPath()
arrow.lineWidth = 5
arrow.move(to: NSPoint(x: 250, y: 240))
arrow.line(to: NSPoint(x: 410, y: 240))
arrow.line(to: NSPoint(x: 388, y: 260))
arrow.move(to: NSPoint(x: 410, y: 240))
arrow.line(to: NSPoint(x: 388, y: 220))
arrow.stroke()

let paragraph = NSMutableParagraphStyle()
paragraph.alignment = .center
let titleAttrs: [NSAttributedString.Key: Any] = [
    .font: NSFont.systemFont(ofSize: 22, weight: .semibold),
    .foregroundColor: NSColor(calibratedRed: 0.18, green: 0.20, blue: 0.23, alpha: 1.0),
    .paragraphStyle: paragraph
]
let hintAttrs: [NSAttributedString.Key: Any] = [
    .font: NSFont.systemFont(ofSize: 13, weight: .regular),
    .foregroundColor: NSColor(calibratedRed: 0.38, green: 0.41, blue: 0.45, alpha: 1.0),
    .paragraphStyle: paragraph
]

("Drag \(appName) to Applications" as NSString).draw(
    in: NSRect(x: 0, y: 292, width: width, height: 30),
    withAttributes: titleAttrs
)
("Unsigned builds can use First Run after copying." as NSString).draw(
    in: NSRect(x: 0, y: 36, width: width, height: 20),
    withAttributes: hintAttrs
)

image.unlockFocus()

guard let tiff = image.tiffRepresentation,
      let rep = NSBitmapImageRep(data: tiff),
      let png = rep.representation(using: .png, properties: [:]) else {
    fputs("failed to render dmg background\n", stderr)
    exit(2)
}

do {
    try png.write(to: URL(fileURLWithPath: outPath))
} catch {
    fputs("failed to write dmg background: \(error)\n", stderr)
    exit(2)
}
SWIFT

    DMG_BACKGROUND_PATH="${out_png}" \
    DMG_BACKGROUND_APP_NAME="${APP_NAME}" \
        swift "${swift_source}" >/dev/null
}

apple_script_escape() {
    printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'
}

style_dmg_volume() {
    local mount_dir="$1"
    if [[ "${MACOS_DMG_STYLE}" != "1" ]]; then
        return
    fi
    if ! command -v osascript >/dev/null 2>&1; then
        echo "osascript not found; skipping dmg Finder layout"
        return
    fi

    local mount_dir_escaped
    local app_item_escaped
    local background_path_escaped
    mount_dir_escaped="$(apple_script_escape "${mount_dir}")"
    app_item_escaped="$(apple_script_escape "${APP_NAME}.app")"
    background_path_escaped="$(apple_script_escape "${mount_dir}/.background/background.png")"

    local applescript="${WORK_DIR}/style-dmg.applescript"
    cat > "${applescript}" <<EOF
with timeout of 30 seconds
  tell application "Finder"
    set dmgFolder to POSIX file "${mount_dir_escaped}" as alias
    open dmgFolder
    delay 1

    set dmgWindow to container window of dmgFolder
    set current view of dmgWindow to icon view
    set toolbar visible of dmgWindow to false
    set statusbar visible of dmgWindow to false
    set the bounds of dmgWindow to {120, 120, 780, 550}

    set viewOptions to the icon view options of dmgWindow
    set arrangement of viewOptions to not arranged
    set icon size of viewOptions to 96
    try
      set background picture of viewOptions to POSIX file "${background_path_escaped}"
    end try

    if exists item "${app_item_escaped}" of dmgWindow then
      set position of item "${app_item_escaped}" of dmgWindow to {170, 190}
    end if
    if exists item "Applications" of dmgWindow then
      set position of item "Applications" of dmgWindow to {490, 190}
    end if
    if exists item "OpenAI Reasoning Guard - First Run.command" of dmgWindow then
      set position of item "OpenAI Reasoning Guard - First Run.command" of dmgWindow to {170, 360}
    end if
    if exists item "README-macOS-Install.txt" of dmgWindow then
      set position of item "README-macOS-Install.txt" of dmgWindow to {490, 360}
    end if
    if exists item "bin" of dmgWindow then
      set position of item "bin" of dmgWindow to {80, 360}
    end if

    delay 1
    close dmgWindow
  end tell
end timeout
EOF

    if ! osascript "${applescript}"; then
        if [[ "${MACOS_DMG_STYLE_STRICT}" == "1" ]]; then
            echo "failed to apply dmg Finder layout" >&2
            return 2
        fi
        echo "failed to apply dmg Finder layout; continuing because MACOS_DMG_STYLE_STRICT=0" >&2
    fi
}

detach_dmg_volume() {
    local detach_target="$1"
    local mount_dir="$2"
    local attempt

    for attempt in 1 2 3; do
        if hdiutil detach "${detach_target}"; then
            break
        fi
        sleep "${attempt}"
    done

    if [[ -d "${mount_dir}" ]]; then
        for attempt in 1 2 3; do
            if hdiutil detach "${detach_target}" -force || hdiutil detach "${mount_dir}" -force; then
                break
            fi
            sleep "${attempt}"
        done
    fi

    for attempt in 1 2 3 4 5; do
        if [[ ! -d "${mount_dir}" ]]; then
            return 0
        fi
        sleep 1
    done

    echo "unable to detach dmg volume: ${detach_target} (${mount_dir})" >&2
    return 2
}

convert_dmg_with_retry() {
    local source="$1"
    local out="$2"
    local attempt

    for attempt in 1 2 3 4 5; do
        rm -f "${out}"
        if hdiutil convert "${source}" -format UDZO -imagekey zlib-level=9 -ov -o "${out}"; then
            return 0
        fi
        echo "dmg conversion attempt ${attempt} failed; retrying" >&2
        sleep "$((attempt * 2))"
    done

    echo "unable to convert dmg after 5 attempts: ${source}" >&2
    return 2
}

create_drag_install_dmg() {
    local dmg_root="$1"
    local out="$2"
    local rw_dmg="${WORK_DIR}/${PACKAGE_ID}-macos-${ARCH}-${VERSION}.rw.dmg"
    local attach_output
    local mount_dir
    local device
    local detach_target
    local style_status=0

    rm -f "${rw_dmg}" "${out}"
    hdiutil create \
        -volname "${APP_NAME}" \
        -srcfolder "${dmg_root}" \
        -ov \
        -format UDRW \
        -fs HFS+ \
        "${rw_dmg}"

    attach_output="$(hdiutil attach -readwrite -noverify -noautoopen "${rw_dmg}")"
    device="$(printf '%s\n' "${attach_output}" | awk '/\/Volumes\// {print $1; exit}')"
    mount_dir="$(printf '%s\n' "${attach_output}" | sed -n 's#^/dev/[^[:space:]]*[[:space:]].*\(/Volumes/.*\)$#\1#p' | tail -n 1)"
    if [[ -z "${mount_dir}" || ! -d "${mount_dir}" ]]; then
        mount_dir="/Volumes/${APP_NAME}"
    fi
    if [[ ! -d "${mount_dir}" ]]; then
        echo "unable to find mounted dmg volume" >&2
        printf '%s\n' "${attach_output}" >&2
        exit 2
    fi

    style_dmg_volume "${mount_dir}" || style_status=$?
    sync
    detach_target="${device:-${mount_dir}}"
    detach_dmg_volume "${detach_target}" "${mount_dir}"
    if ((style_status != 0)); then
        exit "${style_status}"
    fi

    convert_dmg_with_retry "${rw_dmg}" "${out}"
    rm -f "${rw_dmg}"
}

notarize_dmg() {
    local dmg_path="$1"
    if [[ "${MACOS_NOTARIZE}" != "1" ]]; then
        echo "Skipping macOS notarization because MACOS_NOTARIZE=${MACOS_NOTARIZE}"
        return
    fi
    if [[ "${MACOS_CODESIGN_IDENTITY}" == "-" ]]; then
        echo "MACOS_NOTARIZE=1 requires a Developer ID Application signing identity" >&2
        exit 2
    fi
    require_tool xcrun

    local submit_args=(notarytool submit "${dmg_path}" --wait)
    if [[ -n "${MACOS_NOTARY_PROFILE}" ]]; then
        submit_args+=(--keychain-profile "${MACOS_NOTARY_PROFILE}")
    else
        if [[ -z "${MACOS_NOTARY_APPLE_ID}" || -z "${MACOS_NOTARY_TEAM_ID}" || -z "${MACOS_NOTARY_PASSWORD}" ]]; then
            echo "MACOS_NOTARIZE=1 requires MACOS_NOTARY_PROFILE or Apple ID/team/password variables" >&2
            exit 2
        fi
        submit_args+=(--apple-id "${MACOS_NOTARY_APPLE_ID}" --team-id "${MACOS_NOTARY_TEAM_ID}" --password "${MACOS_NOTARY_PASSWORD}")
    fi

    xcrun "${submit_args[@]}"
    if [[ "${MACOS_STAPLE}" == "1" ]]; then
        xcrun stapler staple "${dmg_path}"
        xcrun stapler validate "${dmg_path}"
    fi
}

write_shell_installer() {
    local dmg_path="$1"
    local out="$2"
    local payload_name="${PACKAGE_ID}-macos-${ARCH}-${VERSION}.dmg"

    require_tool base64
    rm -f "${out}"
    {
        printf '%s\n' '#!/usr/bin/env bash'
        printf '%s\n' 'set -euo pipefail'
        printf 'PACKAGE_ID=%q\n' "${PACKAGE_ID}"
        printf 'APP_NAME=%q\n' "${APP_NAME}"
        printf 'CLI_COMMAND=%q\n' "${CLI_COMMAND}"
        printf 'PAYLOAD_NAME=%q\n' "${payload_name}"
        cat <<'INSTALLER'

die() {
    echo "error: $*" >&2
    exit 1
}

require_tool() {
    command -v "$1" >/dev/null 2>&1 || die "required tool not found: $1"
}

decode_payload() {
    local payload_line="$1"
    local out_dmg="$2"
    if tail -n +"${payload_line}" "$0" | base64 -D > "${out_dmg}" 2>/dev/null; then
        return 0
    fi
    if tail -n +"${payload_line}" "$0" | base64 -d > "${out_dmg}" 2>/dev/null; then
        return 0
    fi
    if tail -n +"${payload_line}" "$0" | base64 --decode > "${out_dmg}" 2>/dev/null; then
        return 0
    fi
    return 1
}

if [[ "$(uname -s)" != "Darwin" ]]; then
    die "this installer must be run on macOS"
fi

require_tool awk
require_tool base64
require_tool hdiutil
require_tool tail

if [[ "${EUID}" -eq 0 ]]; then
    run_privileged() {
        "$@"
    }
else
    require_tool sudo
    echo "This installer needs sudo permission to install ${APP_NAME} into /Applications."
    sudo -v
    run_privileged() {
        sudo "$@"
    }
fi

TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/${PACKAGE_ID}.install.XXXXXX")"
DMG_PATH="${TMP_DIR}/${PAYLOAD_NAME}"
MOUNT_DIR=""
DETACH_TARGET=""

cleanup() {
    if [[ -n "${DETACH_TARGET}" ]]; then
        hdiutil detach "${DETACH_TARGET}" >/dev/null 2>&1 || true
    elif [[ -n "${MOUNT_DIR}" && -d "${MOUNT_DIR}" ]]; then
        hdiutil detach "${MOUNT_DIR}" >/dev/null 2>&1 || true
    fi
    rm -rf "${TMP_DIR}"
}
trap cleanup EXIT

PAYLOAD_LINE="$(awk 'BEGIN { line = 0 } /^__DMG_PAYLOAD_BELOW__$/ { line = NR + 1; exit } END { if (line > 0) print line; else exit 1 }' "$0")" || die "embedded dmg payload marker missing"
decode_payload "${PAYLOAD_LINE}" "${DMG_PATH}" || die "failed to decode embedded dmg payload"

ATTACH_OUTPUT="$(hdiutil attach -nobrowse -noverify "${DMG_PATH}")"
DETACH_TARGET="$(printf '%s\n' "${ATTACH_OUTPUT}" | awk '/\/Volumes\// { print $1; exit }')"
MOUNT_DIR="$(printf '%s\n' "${ATTACH_OUTPUT}" | awk '/\/Volumes\// { for (i = 1; i <= NF; i++) if ($i ~ /^\/Volumes\//) { print substr($0, index($0, $i)); exit } }')"
if [[ -z "${MOUNT_DIR}" || ! -d "${MOUNT_DIR}" ]]; then
    MOUNT_DIR="/Volumes/${APP_NAME}"
fi
[[ -d "${MOUNT_DIR}" ]] || die "unable to find mounted dmg volume"

SOURCE_APP="${MOUNT_DIR}/${APP_NAME}.app"
if [[ ! -d "${SOURCE_APP}" ]]; then
    SOURCE_APP=""
    for candidate in "${MOUNT_DIR}"/*.app; do
        if [[ -d "${candidate}" ]]; then
            SOURCE_APP="${candidate}"
            break
        fi
    done
fi
[[ -d "${SOURCE_APP}" ]] || die "unable to find app bundle in mounted dmg"

TARGET_APP="/Applications/${APP_NAME}.app"
echo "Installing ${APP_NAME} to ${TARGET_APP}"
run_privileged /bin/mkdir -p /Applications
run_privileged /bin/rm -rf "${TARGET_APP}"
run_privileged /usr/bin/ditto "${SOURCE_APP}" "${TARGET_APP}"
run_privileged /usr/bin/xattr -dr com.apple.quarantine "${TARGET_APP}" 2>/dev/null || true
run_privileged /usr/sbin/chown -R root:wheel "${TARGET_APP}" 2>/dev/null || true

CLI_SOURCE="${TARGET_APP}/Contents/Resources/bin/${CLI_COMMAND}"
if [[ "${INSTALL_CLI_SYMLINK:-1}" == "1" && -x "${CLI_SOURCE}" ]]; then
    WRAPPER="${TMP_DIR}/${CLI_COMMAND}"
    cat > "${WRAPPER}" <<WRAPPER_EOF
#!/usr/bin/env bash
exec "${CLI_SOURCE}" "\$@"
WRAPPER_EOF
    run_privileged /bin/mkdir -p /usr/local/bin
    run_privileged /usr/bin/install -m 0755 "${WRAPPER}" "/usr/local/bin/${CLI_COMMAND}"
    echo "Installed CLI wrapper: /usr/local/bin/${CLI_COMMAND}"
fi

echo "Installed ${APP_NAME}."
if [[ "${OPEN_AFTER_INSTALL:-1}" == "1" ]]; then
    /usr/bin/open "${TARGET_APP}" >/dev/null 2>&1 || true
fi
exit 0

__DMG_PAYLOAD_BELOW__
INSTALLER
        base64 < "${dmg_path}"
    } > "${out}"
    chmod +x "${out}"
    echo "Built macOS shell installer: ${out}"
}

build_dmg() {
    require_tool hdiutil
    local app_bundle="${WORK_DIR}/${APP_NAME}.app"
    local dmg_root="${WORK_DIR}/dmgroot"
    local dmg_out="${WORK_DIR}/${PACKAGE_ID}-macos-${ARCH}-${VERSION}.dmg"
    local dmg_dist="${DIST_DIR}/${PACKAGE_ID}-macos-${ARCH}-${VERSION}.dmg"
    local installer_out="${DIST_DIR}/${PACKAGE_ID}-macos-${ARCH}-${VERSION}-installer.sh"

    stage_app_bundle "${app_bundle}"
    deploy_qt "${app_bundle}"
    sign_app_bundle "${app_bundle}"

    rm -rf "${dmg_root}"
    mkdir -p "${dmg_root}/bin" "${dmg_root}/.background"
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
    write_first_run_helper "${dmg_root}"
    write_dmg_background "${dmg_root}/.background/background.png"
    touch "${dmg_root}/.metadata_never_index"
    ln -s /Applications "${dmg_root}/Applications"

    rm -f "${dmg_out}" "${dmg_dist}" "${installer_out}"
    create_drag_install_dmg "${dmg_root}" "${dmg_out}"
    notarize_dmg "${dmg_out}"
    write_shell_installer "${dmg_out}" "${installer_out}"
    if [[ "${MACOS_KEEP_DMG}" == "1" ]]; then
        cp -a "${dmg_out}" "${dmg_dist}"
        echo "Built macOS dmg: ${dmg_dist}"
    fi
}

if [[ "${SKIP_BUILD}" != "1" ]]; then
    require_tool cmake
fi

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
