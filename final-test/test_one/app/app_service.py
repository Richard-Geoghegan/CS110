import http.server
import json
import os
import re
import socket
import threading
import time
import uuid

from opentelemetry import trace
from opentelemetry.exporter.otlp.proto.http.trace_exporter import OTLPSpanExporter
from opentelemetry.sdk.resources import Resource
from opentelemetry.sdk.trace import TracerProvider
from opentelemetry.sdk.trace.export import BatchSpanProcessor

# --- OTel Initialization ---
resource = Resource.create({"service.name": "app-service"})
provider = TracerProvider(resource=resource)
processor = BatchSpanProcessor(
    OTLPSpanExporter(endpoint="http://otel-collector:4318/v1/traces")
)
provider.add_span_processor(processor)
trace.set_tracer_provider(provider)
tracer = trace.get_tracer(__name__)

# --- Config ---
LOOPS = int(os.getenv("WORKLOAD_LOOPS", "5000"))
DELAY = float(os.getenv("WORKLOAD_DELAY", "0.01"))
BRIDGE_SOCKET = os.getenv("TRACE_SOCKET_PATH", "/tmp/trace_bridge.sock")

def gen_trace_id(): return uuid.uuid4().hex + uuid.uuid4().hex
def gen_span_id(): return uuid.uuid4().hex[:16]

def register_bridge(tid, trace_id, span_id):
    """Send mapping to the bridge socket"""
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(0.2)
        s.connect(BRIDGE_SOCKET)
        s.sendall(f"REGISTER {tid} {trace_id} {span_id}\n".encode())
        s.close()
    except: pass

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        # 1. Parse or Generate W3C Trace Context
        traceparent = self.headers.get("traceparent", "")
        match = re.match(r"00-([0-9a-f]{32})-([0-9a-f]{16})-\d{2}", traceparent)
        if match:
            trace_id, span_id = match.group(1), match.group(2)
        else:
            trace_id, span_id = gen_trace_id(), gen_span_id()
        
        # 2. Register with Bridge (Phase 3)
        # get_native_id() returns the Linux kernel TID, which matches what
        # perf's PERF_SAMPLE_TID captures — get_ident() is a Python-internal
        # opaque value that differs from the kernel TID on handler threads.
        current_tid = threading.get_native_id()
        register_bridge(current_tid, trace_id, span_id)
        
        # 3. Create OTel Span
        with tracer.start_as_current_span("app-request") as span:
            span.set_attribute("http.method", "GET")
            span.set_attribute("trace_id", trace_id)
            
            start = time.monotonic()
            # Memory-intensive CPU work — repeatedly read a large array
            # to generate cache misses that the PMU can observe
            arr = list(range(LOOPS))
            total = 0
            for i in range(0, len(arr), 8):   # stride-8 access pattern
                total += arr[i]
            latency = (time.monotonic() - start) * 1000
            
            span.set_attribute("latency_ms", latency)
        
        # 4. Response
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("traceparent", f"00-{trace_id}-{span_id}-01")
        self.end_headers()
        self.wfile.write(json.dumps({"status": "ok", "latency_ms": latency}).encode())
    
    def log_message(self, *args): pass

if __name__ == "__main__":
    print(f"🚀 App Service on :8000 | OTel → otel-collector:4318")
    http.server.ThreadingHTTPServer(("0.0.0.0", 8000), Handler).serve_forever()
