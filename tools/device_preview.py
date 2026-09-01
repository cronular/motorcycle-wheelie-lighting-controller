"""Local browser emulator for the Wheelie Controller web UI.

Serves the exact PROGMEM HTML from the dashboard module and emulates the REST API.
No third-party packages are required.
"""

from __future__ import annotations

import json
import math
import re
import argparse
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "src" / "modules" / "dashboard_module.cpp"
HOST = "127.0.0.1"
PORT = 8899
TOKEN = "LOCAL-PREVIEW"


def embedded_html(name: str) -> bytes:
    source = SOURCE.read_text(encoding="utf-8")
    match = re.search(
        rf"const char {re.escape(name)}\[\] PROGMEM = R\"rawliteral\((.*?)\)rawliteral\";",
        source,
        re.DOTALL,
    )
    if not match:
        raise RuntimeError(f"Could not extract {name} from {SOURCE}")
    return match.group(1).encode("utf-8")


DASHBOARD = embedded_html("DASHBOARD_HTML")
SETTINGS = embedded_html("SETTINGS_HTML")

STATIC_SHIM = r"""
<script>
(() => {
  const realFetch = window.fetch.bind(window), started = performance.now();
  let mode = 'ARMED', peakAngle = 0, peakG = 0;
  const base = {angleMode:'adaptive',rotationAxis:'y',adaptiveTau:4,freezeRate:8,
    trigger:25,reset:12,hold:150,minon:1000,brightness:100,fade:200,
    bootArmed:false,warningAngle:42,warningReset:36,warningRate:45,
    wheeliePattern:1,warningPattern:4,warningBrightness:100,otaChannel:'testing'};
  function status(){
    const elapsed=(performance.now()-started)/1000, phase=elapsed%14;
    let pitch=phase<3?1.5*Math.sin(phase*2):phase<7?(phase-3)/4*49:
      phase<9?49-2*Math.sin((phase-7)*Math.PI):phase<12?Math.max(0,49*(1-(phase-9)/3)):.7*Math.sin(phase*3);
    const gyro=phase>=3&&phase<7?12.25:phase>=9&&phase<12?-16.33:0;
    const wheelie=mode==='ARMED'&&pitch>=base.trigger, warning=mode==='ARMED'&&pitch>=base.warningAngle;
    const g=Math.max(0,.08+Math.abs(gyro)/90+.05*Math.sin(elapsed*4));
    peakAngle=Math.max(peakAngle,pitch);peakG=Math.max(peakG,g);
    return {...base,pitch,rawPitch:pitch+2.4,roll:Math.sin(elapsed)*8,rollRate:Math.cos(elapsed)*8,baseline:2.4,gyroRate:gyro,gLoad:g,
      peakAngle,peakG,warningActive:warning,eventCount:3,activeDuration:wheelie?Math.max(0,phase-5)*1000:0,
      lastDuration:4200,lastPeakAngle:38.6,lastPeakG:.72,accelX:0,accelY:0,accelZ:1,
      baselineFrozen:wheelie,mode,state:wheelie?'WHEELIE':pitch>=base.trigger-2?'PENDING':'NORMAL',
      output:wheelie||warning?100:0,imu:true,apEnabled:true,ssid:'wheelie_controller_4821',
      rollAxis:'x',verticalAxis:'z',orientationConfigured:true,mdns:true,dns:true,clients:1,uptime:Math.floor(elapsed),firmware:'preview',
      board:'seeed_xiao_esp32s3',chip:'esp32s3',buildCommit:'preview123456',buildDate:'2026-08-30T12:00:00Z',releaseChannel:'testing',signedOta:true,calOneGRaw:1,
      calAccelRms:.008,calGyroRms:.24,calHighVibration:false,token:'LOCAL-PREVIEW'};
  }
  window.fetch = async (input, options={}) => {
    const url=String(input), path=url.split('?')[0];
    if(path.endsWith('/api/status'))return new Response(JSON.stringify(status()),{status:200,headers:{'Content-Type':'application/json'}});
    if(path.endsWith('/api/mode')){mode=(new URL(url,location.href)).searchParams.get('mode').toUpperCase();return new Response('Controller '+mode);}
    if(path.endsWith('/api/peak/reset')){peakAngle=0;peakG=0;return new Response('Peak reset');}
    if(path.endsWith('/api/calibrate'))return new Response('Calibration complete (preview)');
    if(path.endsWith('/api/update')||path.endsWith('/api/rollback'))return new Response('Firmware operations disabled in preview',{status:409});
    if(path.includes('/api/'))return new Response('Command accepted (preview)');
    return realFetch(input,options);
  };
})();
</script>
"""


