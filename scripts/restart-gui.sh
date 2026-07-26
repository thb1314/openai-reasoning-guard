#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
GUI_BIN="${GUI_BIN:-${PROJECT_DIR}/build-profile/net-tunnel-gui}"
LOG_FILE="${GUI_LOG:-${PROJECT_DIR}/build-profile/net-tunnel-gui.restart.log}"
PID_FILE="${GUI_PID_FILE:-${PROJECT_DIR}/build-profile/net-tunnel-gui.pid}"
DESKTOP_ID="${DESKTOP_ID:-openai-reasoning-guard}"
APP_WM_CLASS="${APP_WM_CLASS:-openai-reasoning-guard-gui}"
APP_NAME="${APP_NAME:-OpenAI Reasoning Guard}"
ICON_SOURCE="${ICON_SOURCE:-${PROJECT_DIR}/assets/openai-reasoning-guard-icon-1024.png}"
CONFIG_BASE="${XDG_CONFIG_HOME:-${HOME}/.config}"
CONFIG_DIR="${CONFIG_BASE}/OpenAI Reasoning Guard"
PREVIOUS_CONFIG_DIR="${CONFIG_BASE}/openai-reasoning-guard"
LEGACY_CONFIG_DIR="${CONFIG_BASE}/net-tunnel-cpp-client"

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    cat <<EOF
Usage: $(basename "$0")

Stops running OpenAI Reasoning Guard development GUI processes owned by the current user, then starts:
  ${GUI_BIN}

Environment overrides:
  GUI_BIN       Path to net-tunnel-gui binary
  GUI_LOG       Log file path
  GUI_PID_FILE  PID file path
EOF
    exit 0
fi

if [[ ! -x "${GUI_BIN}" ]]; then
    echo "GUI binary is not executable: ${GUI_BIN}" >&2
    echo "Build it first with: cmake --build ${PROJECT_DIR}/build-profile -j2" >&2
    exit 1
fi

install_dev_desktop_entry() {
    [[ -n "${HOME:-}" && -f "${ICON_SOURCE}" ]] || return 0

    local data_home="${XDG_DATA_HOME:-${HOME}/.local/share}"
    local app_dir="${data_home}/applications"
    local icon_root="${data_home}/icons/hicolor"
    mkdir -p "${app_dir}"
    install_dev_icon_sizes "${icon_root}" "${DESKTOP_ID}"
    write_dev_desktop_file "${app_dir}/${DESKTOP_ID}.desktop" "${DESKTOP_ID}"
    if [[ "${APP_WM_CLASS}" != "${DESKTOP_ID}" ]]; then
        remove_legacy_desktop_alias "${app_dir}/${APP_WM_CLASS}.desktop"
    fi
    gtk-update-icon-cache -q -t -f "${icon_root}" >/dev/null 2>&1 || true
    update-desktop-database "${app_dir}" >/dev/null 2>&1 || true
}

remove_legacy_desktop_alias() {
    local path="$1"
    [[ -f "${path}" ]] || return 0

    if grep -Fqx "Exec=${GUI_BIN}" "${path}" &&
       grep -Fqx "StartupWMClass=${APP_WM_CLASS}" "${path}"; then
        rm -f "${path}"
    fi
}

install_dev_icon_sizes() {
    local icon_root="$1"
    local icon_id="$2"
    local size
    for size in 16 22 24 32 48 64 128 256 512 1024; do
        local icon_dir="${icon_root}/${size}x${size}/apps"
        local sized_source="${PROJECT_DIR}/assets/openai-reasoning-guard-icon-${size}.png"
        mkdir -p "${icon_dir}"
        [[ -f "${sized_source}" ]] || sized_source="${ICON_SOURCE}"
        install -m 0644 "${sized_source}" "${icon_dir}/${icon_id}.png"
    done
}

prepare_default_config() {
    umask 077
    mkdir -p "${CONFIG_DIR}"
    chmod 0700 "${CONFIG_DIR}"
    if [[ ! -f "${CONFIG_DIR}/config.json" ]]; then
        local source_dir
        for source_dir in "${PREVIOUS_CONFIG_DIR}" "${LEGACY_CONFIG_DIR}"; do
            if [[ -f "${source_dir}/config.json" ]]; then
                cp "${source_dir}/config.json" "${CONFIG_DIR}/config.json"
                break
            fi
        done
    fi
    if [[ -f "${CONFIG_DIR}/config.json" ]]; then
        chmod 0600 "${CONFIG_DIR}/config.json"
    fi
    export NET_TUNNEL_CONFIG="${NET_TUNNEL_CONFIG:-${CONFIG_DIR}/config.json}"
}

write_dev_desktop_file() {
    local path="$1"
    local icon_id="$2"
    cat > "${path}" <<EOF
[Desktop Entry]
Type=Application
Name=${APP_NAME}
Comment=Local OpenAI-compatible reasoning guard proxy
Exec=${GUI_BIN}
Icon=${icon_id}
Terminal=false
Categories=Network;Qt;
StartupNotify=true
StartupWMClass=${APP_WM_CLASS}
EOF
}

prepare_default_config
install_dev_desktop_entry

readarray -t OLD_PIDS < <(pgrep -u "$(id -u)" -x net-tunnel-gui || true)

import_display_env() {
    local pid="$1"
    local env_file="/proc/${pid}/environ"
    [[ -r "${env_file}" ]] || return 0

    while IFS='=' read -r key value; do
        case "${key}" in
            DISPLAY|XAUTHORITY|WAYLAND_DISPLAY|XDG_RUNTIME_DIR|QT_QPA_PLATFORM)
                export "${key}=${value}"
                ;;
        esac
    done < <(tr '\0' '\n' < "${env_file}")
}

if ((${#OLD_PIDS[@]} > 0)); then
    import_display_env "${OLD_PIDS[0]}"
    echo "Stopping existing OpenAI Reasoning Guard GUI: ${OLD_PIDS[*]}"
    for pid in "${OLD_PIDS[@]}"; do
        kill -TERM "${pid}" 2>/dev/null || true
    done

    for _ in {1..25}; do
        still_running=0
        for pid in "${OLD_PIDS[@]}"; do
            if kill -0 "${pid}" 2>/dev/null; then
                still_running=1
                break
            fi
        done
        [[ "${still_running}" == "0" ]] && break
        sleep 0.2
    done

    for pid in "${OLD_PIDS[@]}"; do
        if kill -0 "${pid}" 2>/dev/null; then
            echo "Force stopping stuck OpenAI Reasoning Guard GUI: ${pid}"
            kill -KILL "${pid}" 2>/dev/null || true
        fi
    done
else
    echo "No existing OpenAI Reasoning Guard GUI process found."
fi

mkdir -p "$(dirname -- "${LOG_FILE}")" "$(dirname -- "${PID_FILE}")"
: > "${LOG_FILE}"

echo "Starting ${GUI_BIN}"
(
    cd "${PROJECT_DIR}"
    nohup "${GUI_BIN}" >> "${LOG_FILE}" 2>&1 < /dev/null &
    echo "$!" > "${PID_FILE}"
)

NEW_PID="$(cat "${PID_FILE}")"
sleep 0.5

if ! kill -0 "${NEW_PID}" 2>/dev/null; then
    echo "Failed to start OpenAI Reasoning Guard GUI. Log:" >&2
    sed -n '1,80p' "${LOG_FILE}" >&2 || true
    exit 1
fi

echo "Started OpenAI Reasoning Guard GUI pid=${NEW_PID}"
echo "Log: ${LOG_FILE}"
