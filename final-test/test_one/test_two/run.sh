# 1. Clean & Rebuild Stack (from phase1/)
docker compose down --remove-orphans --volumes
docker compose up -d --build

# 2. Recompile Daemon (from phase1/phase2_3/)
make clean && make

# 3. Resolve cgroup path & Launch Daemon
CGROUP_PATH="/sys/fs/cgroup$(docker inspect -f '{{.State.Pid}}' app-service | xargs -I{} awk -F: '{print $3}' /proc/{}/cgroup)/cgroup.procs"
sudo ./bin/perf_daemon "$CGROUP_PATH"