def build_static_preview() -> Path:
    output = ROOT / ".preview"
    output.mkdir(exist_ok=True)
    dashboard = DASHBOARD.decode("utf-8").replace("</head>", STATIC_SHIM + "</head>")
    settings = SETTINGS.decode("utf-8").replace("</head>", STATIC_SHIM + "</head>")
    dashboard = dashboard.replace('href="/settings"', 'href="settings.html"')
    settings = settings.replace('href="/"', 'href="dashboard.html"')
    (output / "dashboard.html").write_text(dashboard, encoding="utf-8")
    (output / "settings.html").write_text(settings, encoding="utf-8")
    return output / "dashboard.html"

state = {
    "mode": "ARMED",
    "peakAngle": 0.0,
    "peakG": 0.0,
    "eventCount": 3,
    "lastDuration": 4200,
    "lastPeakAngle": 38.6,
    "lastPeakG": 0.72,
    "settings": {
        "angleMode": "adaptive",
        "rotationAxis": "y",
        "adaptiveTau": 4.0,
        "freezeRate": 8.0,
        "trigger": 25.0,
        "reset": 12.0,
        "hold": 150,
        "minon": 1000,
        "brightness": 100,
        "fade": 200,
        "bootArmed": False,
        "warningAngle": 42.0,
        "warningReset": 36.0,
        "warningRate": 45.0,
        "wheeliePattern": 1,
        "warningPattern": 4,
        "warningBrightness": 100,
        "otaChannel": "testing",
    },
}
started = time.monotonic()


