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
        echo -e "\n${GREEN}${BOLD}All steps completed successfully!${NC}"
        echo -e "Tip: keep typing and then run   ${CYAN}sudo ./run.sh stats${NC}   to see updated analytics.\n"
        ;;
    stats)
        require_root
        step_stats
        ;;
    unload)
        cmd_unload
        ;;
    udev_hotplug)
        cmd_udev_hotplug
        ;;
    *)
        echo "Usage: sudo $0 [full|stats|unload|udev_hotplug]"
        exit 1
        ;;
esac
