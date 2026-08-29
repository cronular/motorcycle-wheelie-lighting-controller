"use strict";

const $ = (id) => document.getElementById(id);
const clamp = (value, low, high) => Math.max(low, Math.min(high, value));
const fmtSigned = (value, digits = 1) => `${value >= 0 ? "+" : ""}${value.toFixed(digits)}`;

const defaults = Object.freeze({
  triggerAngle: 20, resetAngle: 10, triggerHold: 150, minimumOn: 1000,
  brightness: 100, adaptiveTau: 4, freezeRate: 8,
  warningAngle: 45, warningReset: 40, warningRate: 45,
  angleMode: "absolute", wheeliePattern: "solid"
});

const scenarioDefinitions = {
  manual: { duration: Infinity, sample: () => null },
  wheelie: { duration: 8, sample(t) {
    const pitch = t < 1 ? 0 : t < 2 ? (t - 1) * 32 : t < 5 ? 32 : t < 6 ? 32 * (6 - t) : 0;
    const gyro = t >= 1 && t < 2 ? 32 : t >= 5 && t < 6 ? -32 : 0;
    const g = t > .8 && t < 1.35 ? .75 * Math.sin((t - .8) / .55 * Math.PI) : t > 5 && t < 5.4 ? .45 : .03;
    return { pitch, gyro, g, imu: true };
  }},
  blip: { duration: 4, sample(t) {
    const pitch = t >= 1 && t < 1.1 ? 26 : 0;
    return { pitch, gyro: t < 1 ? 0 : t < 1.1 ? 90 : t < 1.3 ? -45 : 0, g: t >= 1 && t < 1.3 ? .35 : 0, imu: true };
  }},
  hill: { duration: 14, sample(t) {
    const hill = t < 7 ? t * 2 : 14;
    const lift = t < 8 ? 0 : t < 9 ? (t - 8) * 30 : t < 11 ? 30 : t < 12 ? 30 * (12 - t) : 0;
    return { pitch: hill + lift, gyro: t < 7 ? 2 : t >= 8 && t < 9 ? 30 : t >= 11 && t < 12 ? -30 : 0, g: lift > 0 ? .45 : .03, imu: true };
  }},
  warning: { duration: 8, sample(t) {
    const pitch = t < 1 ? 0 : t < 2 ? (t - 1) * 58 : t < 5 ? 58 : t < 6.5 ? 58 * (6.5 - t) / 1.5 : 0;
    return { pitch, gyro: t >= 1 && t < 2 ? 58 : t >= 5 && t < 6.5 ? -39 : 0, g: pitch > 20 ? .55 : 0, imu: true };
  }},
  fault: { duration: 8, sample(t) {
    return { pitch: t < 1 ? 0 : 27, gyro: t >= 1 && t < 1.5 ? 54 : 0, g: t > 1 ? .25 : 0, imu: !(t >= 2.5 && t < 5.5) };
  }}
};

const sim = {
  timeMs: 0, scenarioTime: 0, accumulator: 0, running: true, speed: 1,
  scenario: "manual", mode: "STANDBY", state: "NORMAL", imu: true,
  absolutePitch: 0, gyro: 0, g: 0, triggerPitch: 0, baseline: 0,
  baselineFrozen: false, warning: false, output: 0, triggerStart: 0,
  wheelieStart: 0, completedWheelies: 0, lastDuration: 0,
  activePeak: 0, highestAngle: 0, highestG: 0, page: 0, apEnabled: true,
  previousState: "NORMAL", previousWarning: false, previousImu: true
};

