# 1. Clean old state to avoid conflicts
docker compose down --remove-orphans --volumes
docker network prune -f

# 2. Start the unified stack
docker compose up -d --build

# 3. Verify all 5 containers are healthy
docker ps --format "table {{.Names}}\t{{.Status}}"

# 4. Launch the daemon (still from phase1/phase2_3/)
cd phase2_3
CGROUP_PATH="/sys/fs/cgroup$(docker inspect -f '{{.State.Pid}}' app-service | xargs -I{} awk -F: '{print $3}' /proc/{}/cgroup)/cgroup.procs"
sudo ./bin/perf_daemon "$CGROUP_PATH"
