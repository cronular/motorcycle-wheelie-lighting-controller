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

const patternValues = Object.freeze({ off: 0, solid: 1, slow: 2, fast: 3, strobe: 4 });
const patternNames = Object.freeze(["off", "solid", "slow", "fast", "strobe"]);
const device = {
  rotationAxis: "y", rollAxis: "x", verticalAxis: "z", orientationConfigured: true,
  roll: 0, rollRate: 0, fade: 250, bootArmed: false,
  rideLoggingEnabled: false, riderModelEnabled: false,
  warningPattern: 4, warningBrightness: 100, otaChannel: "testing",
  tokenVersion: 1, token: "simulator-token-1", wifiPassword: "wheeliectrl",
  ssid: "WheelieCtrl-SIM", buildDate: new Date().toISOString()
};

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
  lastPeakAngle: 0, lastPeakG: 0, manualOutput: 0, manualOutputUntil: 0,
  previousState: "NORMAL", previousWarning: false, previousImu: true,
  rideSessions: [], activeRide: null, nextRideId: 1, nextRideSampleMs: 0,
  modelEvents: [], activeModelEvent: null, modelPreEventSamples: [], nextModelEventId: 1,
  nextModelSampleMs: 0, modelStableSamples: 0,
  modelCorrectLabels: 0, modelFalseLabels: 0, modelMissedLabels: 0
};

const RIDE_SAMPLE_INTERVAL_MS = 200;
const RIDE_MAX_DURATION_MS = 90 * 60 * 1000;
const RIDE_MAX_SAMPLES = RIDE_MAX_DURATION_MS / RIDE_SAMPLE_INTERVAL_MS;
const RIDE_MAX_SESSIONS = 3;
const MODEL_SAMPLE_INTERVAL_MS = 20;
const MODEL_EVENT_LIMIT = 12;
const MODEL_PRE_EVENT_LIMIT = 100;

function modelAccelerationTrust() { return clamp(1 - Math.abs(sim.g) / .2, 0, 1); }
function modelEffectiveFreezeRate() { return readSettings().freezeRate; }
function modelBaselineState() {
  if (readSettings().angleMode !== "adaptive") return "TRACKING";
  if (!sim.imu) return "HOLD: IMU";
  if (sim.mode !== "ARMED" || sim.state !== "NORMAL") return "HOLD: STATE";
  if (Math.abs(sim.gyro) >= modelEffectiveFreezeRate()) return "HOLD: MOTION";
  if (modelAccelerationTrust() < .15) return "HOLD: ACCEL";
  return "TRACKING";
}

function beginSimulatorModelEvent() {
  const preceding = sim.modelPreEventSamples;
  const first = preceding[0];
  sim.activeModelEvent = {
    id: sim.nextModelEventId++, rideSessionId: sim.activeRide?.id || 0,
    startedMs: first?.timeMs ?? sim.timeMs,
    startPitch: first?.pitch ?? sim.triggerPitch, endPitch: first?.pitch ?? sim.triggerPitch,
    peakPitch: first?.pitch ?? sim.triggerPitch, peakPitchRate: 0, sumRateSq: 0,
    integratedPositiveRate: 0, peakG: 0, sumGSq: 0, peakAbsRoll: 0,
    samples: 0, aboveTriggerSamples: 0, frozenSamples: 0, outcome: "cancelled"
  };
  preceding.forEach(sample => accumulateSimulatorModelSample(sim.activeModelEvent, sample));
}

function accumulateSimulatorModelSample(event, sample) {
  const settings = readSettings();
  event.samples++;
  event.endPitch = sample.pitch;
  event.peakPitch = Math.max(event.peakPitch, sample.pitch);
  event.peakPitchRate = Math.max(event.peakPitchRate, sample.pitchRate);
  event.sumRateSq += sample.pitchRate * sample.pitchRate;
  event.integratedPositiveRate += Math.max(0, sample.pitchRate) * MODEL_SAMPLE_INTERVAL_MS / 1000;
  event.peakG = Math.max(event.peakG, sample.gLoad);
  event.sumGSq += sample.gLoad * sample.gLoad;
  event.peakAbsRoll = Math.max(event.peakAbsRoll, Math.abs(sample.roll));
  if (sample.pitch >= settings.triggerAngle) event.aboveTriggerSamples++;
  if (sample.baselineFrozen) event.frozenSamples++;
}

function currentSimulatorModelSample() {
  return {
    timeMs: sim.timeMs, pitch: sim.triggerPitch, pitchRate: sim.gyro,
    gLoad: sim.g, roll: device.roll, baselineFrozen: sim.baselineFrozen
  };
}

function sampleSimulatorModelEvent(sample = currentSimulatorModelSample()) {
  const event = sim.activeModelEvent;
  if (!event) return;
  accumulateSimulatorModelSample(event, sample);
}

