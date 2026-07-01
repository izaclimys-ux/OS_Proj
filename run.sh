#!/usr/bin/env bash
# ─────────────────────────────────────────────────────────────────────────────
#  run.sh — KB Analytics: full automation script
#
#  Usage:
#    sudo ./run.sh              → full setup (build + load + demo)
#    sudo ./run.sh stats        → print /proc/kb_stats (module must be loaded)
#    sudo ./run.sh unload       → remove module and clean up
#    sudo ./run.sh udev_hotplug → called automatically by udev on USB plug
#
#  What it does (full setup):
#    1. Build the kernel module with make
#    2. Build the user-space app with gcc (via make)
#    3. Insert the kernel module (insmod)
#    4. Run the user-space app (write + read hello-world exchange)
#    5. Show recent kernel log (dmesg)
#    6. Print keyboard analytics from /proc/kb_stats
# ─────────────────────────────────────────────────────────────────────────────

set -euo pipefail

# ── Configurable paths ───────────────────────────────────────────────────────
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MODULE_NAME="kb_module"
MODULE_FILE="${SCRIPT_DIR}/${MODULE_NAME}.ko"
USERSPACE_BIN="${SCRIPT_DIR}/userspace_app"
PROC_STATS="/proc/kb_stats"
DEVICE_NODE="/dev/kb_analytics"

# ── Colour helpers ───────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'
CYAN='\033[0;36m'; BOLD='\033[1m'; NC='\033[0m'

info()    { echo -e "${CYAN}[INFO]${NC}  $*"; }
success() { echo -e "${GREEN}[OK]${NC}    $*"; }
warn()    { echo -e "${YELLOW}[WARN]${NC}  $*"; }
error()   { echo -e "${RED}[ERROR]${NC} $*" >&2; }
section() { echo -e "\n${BOLD}══════════════════════════════════════${NC}"; \
            echo -e "${BOLD} $*${NC}"; \
            echo -e "${BOLD}══════════════════════════════════════${NC}"; }