function readSettings() {
  return {
    angleMode: $("angleMode").value,
    wheeliePattern: $("wheeliePattern").value,
    triggerAngle: clamp(Number($("triggerAngle").value) || defaults.triggerAngle, 5, 70),
    resetAngle: clamp(Number($("resetAngle").value), 0, 69),
    triggerHold: clamp(Number($("triggerHold").value), 0, 5000),
    minimumOn: clamp(Number($("minimumOn").value), 0, 15000),
    brightness: clamp(Number($("brightness").value), 1, 100),
    adaptiveTau: clamp(Number($("adaptiveTau").value), .5, 60),
    freezeRate: clamp(Number($("freezeRate").value), 1, 100),
    warningAngle: clamp(Number($("warningAngle").value), 5, 85),
    warningReset: clamp(Number($("warningReset").value), 0, 84),
    warningRate: clamp(Number($("warningRate").value), 0, 250)
  };
}

function patternBrightness(pattern, brightness, now) {
  if (pattern === "off") return 0;
  if (pattern === "solid") return brightness;
  if (pattern === "strobe") return now % 320 < 95 ? brightness : 0;
  const period = pattern === "slow" ? 1600 : 650;
  const phase = (now % period) / period;
  return Math.round(brightness * (.18 + .82 * (.5 - .5 * Math.cos(phase * 2 * Math.PI))));
}

function setControllerMode(mode) {
  if (sim.mode === mode) return;
  sim.mode = mode; sim.state = "NORMAL"; sim.warning = false; sim.output = 0;
  sim.triggerStart = 0; sim.wheelieStart = 0;
  if (mode === "ARMED" && readSettings().angleMode === "adaptive") {
    sim.baseline = sim.absolutePitch; sim.triggerPitch = 0; sim.baselineFrozen = false;
  }
  logEvent(`Controller → ${mode}`, mode === "ARMED" ? "good" : "");
}

function setAngleMode(mode) {
  $("angleMode").value = mode;
  sim.state = "NORMAL"; sim.warning = false; sim.output = 0;
  if (mode === "adaptive") { sim.baseline = sim.absolutePitch; sim.triggerPitch = 0; }
  else { sim.baseline = 0; sim.triggerPitch = sim.absolutePitch; }
  logEvent(`Angle processing → ${mode.toUpperCase()}`);
}

function controllerStep(dt) {
  const s = readSettings();
  if (s.resetAngle >= s.triggerAngle) s.resetAngle = Math.max(0, s.triggerAngle - .5);
  const allowsTracking = sim.mode === "ARMED" && sim.state === "NORMAL";
  if (s.angleMode === "absolute") {
    sim.baseline = 0; sim.baselineFrozen = false; sim.triggerPitch = sim.absolutePitch;
  } else {
    const canTrack = sim.imu && allowsTracking && Math.abs(sim.gyro) < s.freezeRate;
    sim.baselineFrozen = !canTrack;
    if (canTrack) {
      const alpha = dt / (Math.max(s.adaptiveTau, .5) + dt);
      sim.baseline += (sim.absolutePitch - sim.baseline) * clamp(alpha, 0, 1);
    }
    sim.triggerPitch = sim.absolutePitch - sim.baseline;
  }
  sim.highestAngle = Math.max(sim.highestAngle, sim.triggerPitch);
  sim.highestG = Math.max(sim.highestG, sim.g);

  const priorState = sim.state;
  if (sim.mode !== "ARMED" || !sim.imu) {
    sim.state = "NORMAL"; sim.warning = false; sim.output = 0;
  } else {
    let outputOn = false;
    if (sim.state === "NORMAL") {
      if (sim.triggerPitch >= s.triggerAngle) { sim.triggerStart = sim.timeMs; sim.state = "PENDING"; }
    } else if (sim.state === "PENDING") {
      if (sim.triggerPitch < s.triggerAngle) sim.state = "NORMAL";
      else if ((sim.timeMs - sim.triggerStart) >>> 0 >= s.triggerHold) {
        sim.wheelieStart = sim.timeMs; sim.state = "WHEELIE"; outputOn = true;
      }
    } else if (sim.state === "WHEELIE") {
      outputOn = true;
      if ((sim.timeMs - sim.wheelieStart) >>> 0 >= s.minimumOn && sim.triggerPitch <= s.resetAngle) {
        sim.state = "NORMAL"; outputOn = false;
      }
    }
    if (sim.state === "WHEELIE") outputOn = true;

    const rateEnabled = s.warningRate > 0;
    const rateWarning = rateEnabled && sim.gyro >= s.warningRate && sim.triggerPitch >= s.triggerAngle;
    if (!sim.warning) sim.warning = sim.triggerPitch >= s.warningAngle || rateWarning;
    else if (sim.triggerPitch <= s.warningReset && (!rateEnabled || sim.gyro < s.warningRate * .5)) sim.warning = false;
    const maxPwm = Math.round(s.brightness * 2.55);
    sim.output = sim.warning ? patternBrightness("strobe", 255, sim.timeMs) : outputOn ? patternBrightness(s.wheeliePattern, maxPwm, sim.timeMs) : 0;
  }

  if (priorState !== "WHEELIE" && sim.state === "WHEELIE") sim.activePeak = sim.triggerPitch;
  if (sim.state === "WHEELIE") sim.activePeak = Math.max(sim.activePeak, sim.triggerPitch);
  if (priorState === "WHEELIE" && sim.state !== "WHEELIE") {
    sim.completedWheelies++; sim.lastDuration = sim.timeMs - sim.wheelieStart;
  }
  emitTransitions(priorState);
}

