"""Serve the desktop simulator using only the Python standard library."""

from argparse import ArgumentParser
from functools import partial
from http.server import ThreadingHTTPServer, SimpleHTTPRequestHandler
from pathlib import Path
import threading
import urllib.request
import webbrowser


PROJECT_DIR = Path(__file__).resolve().parent.parent
SIMULATOR_DIR = PROJECT_DIR / "simulator"


class QuietHandler(SimpleHTTPRequestHandler):
    def log_message(self, format, *args):
        pass


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

    handler = partial(QuietHandler, directory=str(SIMULATOR_DIR))
    server = ThreadingHTTPServer(("127.0.0.1", args.port), handler)
    url = f"http://127.0.0.1:{args.port}/"

    if args.check:
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        with urllib.request.urlopen(url, timeout=3) as response:
            body = response.read().decode("utf-8")
            if response.status != 200 or "Wheelie Controller Lab" not in body:
                raise SystemExit("Simulator HTTP smoke test failed")
        server.shutdown()
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