function finishSimulatorModelEvent(outcome = "cancelled", label = "unlabeled") {
  const event = sim.activeModelEvent;
  if (!event) return null;
  event.outcome = outcome;
  const feature = {
    id: event.id, rideSessionId: event.rideSessionId, outcome, label,
    durationMs: Math.max(0, sim.timeMs - event.startedMs), samples: event.samples,
    aboveTriggerSamples: event.aboveTriggerSamples,
    startPitch: event.startPitch, endPitch: event.endPitch,
    peakPitch: event.peakPitch, pitchRise: event.peakPitch - event.startPitch,
    peakPitchRate: event.peakPitchRate,
    rmsPitchRate: Math.sqrt(event.sumRateSq / Math.max(1, event.samples)),
    integratedPositiveRate: event.integratedPositiveRate, peakG: event.peakG,
    rmsG: Math.sqrt(event.sumGSq / Math.max(1, event.samples)),
    peakAbsRoll: event.peakAbsRoll,
    frozenFraction: event.frozenSamples / Math.max(1, event.samples)
  };
  const above = feature.aboveTriggerSamples / Math.max(1, feature.samples);
  const z = -3 + .1 * clamp(feature.pitchRise, 0, 45) +
    .025 * clamp(feature.peakPitchRate, 0, 120) +
    .018 * clamp(feature.integratedPositiveRate, 0, 80) + .8 * above -
    .55 * clamp(feature.peakG, 0, 4) - .35 * feature.frozenFraction;
  feature.shadowScore = 1 / (1 + Math.exp(-z));
  sim.modelEvents.unshift(feature);
  sim.modelEvents.length = Math.min(sim.modelEvents.length, MODEL_EVENT_LIMIT);
  sim.activeModelEvent = null;
  return feature;
}

function startRideSession() {
  if (!device.rideLoggingEnabled || sim.activeRide) return;
  const ride = {
    id: sim.nextRideId++, startMs: sim.timeMs, durationMs: 0, samples: [],
    wheeliesAtStart: sim.completedWheelies, wheelies: 0, peakPitch: 0, peakRoll: 0,
    peakGyro: 0, peakG: 0, active: true, complete: false, capacityReached: false,
    sha256: "pending"
  };
  sim.rideSessions.unshift(ride);
  while (sim.rideSessions.length > RIDE_MAX_SESSIONS) sim.rideSessions.pop();
  sim.activeRide = ride;
  sim.nextRideSampleMs = sim.timeMs;
  logEvent(`Ride logging started · session ${ride.id}`, "good");
}

function finishRideSession(capacityReached = false) {
  const ride = sim.activeRide;
  if (!ride) return;
  ride.durationMs = Math.min(RIDE_MAX_DURATION_MS, Math.max(0, sim.timeMs - ride.startMs));
  ride.wheelies = sim.completedWheelies - ride.wheeliesAtStart;
  ride.active = false; ride.complete = true; ride.capacityReached = capacityReached;
  sim.activeRide = null;
  void ensureRideDigest(ride);
  logEvent(`Ride ${ride.id} saved · ${ride.samples.length.toLocaleString()} samples${capacityReached ? " · 90 min limit" : ""}`, "good");
}

function rideSampleLine(sample) {
  return [sample.elapsedMs, sample.pitch.toFixed(2), sample.rawPitch.toFixed(2),
    sample.roll.toFixed(2), sample.gyro.toFixed(2), sample.g.toFixed(3),
    sample.baseline.toFixed(2), sample.output, sample.imu ? 1 : 0, sample.state,
    sample.warning ? 1 : 0, sample.baselineFrozen ? 1 : 0].join(",");
}

async function ensureRideDigest(ride) {
  if (ride.sha256 !== "pending") return ride.sha256;
  if (!globalThis.crypto?.subtle) {
    ride.sha256 = "unavailable-in-this-browser";
    return ride.sha256;
  }
  const data = new TextEncoder().encode(ride.samples.map(rideSampleLine).join("\n"));
  const hash = await crypto.subtle.digest("SHA-256", data);
  ride.sha256 = Array.from(new Uint8Array(hash), byte => byte.toString(16).padStart(2, "0")).join("");
  return ride.sha256;
}

