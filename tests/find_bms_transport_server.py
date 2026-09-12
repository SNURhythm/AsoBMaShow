import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import subprocess
import threading
import time
from urllib.parse import urlsplit


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def do_GET(self):
        route = urlsplit(self.path).path
        if route.startswith("/ios-metadata/"):
            self.ios_metadata(route.removeprefix("/ios-metadata"))
            return
        if route.startswith("/metadata/"):
            scenario = route.rsplit("/", 1)[-1]
            payload = b"12345678abcdefgh!"
            if scenario == "normal":
                payload = self.command.encode()
            elif scenario == "exact":
                payload = b"12345678abcdefgh"
            self.send_response(500 if scenario == "error" else 200)
            if scenario == "chunked":
                self.send_header("Transfer-Encoding", "chunked")
            elif scenario != "no-length":
                self.send_header("Content-Length", str(len(payload)))
            self.send_header("Connection", "close")
            self.end_headers()
            try:
                if scenario == "chunked":
                    self.wfile.write(f"{len(payload):x}\r\n".encode())
                self.wfile.write(payload)
                if scenario == "chunked":
                    self.wfile.write(b"\r\n0\r\n\r\n")
            except (BrokenPipeError, ConnectionResetError):
                pass
            self.close_connection = True
            return
        if route == "/redirect":
            self.send_response(302)
            self.send_header("Location", "/normal")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        if route == "/invalid-redirect":
            self.send_response(302)
            self.send_header("Location", "file:///fixture-must-not-open")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        size = 16 * 1024 if route in ("/normal", "/replace") else 128 * 1024
        if route == "/exact":
            size = 64 * 1024
        self.send_response(503 if route == "/error" else 200)
        chunked = route in ("/chunked", "/cancel", "/error")
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
        elif route != "/no-length":
            declared = size + 4096 if route == "/truncated" else size
            self.send_header("Content-Length", str(declared))
        self.send_header("Connection", "close")
        self.end_headers()
        if route == "/stall":
            time.sleep(2)
        try:
            for offset in range(0, size, 4096):
                payload = b"a" * min(4096, size - offset)
                if chunked:
                    self.wfile.write(f"{len(payload):x}\r\n".encode())
                self.wfile.write(payload)
                if chunked:
                    self.wfile.write(b"\r\n")
                self.wfile.flush()
                if route == "/cancel":
                    time.sleep(0.01)
            if chunked:
                self.wfile.write(b"0\r\n\r\n")
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True

    def do_POST(self):
        self.do_GET()

    def ios_metadata(self, route):
        if route in ("/redirect", "/redirect-over", "/invalid-redirect"):
            self.send_response(307)
            self.send_header("Location", {
                "/redirect": "/ios-metadata/normal",
                "/redirect-over": "/ios-metadata/oversized",
                "/invalid-redirect": "file:///fixture-must-not-open",
            }[route])
            self.send_header("Content-Length", "0")
            self.end_headers()
            return
        payload = b"12345678abcdefgh!"
        if route == "/normal":
            payload = self.command.encode()
        elif route == "/exact":
            payload = b"12345678abcdefgh"
        elif route == "/empty":
            payload = b""
        elif route == "/utf8":
            payload = "가나다".encode()
        elif route == "/invalid-utf8":
            payload = b"\xc0\xaf"
        elif route == "/nul":
            payload = b"a\0b"
        elif route == "/error":
            payload = b"error"
        chunked = route in ("/chunked", "/stream-stall", "/cancel")
        self.send_response(503 if route == "/error" else 200)
        self.send_header("Content-Type", "application/json")
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
        elif route != "/no-length":
            self.send_header("Content-Length", str(len(payload)))
        self.send_header("Connection", "close")
        self.end_headers()
        if route in ("/header-stall", "/cancel"):
            if route == "/header-stall":
                self.wfile.write(payload[:1])
                self.wfile.flush()
                payload = payload[1:]
            time.sleep(2)
        try:
            for offset in range(0, len(payload), 8):
                chunk = payload[offset:offset + 8]
                if chunked:
                    self.wfile.write(f"{len(chunk):x}\r\n".encode())
                self.wfile.write(chunk)
                if chunked:
                    self.wfile.write(b"\r\n")
                self.wfile.flush()
            if route == "/stream-stall":
                time.sleep(2)
            if chunked:
                self.wfile.write(b"0\r\n\r\n")
        except (BrokenPipeError, ConnectionResetError):
            pass
        self.close_connection = True


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("executable")
    args = parser.parse_args()
    server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    thread = threading.Thread(target=server.serve_forever, daemon=True)
    thread.start()
    try:
        result = subprocess.run(
            [args.executable, f"http://127.0.0.1:{server.server_port}"],
            timeout=45, check=False)
    finally:
        server.shutdown()
        server.server_close()
        thread.join(timeout=2)
    raise SystemExit(result.returncode)


if __name__ == "__main__":
    main()