# ── Root check ───────────────────────────────────────────────────────────────
require_root() {
    if [[ $EUID -ne 0 ]]; then
        error "This script must be run as root (use sudo)."
        exit 1
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
#  STEP 1: Build everything with make
# ─────────────────────────────────────────────────────────────────────────────
step_build() {
    section "Step 1: Build kernel module + user-space app"
    cd "${SCRIPT_DIR}"

    info "Running: make"
    if make; then
        success "Build complete."
    else
        error "Build failed. Check compiler/kernel-headers output above."
        exit 1
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
#  STEP 2: Insert the kernel module
# ─────────────────────────────────────────────────────────────────────────────
step_insert_module() {
    section "Step 2: Insert kernel module"

    # Remove old instance if already loaded (graceful reload)
    if lsmod | grep -q "^${MODULE_NAME}"; then
        warn "Module already loaded — removing old instance first."
        rmmod "${MODULE_NAME}" || { error "rmmod failed"; exit 1; }
    fi

    if [[ ! -f "${MODULE_FILE}" ]]; then
        error "Module file not found: ${MODULE_FILE}"
        exit 1
    fi

    info "Running: insmod ${MODULE_FILE}"
    if insmod "${MODULE_FILE}"; then
        success "Module '${MODULE_NAME}' inserted into the kernel."
    else
        error "insmod failed. Check dmesg for details."
        exit 1
    fi

    # Verify device node appeared
    local retries=5
    while [[ ! -e "${DEVICE_NODE}" && retries -gt 0 ]]; do
        sleep 0.2; ((retries--))
    done
    if [[ -e "${DEVICE_NODE}" ]]; then
        success "Device node ${DEVICE_NODE} is ready."
    else
        warn "Device node ${DEVICE_NODE} not yet visible (udev may be slow)."
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
#  STEP 3: Run the user-space application
# ─────────────────────────────────────────────────────────────────────────────
step_run_userspace() {
    section "Step 3: Run user-space application (write/read hello-world)"

    if [[ ! -x "${USERSPACE_BIN}" ]]; then
        error "User-space binary not found or not executable: ${USERSPACE_BIN}"
        exit 1
    fi

    info "Running: ${USERSPACE_BIN}"
    echo "────────────────────────────────────"
    "${USERSPACE_BIN}"
    echo "────────────────────────────────────"
    success "User-space app finished."
}

# ─────────────────────────────────────────────────────────────────────────────
#  STEP 4: Show kernel log with dmesg
# ─────────────────────────────────────────────────────────────────────────────
step_dmesg() {
    section "Step 4: Recent kernel log (dmesg | grep kb_analytics)"
    echo "────────────────────────────────────"
    dmesg | grep -i "kb_analytics" | tail -30 || true
    echo "────────────────────────────────────"
    info "Full log: run   dmesg | tail -50"
}

# ─────────────────────────────────────────────────────────────────────────────
#  STEP 5: Show keyboard analytics from /proc
# ─────────────────────────────────────────────────────────────────────────────
step_stats() {
    section "Step 5: Keyboard Analytics (/proc/kb_stats)"
    if [[ -r "${PROC_STATS}" ]]; then
        cat "${PROC_STATS}"
        success "Stats displayed."
    else
        warn "${PROC_STATS} not found — type some keys first, then re-run:  sudo ./run.sh stats"
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
#  Unload subcommand
# ─────────────────────────────────────────────────────────────────────────────
cmd_unload() {
    require_root
    section "Unloading kernel module"
    if lsmod | grep -q "^${MODULE_NAME}"; then
        rmmod "${MODULE_NAME}" && success "Module removed." || error "rmmod failed."
    else
        warn "Module '${MODULE_NAME}' is not currently loaded."
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
#  udev hotplug subcommand (triggered by 99-kb-analytics.rules)
# ─────────────────────────────────────────────────────────────────────────────
cmd_udev_hotplug() {
    # udevd runs in a minimal environment — log to kernel ring buffer
    logger -t kb_analytics "USB hotplug event — ACTION=${ACTION:-unknown} DEVTYPE=${DEVTYPE:-unknown}"
    # If module is not yet loaded, insert it
    if ! lsmod | grep -q "^${MODULE_NAME}"; then
        if [[ -f "${MODULE_FILE}" ]]; then
            insmod "${MODULE_FILE}" && logger -t kb_analytics "Module inserted on hotplug." \
                                    || logger -t kb_analytics "insmod failed on hotplug."
        fi
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
#  Main dispatcher
# ─────────────────────────────────────────────────────────────────────────────
# ─────────────────────────────────────────────────────────────────────────────
#  STEP 6: Generate TLS certificates (if not already present)
# ─────────────────────────────────────────────────────────────────────────────
step_gen_certs() {
    section "Step 6: TLS Certificate Generation"

    if [[ -f "${SCRIPT_DIR}/certs/server.crt" && -f "${SCRIPT_DIR}/certs/server.key" ]]; then
        success "TLS certificates already exist — skipping generation."
        return
    fi

    if ! command -v openssl &>/dev/null; then
        warn "openssl not found. Installing..."
        apt-get install -y openssl
    fi

    info "Running: gen_certs.sh"
    bash "${SCRIPT_DIR}/gen_certs.sh"
    success "TLS certificates generated."
}

# ─────────────────────────────────────────────────────────────────────────────
#  STEP 7: Launch the TLS dashboard server
# ─────────────────────────────────────────────────────────────────────────────
step_launch_dashboard() {
    section "Step 7: Launch TLS Dashboard Server"

    # Kill any old instance
    pkill -f "dashboard_server.py" 2>/dev/null || true
    sleep 0.5

    if ! command -v python3 &>/dev/null; then
        warn "python3 not found. Installing..."
        apt-get install -y python3
    fi

    PI_IP=$(hostname -I 2>/dev/null | awk '{print $1}' || echo "localhost")

    info "Starting dashboard_server.py in the background..."
    nohup python3 "${SCRIPT_DIR}/dashboard_server.py" \
        > "${SCRIPT_DIR}/dashboard.log" 2>&1 &
    DASH_PID=$!
    echo "${DASH_PID}" > "${SCRIPT_DIR}/dashboard.pid"

    sleep 1   # give it a moment to bind

    if kill -0 "${DASH_PID}" 2>/dev/null; then
        success "Dashboard running (PID ${DASH_PID})"
        echo ""
        echo -e "  ${BOLD}Open in your laptop browser:${NC}"
        echo -e "  ${GREEN}https://${PI_IP}:5000${NC}"
        echo ""
        echo -e "  ${YELLOW}Note:${NC} Accept the self-signed certificate warning once."
        echo -e "  Log: ${SCRIPT_DIR}/dashboard.log"
    else
        error "Dashboard failed to start. Check ${SCRIPT_DIR}/dashboard.log"
        cat "${SCRIPT_DIR}/dashboard.log" 2>/dev/null || true
    fi
}

# ─────────────────────────────────────────────────────────────────────────────
#  Stop dashboard subcommand
# ─────────────────────────────────────────────────────────────────────────────
cmd_stop_dashboard() {
    section "Stopping Dashboard"
    if pkill -f "dashboard_server.py" 2>/dev/null; then
        success "Dashboard stopped."
    else
        warn "Dashboard was not running."
    fi
    rm -f "${SCRIPT_DIR}/dashboard.pid"
}

SUBCOMMAND="${1:-full}"

case "${SUBCOMMAND}" in
    full)
        require_root
        echo -e "\n${BOLD}╔══════════════════════════════════════════╗${NC}"
        echo -e "${BOLD}║     KB Analytics — Full Setup Script     ║${NC}"
        echo -e "${BOLD}╚══════════════════════════════════════════╝${NC}"
        step_build
        step_insert_module
        step_run_userspace
        step_dmesg
        step_stats
        step_gen_certs
        step_launch_dashboard
        echo -e "\n${GREEN}${BOLD}All steps completed successfully!${NC}"
        echo -e "Tip: run   ${CYAN}sudo ./run.sh stats${NC}   to see analytics."
        echo -e "     run   ${CYAN}sudo ./run.sh stop-dashboard${NC}   to stop the server.\n"
        ;;
    stats)
        require_root
        step_stats
        ;;
    dashboard)
        require_root
        step_gen_certs
        step_launch_dashboard
        ;;
    stop-dashboard)
        require_root
        cmd_stop_dashboard
        ;;
    unload)
        cmd_unload
        ;;
    udev_hotplug)
        cmd_udev_hotplug
        ;;
    *)
        echo "Usage: sudo $0 [full|stats|dashboard|stop-dashboard|unload|udev_hotplug]"
        exit 1
        ;;
esac