function emitTransitions(priorState) {
  if (priorState !== sim.state) {
    const tone = sim.state === "WHEELIE" ? "good" : sim.state === "PENDING" ? "warn" : "";
    logEvent(`State ${priorState} → ${sim.state}`, tone);
  }
  if (sim.previousWarning !== sim.warning) logEvent(`High-angle warning ${sim.warning ? "ON" : "OFF"}`, sim.warning ? "warn" : "");
  if (sim.previousImu !== sim.imu) logEvent(`MPU6050 ${sim.imu ? "reconnected" : "disconnected — output forced off"}`, sim.imu ? "good" : "bad");
  sim.previousState = sim.state; sim.previousWarning = sim.warning; sim.previousImu = sim.imu;
}

function simulationTick(dt) {
  sim.timeMs = (sim.timeMs + Math.round(dt * 1000)) >>> 0;
  sim.scenarioTime += dt;
  const definition = scenarioDefinitions[sim.scenario];
  const sample = definition.sample(sim.scenarioTime);
  if (sample) {
    sim.absolutePitch = sample.pitch; sim.gyro = sample.gyro; sim.g = sample.g; sim.imu = sample.imu;
    $("imuHealthy").checked = sim.imu;
    syncInputControls();
  } else {
    sim.absolutePitch = Number($("pitchInput").value);
    sim.gyro = Number($("gyroInput").value);
    sim.g = Number($("gInput").value);
    sim.imu = $("imuHealthy").checked;
  }
  controllerStep(dt);
  if (Number.isFinite(definition.duration) && sim.scenarioTime >= definition.duration) selectScenario(sim.scenario);
}

let lastFrame = performance.now();
function frame(now) {
  const elapsed = Math.min(.1, (now - lastFrame) / 1000); lastFrame = now;
  if (sim.running) {
    sim.accumulator += elapsed * sim.speed;
    while (sim.accumulator >= .01) { simulationTick(.01); sim.accumulator -= .01; }
  }
  render(); requestAnimationFrame(frame);
}

function selectScenario(name) {
  sim.scenario = name; sim.scenarioTime = 0; sim.accumulator = 0;
  document.querySelectorAll("[data-scenario]").forEach((button) => button.classList.toggle("active", button.dataset.scenario === name));
  if (name !== "manual") setControllerMode("ARMED");
  logEvent(`Scenario loaded: ${document.querySelector(`[data-scenario="${name}"]`).textContent}`);
}

