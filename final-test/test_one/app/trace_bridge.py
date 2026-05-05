import os
import socket
import threading
import time
from collections import defaultdict

SOCKET_PATH = os.getenv("TRACE_SOCKET_PATH", "/tmp/trace_bridge.sock")
ACTIVE_TRACES = defaultdict(dict)
LOCK = threading.Lock()

def start_bridge():
    if os.path.exists(SOCKET_PATH): os.unlink(SOCKET_PATH)
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    sock.bind(SOCKET_PATH)
    # Critical: Allow the host-level daemon (root) to read this socket
    os.chmod(SOCKET_PATH, 0o777) 
    sock.listen(32)
    print(f"[BRIDGE]  Listening on {SOCKET_PATH}")

    def handle(conn):
        try:
            data = conn.recv(256).decode().strip()
            if not data: return
            parts = data.split()
            cmd = parts[0]
            
            if cmd == "REGISTER" and len(parts) == 4:
                # Format: REGISTER <tid> <trace_id> <span_id>
                tid = int(parts[1])
                with LOCK: ACTIVE_TRACES[tid] = {"t": parts[2], "s": parts[3]}
                # Auto-expire after 15s
                threading.Timer(15, lambda t: ACTIVE_TRACES.pop(t, None), args=(tid,)).start()
                conn.sendall(b"OK\n")
            elif cmd == "QUERY" and len(parts) == 2:
                # Format: QUERY <tid>
                tid = int(parts[1])
                with LOCK:
                    ctx = ACTIVE_TRACES.get(tid, {"t": "00"*16, "s": "00"*8})
                conn.sendall(f"{ctx['t']} {ctx['s']}\n".encode())
        except Exception as e:
            pass
        finally: conn.close()

    while True:
        conn, _ = sock.accept()
        threading.Thread(target=handle, args=(conn,), daemon=True).start()

if __name__ == "__main__": start_bridge()
