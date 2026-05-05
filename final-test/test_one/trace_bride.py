"""trace_bridge.py - Python context bridge for W3C trace IDs"""
import os
import socket
import threading
import time
from collections import defaultdict

SOCKET_PATH = "/tmp/trace_bridge.sock"
ACTIVE_TRACES = defaultdict(lambda: {"trace_id": "", "span_id": ""})

def start_bridge():
    if os.path.exists(SOCKET_PATH): os.unlink(SOCKET_PATH)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.bind(SOCKET_PATH)
    sock.listen(5)
    print(f"[BRIDGE] Listening on {SOCKET_PATH}")

    def handle_client(conn):
        try:
            tid = int(conn.recv(64).decode().strip())
            ctx = ACTIVE_TRACES.get(tid, {"trace_id": "00000000000000000000000000000000", "span_id": "0000000000000000"})
            conn.sendall(f"{ctx['trace_id']} {ctx['span_id']}\n".encode())
        except Exception as e:
            pass
        finally:
            conn.close()

    while True:
        conn, _ = sock.accept()
        threading.Thread(target=handle_client, args=(conn,), daemon=True).start()

# Example: Call from your Flask/FastAPI middleware
def register_request(trace_id: str, span_id: str, tid: int):
    ACTIVE_TRACES[tid] = {"trace_id": trace_id, "span_id": span_id}
    # Auto-cleanup after 30s
    threading.Timer(30, lambda tid: ACTIVE_TRACES.pop(tid, None), args=(tid,)).start()

if __name__ == "__main__":
    start_bridge()