function updateRideLogging() {
  const ride = sim.activeRide;
  if (!ride || sim.timeMs < sim.nextRideSampleMs) return;
  sim.nextRideSampleMs += RIDE_SAMPLE_INTERVAL_MS;
  if (ride.samples.length >= RIDE_MAX_SAMPLES) {
    finishRideSession(true);
    return;
  }
  const sample = {
    elapsedMs: Math.min(RIDE_MAX_DURATION_MS, sim.timeMs - ride.startMs),
    pitch: sim.triggerPitch, rawPitch: sim.absolutePitch, roll: device.roll,
    gyro: sim.gyro, g: sim.g, baseline: sim.baseline,
    output: Math.round(sim.output / 2.55), imu: sim.imu, state: sim.state,
    warning: sim.warning, baselineFrozen: sim.baselineFrozen
  };
  ride.samples.push(sample);
  ride.durationMs = sample.elapsedMs;
  ride.wheelies = sim.completedWheelies - ride.wheeliesAtStart;
  ride.peakPitch = Math.max(ride.peakPitch, sample.pitch);
  ride.peakRoll = Math.max(ride.peakRoll, Math.abs(sample.roll));
  ride.peakGyro = Math.max(ride.peakGyro, Math.abs(sample.gyro));
  ride.peakG = Math.max(ride.peakG, sample.g);
}

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
  if (sim.mode === "ARMED" && mode !== "ARMED") finishRideSession();
  sim.mode = mode; sim.state = "NORMAL"; sim.warning = false; sim.output = 0;
  sim.triggerStart = 0; sim.wheelieStart = 0; sim.manualOutput = 0; sim.manualOutputUntil = 0;
  if (mode === "ARMED" && readSettings().angleMode === "adaptive") {
    sim.baseline = sim.absolutePitch; sim.triggerPitch = 0; sim.baselineFrozen = false;
  }
  if (mode === "ARMED") startRideSession();
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
    const trust = modelAccelerationTrust();
    const canTrack = sim.imu && allowsTracking &&
      Math.abs(sim.gyro) < modelEffectiveFreezeRate() && trust >= .15;
    sim.baselineFrozen = !canTrack;
    if (canTrack) {
      const alpha = dt / (Math.max(s.adaptiveTau, .5) + dt) * trust;
      sim.baseline += (sim.absolutePitch - sim.baseline) * clamp(alpha, 0, 1);
    }
    sim.triggerPitch = sim.absolutePitch - sim.baseline;
  }
  sim.highestAngle = Math.max(sim.highestAngle, sim.triggerPitch);
  sim.highestG = Math.max(sim.highestG, sim.g);

  const priorState = sim.state;
  if (sim.mode !== "ARMED" || !sim.imu) {
    sim.state = "NORMAL"; sim.warning = false;
    sim.output = sim.mode === "STANDBY" && sim.manualOutputUntil > sim.timeMs ? sim.manualOutput : 0;
    if (sim.manualOutputUntil && sim.manualOutputUntil <= sim.timeMs) {
      sim.manualOutput = 0; sim.manualOutputUntil = 0;
    }
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
    const warningPattern = patternNames[device.warningPattern] || "off";
    sim.output = sim.warning && warningPattern !== "off"
      ? patternBrightness(warningPattern, Math.round(device.warningBrightness * 2.55), sim.timeMs)
      : outputOn ? patternBrightness(s.wheeliePattern, maxPwm, sim.timeMs) : 0;
  }

  if (priorState !== "WHEELIE" && sim.state === "WHEELIE") {
    sim.activePeak = sim.triggerPitch; sim.activePeakG = sim.g;
  }
  if (sim.state === "WHEELIE") {
    sim.activePeak = Math.max(sim.activePeak, sim.triggerPitch);
    sim.activePeakG = Math.max(sim.activePeakG || 0, sim.g);
  }
  if (priorState === "WHEELIE" && sim.state !== "WHEELIE") {
    sim.completedWheelies++; sim.lastDuration = sim.timeMs - sim.wheelieStart;
    sim.lastPeakAngle = sim.activePeak; sim.lastPeakG = sim.activePeakG || 0;
  }
  if (device.riderModelEnabled && priorState === "NORMAL" && sim.state === "PENDING") beginSimulatorModelEvent();
  if (priorState === "PENDING" && sim.state === "WHEELIE" && sim.activeModelEvent) {
    sim.activeModelEvent.outcome = "detected";
  }
  if (priorState === "PENDING" && sim.state === "NORMAL") finishSimulatorModelEvent("cancelled");
  if (priorState === "WHEELIE" && sim.state === "NORMAL") finishSimulatorModelEvent("detected");
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
    const nextRoll = Number.isFinite(sample.roll) ? sample.roll : 8 * Math.sin(sim.scenarioTime * .8);
    device.rollRate = (nextRoll - device.roll) / Math.max(dt, .001); device.roll = nextRoll;
    $("imuHealthy").checked = sim.imu;
    syncInputControls();
  } else {
    sim.absolutePitch = Number($("pitchInput").value);
    const nextRoll = Number($("rollInput").value);
    device.rollRate = (nextRoll - device.roll) / Math.max(dt, .001); device.roll = nextRoll;
    sim.gyro = Number($("gyroInput").value);
    sim.g = Number($("gInput").value);
    sim.imu = $("imuHealthy").checked;
  }
  controllerStep(dt);
  if (device.riderModelEnabled && sim.timeMs >= sim.nextModelSampleMs) {
    sim.nextModelSampleMs = sim.timeMs + MODEL_SAMPLE_INTERVAL_MS;
    if (sim.mode === "ARMED" && sim.state === "NORMAL" && sim.imu &&
        modelAccelerationTrust() >= .8 && Math.abs(sim.gyro) < 2) {
      sim.modelStableSamples++;
    }
    const modelSample = currentSimulatorModelSample();
    sampleSimulatorModelEvent(modelSample);
    sim.modelPreEventSamples.push(modelSample);
    if (sim.modelPreEventSamples.length > MODEL_PRE_EVENT_LIMIT) {
      sim.modelPreEventSamples.shift();
    }
  }
  updateRideLogging();
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
  finishRideSession();
  const restartRide = sim.mode === "ARMED" && device.rideLoggingEnabled;
  Object.assign(sim, { timeMs: 0, scenarioTime: 0, accumulator: 0, state: "NORMAL", warning: false,
    output: 0, triggerStart: 0, wheelieStart: 0, baseline: 0, triggerPitch: 0,
    baselineFrozen: false, completedWheelies: 0, lastDuration: 0, activePeak: 0,
    highestAngle: 0, highestG: 0, lastPeakAngle: 0, lastPeakG: 0,
    manualOutput: 0, manualOutputUntil: 0,
    previousState: "NORMAL", previousWarning: false, previousImu: sim.imu });
  device.roll = 0; device.rollRate = 0; $("rollInput").value = 0;
  if (restartRide) startRideSession();
  logEvent("Simulation reset");
}