function resetSimulation() {
  Object.assign(sim, { timeMs: 0, scenarioTime: 0, accumulator: 0, state: "NORMAL", warning: false,
    output: 0, triggerStart: 0, wheelieStart: 0, baseline: 0, triggerPitch: 0,
    baselineFrozen: false, completedWheelies: 0, lastDuration: 0, activePeak: 0,
    highestAngle: 0, highestG: 0, previousState: "NORMAL", previousWarning: false, previousImu: sim.imu });
  logEvent("Simulation reset");
}

function syncInputControls() {
  $("pitchInput").value = clamp(sim.absolutePitch, -20, 75);
  $("gyroInput").value = clamp(sim.gyro, -120, 120);
  $("gInput").value = clamp(sim.g, 0, 2.5);
}

function oledRows() {
  const s = readSettings();
  if (sim.mode === "STANDBY") return ["    STANDBY", "----------------", "OUTPUT: OFF", "", `PITCH:${fmtSigned(sim.triggerPitch)}`, "", "Hold 1.5s ARM", "30s WiFi reset"];
  if (sim.page === 0) return ["WHEELIE CTRL 1/4", "MODE: ARMED", `PITCH:${fmtSigned(sim.triggerPitch)} deg`, `STATE:${sim.state}`, `OUTPUT:${Math.round(sim.output/2.55)}%`, `ANGLE:${s.angleMode === "adaptive" ? "ADAPT" : "ABS"}`, "1xPg 2xAng 3xAP", "Hold=Arm 30sPwd"];
  if (sim.page === 1) return ["SETTINGS     2/4", "----------------", `TRIG:${s.triggerAngle.toFixed(1)} deg`, `RESET:${s.resetAngle.toFixed(1)} deg`, `MODE:${s.angleMode === "adaptive" ? "ADAPT" : "ABS"}`, s.angleMode === "adaptive" ? `TAU:${s.adaptiveTau.toFixed(1)}s` : `HOLD:${s.triggerHold}ms`, s.angleMode === "adaptive" ? `FREEZE:${s.freezeRate.toFixed(1)}/s` : `MIN:${s.minimumOn}ms`, "2x btn = mode"];
  if (sim.page === 2) return ["NETWORK      3/4", "----------------", `AP: ${sim.apEnabled ? "ON" : "OFF"}`, sim.apEnabled ? "wheelie.local" : "WiFi disabled", sim.apEnabled ? "192.168.4.1" : "", "CLIENTS:0", `mDNS:${sim.apEnabled ? "OK" : "--"} DNS:${sim.apEnabled ? "OK" : "--"}`, "3xAP 30sPwdRst"];
  return ["DIAGNOSTICS  4/4", `RAW:${fmtSigned(sim.absolutePitch)}`, `BASE:${fmtSigned(sim.baseline)}`, `TRIG:${fmtSigned(sim.triggerPitch)}`, `GYRO:${fmtSigned(sim.gyro)}/s`, `+G:${sim.g.toFixed(2)}g`, `BASE:${sim.baselineFrozen ? "FROZEN" : "TRACKING"}`, `IMU:${sim.imu ? "OK" : "FAULT"}`];
}

