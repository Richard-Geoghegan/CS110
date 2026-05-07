#!/usr/bin/env bash
set -euo pipefail

echo "[PHASE1] Locking CPU governor & disabling Turbo Boost..."
[[ $EUID -ne 0 ]] && { echo "Run with sudo"; exit 1; }

# --- Disable frequency boost (Intel + AMD) ---
if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
    echo "[PHASE1] Turbo Boost (Intel): DISABLED"
elif [ -f /sys/devices/system/cpu/cpufreq/boost ]; then
    echo 0 > /sys/devices/system/cpu/cpufreq/boost
    echo "[PHASE1] Boost (AMD): DISABLED"
else
    echo "[PHASE1] No boost control found; skipping."
fi

# --- Force performance governor ---
# Try cpupower first (more reliable on CloudLab), fall back to sysfs
if command -v cpupower &>/dev/null; then
    cpupower frequency-set -g performance
    echo "[PHASE1] CPU Governor (cpupower): performance"
else
    for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
        echo performance > "$cpu" 2>/dev/null || true
    done
    echo "[PHASE1] CPU Governor (sysfs): performance"
fi

# --- Allow non-root perf (CloudLab needs this) ---
echo 1 > /proc/sys/kernel/perf_event_paranoid
echo "[PHASE1] perf_event_paranoid: 1"

# --- Disable ASLR for reproducible addresses ---
echo 0 > /proc/sys/kernel/randomize_va_space
echo "[PHASE1] ASLR: DISABLED"

# --- Validate ---
echo "[PHASE1] Current CPU Frequencies (first 4 cores):"
grep "cpu MHz" /proc/cpuinfo | head -n 4

# --- cgroup v2 check ---
if stat -fc %T /sys/fs/cgroup/ | grep -q "cgroup2"; then
    echo "[PHASE1] cgroup v2: ENABLED"
else
    echo "[PHASE1] ERROR: cgroup v2 not active. Add systemd.unified_cgroup_hierarchy=1 to GRUB_CMDLINE_LINUX in /etc/default/grub, then: sudo update-grub && sudo reboot"
    exit 1
fi