function syncInputControls() {
  $("pitchInput").value = clamp(sim.absolutePitch, -20, 75);
  $("rollInput").value = clamp(device.roll, -55, 55);
  $("gyroInput").value = clamp(sim.gyro, -120, 120);
  $("gInput").value = clamp(sim.g, 0, 2.5);
}

function oledRows() {
  const s = readSettings();
  if (sim.mode === "STANDBY") return ["    STANDBY", "----------------", "OUTPUT: OFF", "", `PITCH:${fmtSigned(sim.triggerPitch)}`, "", "Hold 1.5s ARM", "30s WiFi reset"];
  if (sim.page === 0) return ["WHEELIE CTRL 1/4", "MODE: ARMED", `PITCH:${fmtSigned(sim.triggerPitch)} deg`, `STATE:${sim.state}`, `OUTPUT:${Math.round(sim.output/2.55)}%`, `ANGLE:${s.angleMode === "adaptive" ? "ADAPT" : "ABS"}`, "1xPg 2xAng 3xAP", "Hold=Arm 30sPwd"];
  if (sim.page === 1) return ["SETTINGS     2/4", "----------------", `TRIG:${s.triggerAngle.toFixed(1)} deg`, `RESET:${s.resetAngle.toFixed(1)} deg`, `MODE:${s.angleMode === "adaptive" ? "ADAPT" : "ABS"}`, s.angleMode === "adaptive" ? `TAU:${s.adaptiveTau.toFixed(1)}s` : `HOLD:${s.triggerHold}ms`, s.angleMode === "adaptive" ? `FREEZE:${s.freezeRate.toFixed(1)}/s` : `MIN:${s.minimumOn}ms`, "2x btn = mode"];
  if (sim.page === 2) return ["NETWORK      3/4", "----------------", `AP: ${sim.apEnabled ? "ON" : "OFF"}`, sim.apEnabled ? "wheelie.local" : "WiFi disabled", sim.apEnabled ? "192.168.4.1" : "", "CLIENTS:0", `mDNS:${sim.apEnabled ? "OK" : "--"} DNS:${sim.apEnabled ? "OK" : "--"}`, "3xAP 30sPwdRst"];
  return ["DIAGNOSTICS  4/4", `RAW:${fmtSigned(sim.absolutePitch)}`, `BASE:${fmtSigned(sim.baseline)}`, `TRIG:${fmtSigned(sim.triggerPitch)}`, `GYRO:${fmtSigned(sim.gyro)}/s`, `+G:${sim.g.toFixed(2)}g`, `BASE:${modelBaselineState().replace("HOLD: ", "")}`, `IMU:${sim.imu ? "OK" : "FAULT"}`];
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
  $("rollOut").textContent = `${device.roll.toFixed(1)}°`;
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

function rotateWriteToken() {
  device.tokenVersion += 1;
  device.token = `simulator-token-${device.tokenVersion}`;
}

function phoneStatusSnapshot() {
  const settings = readSettings();
  return {
    pitch: sim.triggerPitch, rawPitch: sim.absolutePitch,
    roll: device.roll, rollRate: device.rollRate,
    baseline: sim.baseline, gyroRate: sim.gyro, filteredGyroRate: sim.gyro, gLoad: sim.g,
    accelMagnitude: 1 + sim.g, accelTrust: modelAccelerationTrust(),
    peakAngle: sim.highestAngle, peakG: sim.highestG,
    warningActive: sim.warning, warningAngle: settings.warningAngle,
    warningReset: settings.warningReset, warningRate: settings.warningRate,
    wheeliePattern: patternValues[settings.wheeliePattern],
    warningPattern: device.warningPattern, warningBrightness: device.warningBrightness,
    eventCount: sim.completedWheelies,
    activeDuration: sim.state === "WHEELIE" ? sim.timeMs - sim.wheelieStart : 0,
    lastDuration: sim.lastDuration, lastPeakAngle: sim.lastPeakAngle, lastPeakG: sim.lastPeakG,
    accelX: 0, accelY: 0, accelZ: 1 + sim.g,
    baselineFrozen: sim.baselineFrozen, baselineState: modelBaselineState(),
    mode: sim.mode, state: sim.state,
    output: Math.round(sim.output / 2.55), imu: sim.imu, angleMode: settings.angleMode,
    rotationAxis: device.rotationAxis, rollAxis: device.rollAxis,
    verticalAxis: device.verticalAxis, orientationConfigured: device.orientationConfigured,
    adaptiveTau: settings.adaptiveTau, freezeRate: settings.freezeRate,
    effectiveFreezeRate: modelEffectiveFreezeRate(),
    trigger: settings.triggerAngle, reset: settings.resetAngle,
    hold: settings.triggerHold, minon: settings.minimumOn,
    brightness: settings.brightness, fade: device.fade, bootArmed: device.bootArmed,
    apEnabled: sim.apEnabled, ssid: device.ssid, mdns: sim.apEnabled,
    dns: sim.apEnabled, clients: 1, uptime: Math.floor(sim.timeMs / 1000),
    firmware: "desktop-simulator", board: "seeed_xiao_esp32s3", chip: "esp32s3",
    buildCommit: "working-tree", buildDate: device.buildDate,
    releaseChannel: "testing", otaChannel: device.otaChannel, signedOta: true,
    rideLoggingEnabled: device.rideLoggingEnabled, rideLoggingAvailable: true,
    rideLoggingActive: Boolean(sim.activeRide), rideSessionId: sim.activeRide?.id || 0,
    rideSampleCount: sim.activeRide?.samples.length || 0,
    rideStorageUsed: sim.rideSessions.reduce((total, ride) => total + 192 + ride.samples.length * 16, 0),
    rideStorageTotal: 1572864, rideSessionLimit: RIDE_MAX_SESSIONS, rideSampleRateHz: 5,
    calOneGRaw: 1, calAccelRms: 0.004, calGyroRms: 0.18, calHighVibration: false,
    modelStableSamples: sim.modelStableSamples, modelGyroRms: .18,
    riderModelEnabled: device.riderModelEnabled,
    modelAccelRms: .004, modelCorrectLabels: sim.modelCorrectLabels,
    modelFalseLabels: sim.modelFalseLabels, modelMissedLabels: sim.modelMissedLabels,
    modelConfidence: Math.min(1, sim.modelStableSamples / 3000) *
      Math.min(1, (sim.modelCorrectLabels + sim.modelFalseLabels + sim.modelMissedLabels) / 10),
    modelEventCount: sim.modelEvents.length,
    modelLastEventId: sim.modelEvents[0]?.id || 0,
    modelLastScore: sim.modelEvents[0]?.shadowScore || 0,
    token: device.token
  };
}

function bridgeResponse(body, status = 200, contentType = "text/plain") {
  return { body: typeof body === "string" ? body : JSON.stringify(body), status, contentType };
}

function bridgeParams(body) {
  if (!body) return new URLSearchParams();
  return new URLSearchParams(typeof body === "string" ? body : String(body));
}

function validateBridgeToken(url) {
  return url.searchParams.get("token") === device.token;
}

function rideSummary(ride) {
  return {
    id: ride.id, active: ride.active, complete: ride.complete, recovered: false,
    capacityReached: ride.capacityReached, durationMs: ride.durationMs,
    samples: ride.samples.length, wheelies: ride.wheelies,
    peakPitch: ride.peakPitch, peakRoll: ride.peakRoll, peakGyro: ride.peakGyro,
    peakG: ride.peakG, firmware: "desktop-simulator", commit: "working-tree",
    channel: "testing", sha256: ride.sha256
  };
}

function rideListResponse() {
  const usedBytes = sim.rideSessions.reduce((total, ride) => total + 192 + ride.samples.length * 16, 0);
  return {
    available: true, enabled: device.rideLoggingEnabled, active: Boolean(sim.activeRide),
    maxSessions: RIDE_MAX_SESSIONS, sampleRateHz: 5, maxSessionMinutes: 90,
    usedBytes, totalBytes: 1572864, sessions: sim.rideSessions.map(rideSummary)
  };
}

function riderModelResponse() {
  const labels = sim.modelCorrectLabels + sim.modelFalseLabels + sim.modelMissedLabels;
  return {
    format: "wheelie-rider-model", version: 1, enabled: device.riderModelEnabled,
    firmware: "desktop-simulator",
    sampleRateHz: 50, stableSamples: sim.modelStableSamples, gyroRms: .18,
    accelResidualRms: .004, effectiveFreezeRate: modelEffectiveFreezeRate(),
    recommendedFreezeRate: modelEffectiveFreezeRate(),
    correctLabels: sim.modelCorrectLabels, falseLabels: sim.modelFalseLabels,
    missedLabels: sim.modelMissedLabels,
    confidence: Math.min(1, sim.modelStableSamples / 3000) * Math.min(1, labels / 10),
    events: [...sim.modelEvents].reverse()
  };
}

function simulatorModelCsv() {
  const heading = "firmware,commit,ride_session_id,event_id,outcome,label,duration_ms,samples,above_trigger_samples,start_pitch,end_pitch,peak_pitch,pitch_rise,peak_pitch_rate,rms_pitch_rate,integrated_positive_rate,peak_g,rms_g,peak_abs_roll,frozen_fraction,shadow_score";
  const rows = sim.modelEvents.map(event => [
    "desktop-simulator", "working-tree", event.rideSessionId, event.id,
    event.outcome, event.label, event.durationMs, event.samples,
    event.aboveTriggerSamples, event.startPitch, event.endPitch, event.peakPitch,
    event.pitchRise, event.peakPitchRate, event.rmsPitchRate,
    event.integratedPositiveRate, event.peakG, event.rmsG,
    event.peakAbsRoll, event.frozenFraction, event.shadowScore
  ].join(","));
  return [heading, ...rows].join("\r\n") + "\r\n";
}

function requestedSimulatorRide(url) {
  const id = Number(url.searchParams.get("id"));
  return sim.rideSessions.find(ride => ride.id === id) || null;
}

async function simulatorRideCsv(ride) {
  const digest = await ensureRideDigest(ride);
  return [
    "# Wheelie Controller Simulator Ride Telemetry",
    "# provenance=SIMULATOR-RECORDED; certification=none; integrity=SHA-256 CSV checksum",
    `# session_id=${ride.id}; firmware=desktop-simulator; commit=working-tree; channel=testing`,
    `# sample_sha256=${digest}`,
    "elapsed_ms,pitch_deg,raw_pitch_deg,roll_deg,pitch_rate_dps,g_load,baseline_deg,output_percent,imu_ok,state,warning,baseline_frozen",
    ...ride.samples.map(rideSampleLine)
  ].join("\r\n") + "\r\n";
}

async function simulatorRideReport(ride) {
  const digest = await ensureRideDigest(ride);
  const duration = (ride.durationMs / 1000).toFixed(1);
  return `<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1"><title>Simulator Ride Report #${ride.id}</title><style>body{margin:0;background:#080b10;color:#f4f7fb;font:15px system-ui,-apple-system,sans-serif}.wrap{width:min(760px,100%);margin:auto;padding:28px}.top{display:flex;justify-content:space-between;gap:20px;align-items:start}.kicker{color:#55e6ff;font-size:12px;letter-spacing:.16em}.badge{display:inline-grid;place-items:center;text-align:center;padding:12px 16px;border:2px solid #52df9a;border-radius:14px;color:#8dffc7;background:#102a21;font-weight:900;letter-spacing:.08em}.badge small{font-size:9px;color:#a9c6b8}.card{margin-top:18px;padding:18px;border:1px solid #273449;border-radius:16px;background:#111720}.grid{display:grid;grid-template-columns:repeat(2,1fr);gap:0 22px}.row{display:flex;justify-content:space-between;gap:12px;padding:10px 0;border-bottom:1px solid #273449}.row span,.note{color:#8f9bad}.hash{overflow-wrap:anywhere;font:12px ui-monospace,monospace}@media(max-width:560px){.top{display:block}.badge{margin-top:16px}.grid{grid-template-columns:1fr}}</style></head><body><main class="wrap"><div class="top"><div><div class="kicker">WHEELIE CONTROLLER LAB</div><h1>Simulator Ride Report #${ride.id}</h1><p>Emulated sensor telemetry captured at 5 Hz.</p></div><div class="badge">SIMULATOR-RECORDED<br><small>SENSOR DATA · SHA-256</small></div></div><section class="card"><h2>Ride summary</h2><div class="grid"><div class="row"><span>Duration</span><b>${duration} s</b></div><div class="row"><span>Samples</span><b>${ride.samples.length}</b></div><div class="row"><span>Wheelies</span><b>${ride.wheelies}</b></div><div class="row"><span>Peak pitch</span><b>${ride.peakPitch.toFixed(1)}°</b></div><div class="row"><span>Peak lean</span><b>${ride.peakRoll.toFixed(1)}°</b></div><div class="row"><span>Peak +G</span><b>${ride.peakG.toFixed(2)} g</b></div></div></section><section class="card"><h2>Recorder provenance</h2><p>Desktop simulator · testing · working-tree</p><div class="hash">${digest}</div></section><p class="note"><b>Simulation only:</b> this report was generated by the desktop emulator, not physical controller hardware. The checksum detects changes to the simulated sample stream; it is not certification or a device signature.</p></main></body></html>`;
}

function applyPhoneSettings(params) {
  const setValue = (id, value) => { if ($(id) && value !== null) $(id).value = value; };
  const pattern = Number(params.get("wheeliePattern"));
  setValue("angleMode", params.get("angleMode"));
  setValue("adaptiveTau", params.get("adaptiveTau"));
  setValue("freezeRate", params.get("freezeRate"));
  setValue("triggerAngle", params.get("trigger"));
  setValue("resetAngle", params.get("reset"));
  setValue("triggerHold", params.get("hold"));
  setValue("minimumOn", params.get("minon"));
  setValue("brightness", params.get("brightness"));
  if (Number.isInteger(pattern) && pattern >= 0 && pattern < patternNames.length) {
    setValue("wheeliePattern", patternNames[pattern]);
  }
  device.fade = clamp(Number(params.get("fade")) || 0, 0, 3000);
  device.bootArmed = params.get("bootMode") === "armed";
  device.warningPattern = clamp(Number(params.get("warningPattern")) || 0, 0, 4);
  device.warningBrightness = clamp(Number(params.get("warningBrightness")) || 100, 1, 100);
  setValue("warningAngle", params.get("warningAngle"));
  setValue("warningReset", params.get("warningReset"));
  setValue("warningRate", params.get("warningRate"));
  device.otaChannel = params.get("otaChannel") === "stable" ? "stable" : "testing";
  const wasRideLoggingEnabled = device.rideLoggingEnabled;
  device.rideLoggingEnabled = params.get("rideLogging") === "enabled";
  const wasRiderModelEnabled = device.riderModelEnabled;
  device.riderModelEnabled = params.get("riderModel") === "enabled";
  if (wasRiderModelEnabled && !device.riderModelEnabled) sim.activeModelEvent = null;
  if (!wasRiderModelEnabled && device.riderModelEnabled) sim.modelPreEventSamples = [];
  if (wasRideLoggingEnabled && !device.rideLoggingEnabled) finishRideSession();
  else if (!wasRideLoggingEnabled && device.rideLoggingEnabled && sim.mode === "ARMED") startRideSession();
  setAngleMode($("angleMode").value);
  rotateWriteToken();
  logEvent("Phone: settings saved", "good");
}

async function handlePhoneRequest(rawUrl, options = {}) {
  const url = new URL(rawUrl, window.location.origin);
  const method = String(options.method || "GET").toUpperCase();
  if (url.pathname === "/api/status" && method === "GET") {
    return bridgeResponse(phoneStatusSnapshot(), 200, "application/json");
  }
  if (url.pathname === "/api/rides" && method === "GET") {
    return bridgeResponse(rideListResponse(), 200, "application/json");
  }
  if (url.pathname === "/api/model" && method === "GET") {
    return bridgeResponse(riderModelResponse(), 200, "application/json");
  }
  if (url.pathname === "/api/model/events.csv" && method === "GET") {
    return bridgeResponse(simulatorModelCsv(), 200, "text/csv; charset=utf-8");
  }
  if ((url.pathname === "/api/ride/csv" || url.pathname === "/api/ride/report") && method === "GET") {
    const ride = requestedSimulatorRide(url);
    if (!ride) return bridgeResponse("Ride session not found", 404);
    if (ride.active) return bridgeResponse("Finish the ride before downloading its report", 409);
    if (url.pathname.endsWith("/csv")) return bridgeResponse(await simulatorRideCsv(ride), 200, "text/csv; charset=utf-8");
    return bridgeResponse(await simulatorRideReport(ride), 200, "text/html; charset=utf-8");
  }
  if (method !== "POST") return bridgeResponse("Method not allowed", 405);
  if (!validateBridgeToken(url)) return bridgeResponse("Invalid write token", 403);

  if (url.pathname === "/api/mode") {
    const requested = url.searchParams.get("mode");
    if (requested === "armed" && !sim.imu) return bridgeResponse("Cannot arm: IMU fault", 409);
    if (requested !== "armed" && requested !== "standby") return bridgeResponse("Invalid mode", 400);
    setControllerMode(requested.toUpperCase());
    return bridgeResponse(`Controller ${requested.toUpperCase()}`);
  }
  if (url.pathname === "/api/peak/reset") {
    const kind = url.searchParams.get("kind");
    if (kind === "angle") sim.highestAngle = Math.max(0, sim.triggerPitch);
    else if (kind === "g") sim.highestG = sim.g;
    else return bridgeResponse("Invalid peak type", 400);
    logEvent(`Phone: ${kind} peak reset`);
    return bridgeResponse(`Highest ${kind === "g" ? "+G" : "angle"} reset`);
  }
  if (url.pathname === "/api/model/feedback") {
    const label = url.searchParams.get("label");
    if (label === "missed") {
      if (!device.riderModelEnabled) return bridgeResponse("Enable Rider Model Lab in Settings first", 409);
      beginSimulatorModelEvent();
      sampleSimulatorModelEvent();
      const event = finishSimulatorModelEvent("missed", "missed");
      if (event) sim.modelMissedLabels++;
      rotateWriteToken();
      return bridgeResponse("Missed event saved for model shaping");
    }
    const event = sim.modelEvents.find(item => item.id === Number(url.searchParams.get("id")));
    if (!event) return bridgeResponse("Model event not found", 404);
    if (event.label !== "unlabeled") return bridgeResponse("This event already has rider feedback", 409);
    if (label !== "correct" && label !== "false") return bridgeResponse("Invalid feedback label", 400);
    event.label = label;
    if (label === "correct") sim.modelCorrectLabels++; else sim.modelFalseLabels++;
    rotateWriteToken();
    return bridgeResponse("Rider feedback saved");
  }
  if (url.pathname === "/api/model/reset") {
    sim.modelEvents = []; sim.activeModelEvent = null; sim.nextModelEventId = 1;
    sim.modelPreEventSamples = [];
    sim.modelStableSamples = 0; sim.modelCorrectLabels = 0;
    sim.modelFalseLabels = 0; sim.modelMissedLabels = 0;
    rotateWriteToken();
    return bridgeResponse("Rider profile and event labels reset");
  }
  if (url.pathname === "/api/settings") {
    applyPhoneSettings(bridgeParams(options.body));
    return bridgeResponse("Settings saved");
  }
  if (url.pathname === "/api/calibrate") {
    sim.baseline = sim.absolutePitch; sim.triggerPitch = 0;
    logEvent("Phone: calibration complete", "good");
    return bridgeResponse("Calibration complete");
  }
  if (url.pathname === "/api/orientation") {
    setControllerMode("STANDBY");
    device.orientationConfigured = true;
    rotateWriteToken();
    logEvent("Phone: mounting orientation saved", "good");
    return bridgeResponse("Orientation saved and calibration complete");
  }
  if (url.pathname === "/api/output") {
    if (sim.mode !== "STANDBY") return bridgeResponse("Manual output is only allowed in STANDBY", 409);
    const level = clamp(Number(url.searchParams.get("level")) || 0, 0, 100);
    sim.manualOutput = Math.round(level * 2.55);
    sim.manualOutputUntil = level ? sim.timeMs + 10000 : 0;
    sim.output = sim.manualOutput;
    logEvent(`Phone: manual output ${level}%`);
    return bridgeResponse(level ? `Manual output ${level}% for up to 10 seconds` : "Manual output OFF");
  }
  if (url.pathname === "/api/wifi") {
    const params = bridgeParams(options.body);
    if (params.get("generate") === "1") {
      device.wifiPassword = `WCTRL-${Math.random().toString(36).slice(2, 12).toUpperCase()}`;
      rotateWriteToken();
      logEvent("Phone: unique Wi-Fi password generated", "warn");
      return bridgeResponse(`Generated device password: ${device.wifiPassword}\nSimulator AP remains connected.`);
    }
    const password = params.get("password") || "";
    if (password.length < 8 || password.length > 63) return bridgeResponse("Wi-Fi password must be 8-63 characters", 400);
    device.wifiPassword = password; rotateWriteToken();
    logEvent("Phone: Wi-Fi password changed", "warn");
    return bridgeResponse("Password saved. Simulator AP remains connected.");
  }
  if (url.pathname === "/api/update") return bridgeResponse("Signed OTA upload is unavailable in the desktop simulator", 409);
  if (url.pathname === "/api/rollback") return bridgeResponse("No previous simulator firmware image is available", 409);
  return bridgeResponse("Unknown simulator endpoint", 404);
}

window.wheelieSimulatorApi = {
  request: handlePhoneRequest,
  phoneReady(path) {
    $("phoneStatus").textContent = "CONNECTED";
    $("phoneDot").classList.add("connected");
    $("phoneAddress").textContent = path.includes("settings") ? "wheelie.local/settings" : "wheelie.local";
    document.querySelectorAll("[data-phone-page]").forEach((button) => {
      button.classList.toggle("active", button.dataset.phonePage === path || (path === "/phone/settings/" && button.dataset.phonePage === "/phone/settings"));
    });
  }
};

document.querySelectorAll("[data-scenario]").forEach((button) => button.addEventListener("click", () => selectScenario(button.dataset.scenario)));
[$("pitchInput"), $("rollInput"), $("gyroInput"), $("gInput")].forEach((input) => input.addEventListener("input", () => selectScenario("manual")));
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
document.querySelectorAll("[data-phone-page]").forEach((button) => button.addEventListener("click", () => {
  $("phoneStatus").textContent = "CONNECTING";
  $("phoneDot").classList.remove("connected");
  $("phoneFrame").src = button.dataset.phonePage;
}));
$("reloadPhone").addEventListener("click", () => {
  $("phoneStatus").textContent = "CONNECTING";
  $("phoneDot").classList.remove("connected");
  $("phoneFrame").contentWindow.location.reload();
});

logEvent("Desktop simulator initialized", "good");
requestAnimationFrame(frame);