function render() {
  $("simClock").textContent = `${(sim.timeMs / 1000).toFixed(2)} s`;
  $("runLabel").textContent = sim.running ? "RUNNING" : "PAUSED";
  $("runDot").style.background = sim.running ? "var(--green)" : "var(--orange)";
  $("playPause").textContent = sim.running ? "Pause" : "Run";
  $("stateHeading").textContent = sim.mode === "STANDBY" ? "STANDBY" : sim.state;
  $("warningBadge").classList.toggle("on", sim.warning);
  $("bike").style.transform = `rotate(${-clamp(sim.triggerPitch, -20, 70)}deg)`;
  $("pitchValue").textContent = `${fmtSigned(sim.triggerPitch)}°`;
  $("absoluteValue").textContent = `${fmtSigned(sim.absolutePitch)}°`;
  $("baselineValue").textContent = `${fmtSigned(sim.baseline)}°`;
  $("gyroValue").textContent = `${fmtSigned(sim.gyro)}°/s`;
  $("gValue").textContent = `${fmtSigned(sim.g, 2)} g`;
  $("outputValue").textContent = `${Math.round(sim.output / 2.55)}%`;
  $("pitchOut").textContent = `${sim.absolutePitch.toFixed(1)}°`;
  $("gyroOut").textContent = `${sim.gyro.toFixed(1)}°/s`;
  $("gOut").textContent = `${sim.g.toFixed(2)} g`;
  $("imuComponent").className = `component ${sim.imu ? "ok" : "fault"}`;
  $("imuStatus").textContent = sim.imu ? "OK" : "FAULT";
  $("imuDetail").textContent = sim.imu ? "I²C 0x68 · samples valid" : "I²C read failed · safe shutdown";
  $("pwmComponent").className = `component ${sim.output ? "ok" : ""}`;
  $("pwmStatus").textContent = sim.output ? "ON" : "OFF";
  $("pwmDetail").textContent = `GPIO D0 · ${sim.output} / 255`;
  const intensity = sim.output / 255;
  $("lamp").style.background = `rgba(255, 232, 178, ${.08 + intensity * .92})`;
  $("lamp").style.boxShadow = `inset 0 0 14px rgba(255,255,255,${intensity}), 0 0 ${Math.round(60*intensity)}px rgba(255,211,105,${.8*intensity})`;
  const rows = oledRows().map((row) => row.slice(0, 16).padEnd(16, " "));
  $("oled").textContent = rows.join("\n");
  $("oledPage").textContent = sim.mode === "STANDBY" ? "STANDBY" : `PAGE ${sim.page + 1}/4`;
}

function logEvent(message, tone = "") {
  const entry = document.createElement("div"); entry.className = `event ${tone}`;
  entry.innerHTML = `<time>${(sim.timeMs / 1000).toFixed(2)}s</time><span></span>`;
  entry.querySelector("span").textContent = message;
  $("eventLog").prepend(entry);
  while ($("eventLog").children.length > 80) $("eventLog").lastChild.remove();
}

document.querySelectorAll("[data-scenario]").forEach((button) => button.addEventListener("click", () => selectScenario(button.dataset.scenario)));
[$("pitchInput"), $("gyroInput"), $("gInput")].forEach((input) => input.addEventListener("input", () => selectScenario("manual")));
$("imuHealthy").addEventListener("change", () => { selectScenario("manual"); sim.imu = $("imuHealthy").checked; });
$("armButton").addEventListener("click", () => setControllerMode("ARMED"));
$("standbyButton").addEventListener("click", () => setControllerMode("STANDBY"));
$("playPause").addEventListener("click", () => { sim.running = !sim.running; logEvent(sim.running ? "Simulation resumed" : "Simulation paused"); });
$("resetSim").addEventListener("click", resetSimulation);
$("clearLog").addEventListener("click", () => { $("eventLog").textContent = ""; });
$("defaultsButton").addEventListener("click", () => { Object.entries(defaults).forEach(([key, value]) => { if ($(key)) $(key).value = value; }); setAngleMode("absolute"); logEvent("Default settings restored"); });
$("angleMode").addEventListener("change", (event) => setAngleMode(event.target.value));
document.querySelectorAll("[data-gesture]").forEach((button) => button.addEventListener("click", () => {
  const gesture = button.dataset.gesture;
  if (gesture === "single") { if (sim.mode === "ARMED") sim.page = (sim.page + 1) % 4; logEvent("Button: single tap"); }
  if (gesture === "double") setAngleMode(readSettings().angleMode === "absolute" ? "adaptive" : "absolute");
  if (gesture === "triple") { sim.apEnabled = !sim.apEnabled; logEvent(`Button: Wi-Fi AP ${sim.apEnabled ? "ON" : "OFF"}`); }
  if (gesture === "hold") setControllerMode(sim.mode === "ARMED" ? "STANDBY" : "ARMED");
}));

logEvent("Desktop simulator initialized", "good");
requestAnimationFrame(frame);