def telemetry() -> dict:
    elapsed = time.monotonic() - started
    # A repeating 14-second ride cycle: level, lift, high-angle warning, land.
    phase = elapsed % 14.0
    if phase < 3.0:
        pitch = 1.5 * math.sin(phase * 2.0)
    elif phase < 7.0:
        pitch = (phase - 3.0) / 4.0 * 49.0
    elif phase < 9.0:
        pitch = 49.0 - 2.0 * math.sin((phase - 7.0) * math.pi)
    elif phase < 12.0:
        pitch = max(0.0, 49.0 * (1.0 - (phase - 9.0) / 3.0))
    else:
        pitch = 0.7 * math.sin(phase * 3.0)

    previous_phase = (elapsed - 0.15) % 14.0
    previous_pitch = pitch if previous_phase > phase else (
        0.0 if previous_phase < 3.0 else min(49.0, (previous_phase - 3.0) / 4.0 * 49.0)
    )
    gyro = (pitch - previous_pitch) / 0.15
    settings = state["settings"]
    armed = state["mode"] == "ARMED"
    wheelie = armed and pitch >= settings["trigger"]
    warning = armed and pitch >= settings["warningAngle"]
    controller_state = "WHEELIE" if wheelie else ("PENDING" if armed and pitch >= settings["trigger"] - 2 else "NORMAL")
    g_load = max(0.0, 0.08 + abs(gyro) / 90.0 + 0.05 * math.sin(elapsed * 4.0))
    state["peakAngle"] = max(state["peakAngle"], pitch)
    state["peakG"] = max(state["peakG"], g_load)

    return {
        "pitch": pitch,
        "rawPitch": pitch + 2.4,
        "roll": math.sin(elapsed) * 8.0,
        "rollRate": math.cos(elapsed) * 8.0,
        "baseline": 2.4,
        "gyroRate": gyro,
        "gLoad": g_load,
        "peakAngle": state["peakAngle"],
        "peakG": state["peakG"],
        "warningActive": warning,
        "eventCount": state["eventCount"],
        "activeDuration": int(max(0.0, phase - 5.0) * 1000) if wheelie else 0,
        "lastDuration": state["lastDuration"],
        "lastPeakAngle": state["lastPeakAngle"],
        "lastPeakG": state["lastPeakG"],
        "accelX": 0.0,
        "accelY": 0.0,
        "accelZ": 1.0,
        "baselineFrozen": wheelie,
        "mode": state["mode"],
        "state": controller_state,
        "output": 100 if wheelie or warning else 0,
        "imu": True,
        "rollAxis": "x",
        "verticalAxis": "z",
        "orientationConfigured": True,
        **settings,
        "apEnabled": True,
        "ssid": "wheelie_controller_4821",
        "mdns": True,
        "dns": True,
        "clients": 1,
        "uptime": int(elapsed),
        "firmware": "preview",
        "board": "seeed_xiao_esp32s3",
        "chip": "esp32s3",
        "buildCommit": "preview123456",
        "buildDate": "2026-08-30T12:00:00Z",
        "releaseChannel": "testing",
        "signedOta": True,
        "calOneGRaw": 1.0,
        "calAccelRms": 0.008,
        "calGyroRms": 0.24,
        "calHighVibration": False,
        "token": TOKEN,
    }


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt: str, *args: object) -> None:
        print(f"[{self.log_date_time_string()}] {fmt % args}")

    def send_bytes(self, status: int, content_type: str, body: bytes) -> None:
        self.send_response(status)
        self.send_header("Content-Type", f"{content_type}; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        path = urllib.parse.urlparse(self.path).path
        if path == "/":
            self.send_bytes(200, "text/html", DASHBOARD)
        elif path == "/settings":
            self.send_bytes(200, "text/html", SETTINGS)
        elif path == "/api/status":
            self.send_bytes(200, "application/json", json.dumps(telemetry()).encode())
        else:
            self.send_response(302)
            self.send_header("Location", "/")
            self.end_headers()

    def do_POST(self) -> None:
        parsed = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(parsed.query)
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8", errors="replace")
        form = urllib.parse.parse_qs(body)
        if query.get("token", [""])[0] != TOKEN:
            self.send_bytes(403, "text/plain", b"Invalid write token")
            return

        if parsed.path == "/api/mode":
            state["mode"] = query.get("mode", ["standby"])[0].upper()
            response = f"Controller {state['mode']}"
        elif parsed.path == "/api/calibrate":
            response = "Calibration complete (preview)"
        elif parsed.path == "/api/peak/reset":
            kind = query.get("kind", [""])[0]
            if kind in ("angle", "all"):
                state["peakAngle"] = 0.0
            if kind in ("g", "all"):
                state["peakG"] = 0.0
            response = "Peak reset"
        elif parsed.path == "/api/settings":
            for key, values in form.items():
                value = values[0]
                if key in state["settings"]:
                    try:
                        state["settings"][key] = float(value)
                    except ValueError:
                        state["settings"][key] = value
            response = "Settings saved (preview)"
        elif parsed.path in ("/api/output", "/api/wifi"):
            response = "Command accepted (preview)"
        elif parsed.path in ("/api/update", "/api/rollback"):
            self.send_bytes(409, "text/plain", b"Firmware operations are disabled in preview mode")
            return
        else:
            self.send_bytes(404, "text/plain", b"Unknown endpoint")
            return
        self.send_bytes(200, "text/plain", response.encode())


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Emulate the Wheelie Controller web UI")
    parser.add_argument("--static", action="store_true", help="write a zero-server HTML preview")
    parser.add_argument("--port", type=int, default=PORT, help=f"preferred HTTP port (default: {PORT})")
    args = parser.parse_args()

    if args.static:
        print(build_static_preview())
        raise SystemExit(0)

    try:
        server = ThreadingHTTPServer((HOST, args.port), Handler)
    except OSError as error:
        # Windows may reserve blocks of otherwise-unused ports for Hyper-V,
        # WSL, VPNs, or Internet Connection Sharing. Let the OS select a safe
        # ephemeral port instead of making the preview fail outright.
        print(f"Port {args.port} is unavailable ({error}); selecting a free port.")
        server = ThreadingHTTPServer((HOST, 0), Handler)

    actual_port = server.server_address[1]
    print(f"Wheelie Controller preview: http://{HOST}:{actual_port}")
    print("Press Ctrl+C to stop.")
    server.serve_forever()
