#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
GUI_BIN="${GUI_BIN:-${PROJECT_DIR}/build/net-tunnel-gui}"
LOG_FILE="${GUI_LOG:-${PROJECT_DIR}/build/net-tunnel-gui.restart.log}"
PID_FILE="${GUI_PID_FILE:-${PROJECT_DIR}/build/net-tunnel-gui.pid}"
DESKTOP_ID="${DESKTOP_ID:-openai-reasoning-guard}"
APP_NAME="${APP_NAME:-OpenAI Reasoning Guard}"
ICON_SOURCE="${ICON_SOURCE:-${PROJECT_DIR}/assets/openai-reasoning-guard-icon-1024.png}"

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
    echo "Build it first with: cmake --build ${PROJECT_DIR}/build -j2" >&2
    exit 1
fi

install_dev_desktop_entry() {
    [[ -n "${HOME:-}" && -f "${ICON_SOURCE}" ]] || return 0

    local data_home="${XDG_DATA_HOME:-${HOME}/.local/share}"
    local icon_dir="${data_home}/icons/hicolor/1024x1024/apps"
    local app_dir="${data_home}/applications"
    mkdir -p "${icon_dir}" "${app_dir}"
    cp -f "${ICON_SOURCE}" "${icon_dir}/${DESKTOP_ID}.png"
    cat > "${app_dir}/${DESKTOP_ID}.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=${APP_NAME}
Comment=Local OpenAI-compatible reasoning guard proxy
Exec=${GUI_BIN}
Icon=${DESKTOP_ID}
Terminal=false
Categories=Network;Qt;
StartupNotify=true
StartupWMClass=openai-reasoning-guard-gui
EOF
}

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
