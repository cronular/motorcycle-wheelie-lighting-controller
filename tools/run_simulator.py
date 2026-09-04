"""Serve the desktop simulator using only the Python standard library."""

from argparse import ArgumentParser
from functools import partial
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from pathlib import Path
import re
import threading
import urllib.parse
import urllib.request
import webbrowser


PROJECT_DIR = Path(__file__).resolve().parent.parent
SIMULATOR_DIR = PROJECT_DIR / "simulator"
FIRMWARE_SOURCE = PROJECT_DIR / "src" / "modules" / "dashboard_module.cpp"


PHONE_BRIDGE = r"""
<script>
(() => {
  const nativeFetch = window.fetch.bind(window);
  async function bridge() {
    for (let attempt = 0; attempt < 80; attempt += 1) {
      if (window.parent !== window && window.parent.wheelieSimulatorApi) {
        return window.parent.wheelieSimulatorApi;
      }
      await new Promise(resolve => setTimeout(resolve, 25));
    }
    throw new Error('Simulator bridge unavailable');
  }
  window.fetch = async (input, options = {}) => {
    const raw = typeof input === 'string' ? input : input.url;
    const url = new URL(raw, window.location.href);
    if (!url.pathname.startsWith('/api/')) return nativeFetch(input, options);
    const api = await bridge();
    const result = await api.request(url.pathname + url.search, {
      method: options.method || (input && input.method) || 'GET',
      body: options.body || null
    });
    return new Response(result.body || '', {
      status: result.status || 200,
      headers: {'Content-Type': result.contentType || 'text/plain'}
    });
  };
  window.addEventListener('DOMContentLoaded', async () => {
    try {
      const api = await bridge();
      api.phoneReady(window.location.pathname);
    } catch (error) {}
    document.querySelectorAll('a[download][href^="/api/"]').forEach(link => link.addEventListener('click', async event => {
      event.preventDefault();
      const response = await window.fetch(link.getAttribute('href'));
      const blob = await response.blob(), download = document.createElement('a');
      download.href = URL.createObjectURL(blob);
      download.download = link.getAttribute('download') || 'capture.csv';
      download.click();
      setTimeout(() => URL.revokeObjectURL(download.href), 1000);
    }));
  });
})();
</script>
"""


def embedded_html(name: str) -> str:
    source = FIRMWARE_SOURCE.read_text(encoding="utf-8")
    match = re.search(
        rf'const char {re.escape(name)}\[\] PROGMEM = R"rawliteral\((.*?)\)rawliteral";',
        source,
        re.DOTALL,
    )
    if not match:
        raise RuntimeError(f"Could not extract {name} from {FIRMWARE_SOURCE}")
    return match.group(1)


def phone_page(name: str) -> bytes:
    page = embedded_html(name).replace("</head>", PHONE_BRIDGE + "</head>")
    if name == "DASHBOARD_HTML":
        page = page.replace('href="/settings"', 'href="settings/"')
        page = page.replace('href="/capture"', 'href="capture/"')
    else:
        page = page.replace('href="/"', 'href="../"')
    return page.encode("utf-8")


PHONE_DASHBOARD = phone_page("DASHBOARD_HTML")
PHONE_SETTINGS = phone_page("SETTINGS_HTML")
PHONE_CAPTURE = phone_page("CAPTURE_HTML")


class SimulatorHandler(SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def send_phone_page(self, body: bytes):
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = urllib.parse.urlparse(self.path).path
        if path in ("/phone", "/phone/"):
            self.send_phone_page(PHONE_DASHBOARD)
            return
        if path in ("/phone/settings", "/phone/settings/"):
            self.send_phone_page(PHONE_SETTINGS)
            return
        if path in ("/phone/capture", "/phone/capture/"):
            self.send_phone_page(PHONE_CAPTURE)
            return
        super().do_GET()


def main():
    parser = ArgumentParser(description="Run the Wheelie Controller desktop simulator")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--no-browser", action="store_true")
    parser.add_argument("--check", action="store_true", help="Smoke-test files and HTTP serving, then exit")
    args = parser.parse_args()

    required = ("index.html", "styles.css", "simulator.js")
    missing = [name for name in required if not (SIMULATOR_DIR / name).is_file()]
    if missing:
        raise SystemExit("Missing simulator files: " + ", ".join(missing))
    simulator_script = (SIMULATOR_DIR / "simulator.js").read_text(encoding="utf-8")
    bridge_markers = (
        "wheelieSimulatorApi", "phoneStatusSnapshot", '"/api/settings"',
        '"/api/model"', '"/api/model/feedback"'
    )
    if any(marker not in simulator_script for marker in bridge_markers):
        raise SystemExit("Simulator phone API bridge is incomplete")

    handler = partial(SimulatorHandler, directory=str(SIMULATOR_DIR))
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    url = f"http://127.0.0.1:{args.port}/"

    if args.check:
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            with urllib.request.urlopen(url, timeout=3) as response:
                body = response.read().decode("utf-8")
                checks = {
                    "http-200": response.status == 200,
                    "lab-title": "Wheelie Controller Lab" in body,
                    "phone-frame": 'src="phone/"' in body,
                }
                missing = [name for name, passed in checks.items() if not passed]
                if missing:
                    raise SystemExit("Simulator HTTP smoke test failed: " + ", ".join(missing))
            with urllib.request.urlopen(url + "phone/", timeout=3) as response:
                body = response.read().decode("utf-8")
                checks = {
                    "http-200": response.status == 200,
                    "api-bridge": "wheelieSimulatorApi" in body,
                    "settings-link": 'href="settings/"' in body,
                    "capture-link": 'href="capture/"' in body,
                    "rider-hud": "Rider HUD" in body,
                    "lean-gauge": 'id="leanGauge"' in body,
                }
                missing = [name for name, passed in checks.items() if not passed]
                if missing:
                    raise SystemExit("Phone dashboard bridge smoke test failed: " + ", ".join(missing))
            with urllib.request.urlopen(url + "phone/settings", timeout=3) as response:
                body = response.read().decode("utf-8")
                checks = {
                    "http-200": response.status == 200,
                    "settings-title": "Controller Settings" in body,
                    "dashboard-link": 'href="../"' in body,
                    "lean-setting": 'id="leanGaugeSetting"' in body,
                    "model-setting": 'id="riderModel"' in body,
                    "model-events": 'id="modelEvents"' in body,
                }
                missing = [name for name, passed in checks.items() if not passed]
                if missing:
                    raise SystemExit("Phone settings bridge smoke test failed: " + ", ".join(missing))
            with urllib.request.urlopen(url + "phone/capture", timeout=3) as response:
                body = response.read().decode("utf-8")
                checks = {
                    "http-200": response.status == 200,
                    "capture-title": "Data Capture" in body,
                    "event-button": 'id="eventBtn"' in body,
                    "api-bridge": "wheelieSimulatorApi" in body,
                }
                missing = [name for name, passed in checks.items() if not passed]
                if missing:
                    raise SystemExit("Phone capture bridge smoke test failed: " + ", ".join(missing))
        finally:
            server.shutdown()
            server.server_close()
        print("Simulator smoke test passed")
        return

    print(f"Wheelie Controller simulator: {url}")
    print("Press Ctrl+C to stop.")
    if not args.no_browser:
        webbrowser.open(url)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nSimulator stopped")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
