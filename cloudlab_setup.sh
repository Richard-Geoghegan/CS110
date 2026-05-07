#!/usr/bin/env bash
# cloudlab_setup.sh — One-shot provisioning for CloudLab bare-metal nodes (Ubuntu 20.04/22.04)
# Run as: sudo bash cloudlab_setup.sh
set -euo pipefail

[[ $EUID -ne 0 ]] && { echo "Run as root (sudo bash $0)"; exit 1; }

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
EXPERIMENT_DIR="$REPO_DIR/final-test/test_one"

echo "=== [1/7] System packages ==="
apt-get update -qq
apt-get install -y --no-install-recommends \
    build-essential \
    libcurl4-openssl-dev \
    linux-tools-common \
    linux-tools-generic \
    linux-tools-"$(uname -r)" \
    cpufrequtils \
    linux-cpupower \
    python3-pip \
    python3-venv \
    ca-certificates \
    curl \
    gnupg \
    lsb-release \
    jq

# pip packages for plotting
pip3 install --quiet matplotlib requests pandas

echo "=== [2/7] Docker ==="
if ! command -v docker &>/dev/null; then
    install -m 0755 -d /etc/apt/keyrings
    curl -fsSL https://download.docker.com/linux/ubuntu/gpg \
        | gpg --dearmor -o /etc/apt/keyrings/docker.gpg
    chmod a+r /etc/apt/keyrings/docker.gpg
    echo \
        "deb [arch=$(dpkg --print-architecture) signed-by=/etc/apt/keyrings/docker.gpg] \
        https://download.docker.com/linux/ubuntu $(lsb_release -cs) stable" \
        > /etc/apt/sources.list.d/docker.list
    apt-get update -qq
    apt-get install -y docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
    systemctl enable --now docker
    echo "Docker installed."
else
    echo "Docker already present."
fi

# Allow the current non-root user to use docker
SUDO_USER="${SUDO_USER:-$USER}"
usermod -aG docker "$SUDO_USER" 2>/dev/null || true

echo "=== [3/7] CPU frequency scaling ==="
# Disable boost
if [ -f /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
    echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
    echo "Intel Turbo Boost: DISABLED"
elif [ -f /sys/devices/system/cpu/cpufreq/boost ]; then
    echo 0 > /sys/devices/system/cpu/cpufreq/boost
    echo "AMD Boost: DISABLED"
fi

# Lock to performance governor
cpupower frequency-set -g performance 2>/dev/null \
    || for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
           echo performance > "$g" 2>/dev/null || true
       done
echo "Governor: performance"

# Persist across reboots via rc.local
cat > /etc/rc.local <<'RCEOF'
#!/bin/sh -e
[ -f /sys/devices/system/cpu/intel_pstate/no_turbo ] && echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
[ -f /sys/devices/system/cpu/cpufreq/boost ] && echo 0 > /sys/devices/system/cpu/cpufreq/boost
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    echo performance > "$g" 2>/dev/null || true
done
echo 1 > /proc/sys/kernel/perf_event_paranoid
echo 0 > /proc/sys/kernel/randomize_va_space
exit 0
RCEOF
chmod +x /etc/rc.local
systemctl enable rc-local 2>/dev/null || true

echo "=== [4/7] perf permissions ==="
echo 1 > /proc/sys/kernel/perf_event_paranoid
echo "kernel.perf_event_paranoid = 1" >> /etc/sysctl.d/99-perf.conf
echo 0 > /proc/sys/kernel/randomize_va_space
echo "kernel.randomize_va_space = 0" >> /etc/sysctl.d/99-perf.conf

echo "=== [5/7] cgroup v2 check ==="
if ! stat -fc %T /sys/fs/cgroup/ | grep -q "cgroup2"; then
    echo "WARNING: cgroup v2 not active."
    echo "To enable: edit /etc/default/grub and add to GRUB_CMDLINE_LINUX:"
    echo "  systemd.unified_cgroup_hierarchy=1"
    echo "Then: sudo update-grub && sudo reboot"
    echo ""
    echo "Continuing — Docker will still work but perf cgroup scoping may fail."
fi

echo "=== [6/7] Build perf_daemon ==="
cd "$EXPERIMENT_DIR"
make clean && make
# Grant capability so daemon can run without full root (optional hardening)
setcap cap_perfmon,cap_sys_admin+ep perf_daemon 2>/dev/null \
    || setcap cap_sys_admin+ep perf_daemon 2>/dev/null \
    || echo "setcap failed — daemon must run as root"

echo "=== [7/7] Validation ==="
echo ""
echo "CPU info:"
grep "model name" /proc/cpuinfo | head -1
echo ""
echo "Governor (first 4 cores):"
for g in /sys/devices/system/cpu/cpu{0..3}/cpufreq/scaling_governor 2>/dev/null; do
    [ -f "$g" ] && echo "  $g: $(cat $g)"
done
echo ""
echo "Current MHz (first 4 cores):"
grep "cpu MHz" /proc/cpuinfo | head -4
echo ""
echo "perf_event_paranoid: $(cat /proc/sys/kernel/perf_event_paranoid)"
echo "cgroup type: $(stat -fc %T /sys/fs/cgroup/)"
echo ""
echo "=== Setup complete ==="
echo ""
echo "Next steps:"
echo "  1. Log out and back in (for docker group)"
echo "  2. cd $EXPERIMENT_DIR"
echo "  3. bash run_experiment.sh"
