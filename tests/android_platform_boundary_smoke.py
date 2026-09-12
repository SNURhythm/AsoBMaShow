import argparse
import base64
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import ssl
import subprocess
import tempfile
import threading


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--adb", default="adb")
    parser.add_argument("--apk", type=Path, required=True)
    parser.add_argument("--test-apk", type=Path, required=True)
    parser.add_argument("--flavor", choices=("firebase", "play"), required=True)
    arguments = parser.parse_args()
    requests = []
    ports = {}

    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            requests.append(self.path)
            if self.path in ("/upgrade", "/downgrade"):
                scheme = "https" if self.path == "/upgrade" else "http"
                target = "/chart.zip" if self.path == "/upgrade" else "/forbidden"
                self.send_response(302)
                self.send_header("Location", f"{scheme}://localhost:{ports[scheme]}{target}")
                self.end_headers()
                return
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b"table-fixture" if self.path == "/table.json" else b"archive-fixture")

        def log_message(self, *unused):
            pass

    def adb(*command, **options):
        return subprocess.run([arguments.adb, *command], check=True, **options)

    with tempfile.TemporaryDirectory(prefix="asobmashow-network-") as temporary:
        certificate = Path(temporary) / "certificate.pem"
        key = Path(temporary) / "key.pem"
        subprocess.run([
            "openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
            "-subj", "/CN=localhost", "-addext", "subjectAltName=DNS:localhost",
            "-keyout", str(key), "-out", str(certificate),
        ], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        servers = [ThreadingHTTPServer(("127.0.0.1", 0), Handler) for unused in range(2)]
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certificate, key)
        servers[1].socket = context.wrap_socket(servers[1].socket, server_side=True)
        for scheme, server in zip(("http", "https"), servers):
            ports[scheme] = server.server_port
            threading.Thread(target=server.serve_forever, daemon=True).start()
        try:
            for port in ports.values():
                adb("reverse", f"tcp:{port}", f"tcp:{port}")
            adb("install", "-r", str(arguments.apk))
            adb("install", "-r", "-t", str(arguments.test_apk))
            encoded = base64.b64encode(ssl.PEM_cert_to_DER_cert(certificate.read_text())).decode()
            completed = adb(
                "shell", "am", "instrument", "-w", "-r",
                "-e", "httpPort", str(ports["http"]),
                "-e", "httpsPort", str(ports["https"]),
                "-e", "fixtureCa", encoded,
                "com.snurhythm.asobmashow.test/com.snurhythm.asobmashow.PlatformBoundaryInstrumentation",
                capture_output=True, text=True, timeout=90,
            )
            print(completed.stdout)
            print(completed.stderr)
            assert f"PASS platform boundaries {arguments.flavor}" in completed.stdout
            assert "/forbidden" not in requests, "Downgrade reached the insecure destination"
            assert requests.count("/table.json") == 2 and requests.count("/chart.zip") == 2
        finally:
            for port in ports.values():
                adb("reverse", "--remove", f"tcp:{port}")
            for server in servers:
                server.shutdown()
                server.server_close()


if __name__ == "__main__":
    main()
