#!/usr/bin/env bash
set -euo pipefail

echo "[PHASE1] 🔒 Locking CPU governor & disabling Turbo Boost..."
[[ $EUID -ne 0 ]] && { echo "⚠️  Run with sudo"; exit 1; }

# Disable Turbo Boost (Intel)
if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
    echo "[PHASE1] ✅ Turbo Boost: DISABLED"
else
    echo "[PHASE1] ⚠️  Intel pstate not found. Turbo control skipped."
fi

# Force performance governor
for cpu in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$cpu" 2>/dev/null || true
done
echo "[PHASE1] ✅ CPU Governor: locked to performance"

# Validate fixed frequencies
echo "[PHASE1] 📊 Current CPU Frequencies (first 4 cores):"
grep "cpu MHz" /proc/cpuinfo | head -n 4

# Verify cgroup v2
if stat -fc %T /sys/fs/cgroup/ | grep -q "cgroup2"; then
    echo "[PHASE1] ✅ cgroup v2: ENABLED"
else
    echo "[PHASE1] ❌ cgroup v2 not active. Add systemd.unified_cgroup_hierarchy=1 to GRUB."
    exit 1
fi
