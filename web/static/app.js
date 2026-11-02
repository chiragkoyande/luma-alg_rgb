/* SPDX-License-Identifier: GPL-2.0 */
"use strict";

const $ = (id) => document.getElementById(id);

const SWATCHES = [
  ["#ffffff", "White"],
  ["#ff3b30", "Red"],
  ["#ff7f00", "Orange"],
  ["#ffd60a", "Yellow"],
  ["#7fff00", "Lime"],
  ["#00ff46", "Green cyan"],
  ["#00ffc8", "Cyan"],
  ["#22d3ee", "Ice"],
  ["#007fc8", "Light blue"],
  ["#2962ff", "Blue"],
  ["#7c4dff", "Violet"],
  ["#ff00c8", "Magenta"],
  ["#ff007f", "Pink"],
  ["#ff7f3f", "Flesh"],
];

const EFFECTS = [
  { id: "none", name: "Static", desc: "Hold one color", icon: "◼" },
  { id: "random", name: "Random", desc: "Random shifts", icon: "✦" },
  { id: "flash", name: "Flash", desc: "Strobe pulse", icon: "⚡" },
  { id: "tempo", name: "Tempo", desc: "Beat sync", icon: "♪" },
  { id: "dance", name: "Dance", desc: "Moving pattern", icon: "◈" },
  { id: "wave", name: "Wave", desc: "Sweep across", icon: "≋" },
  { id: "cycle", name: "Cycle", desc: "Spectrum loop", icon: "◐" },
  { id: "breath", name: "Breath", desc: "Slow fade", icon: "◒" },
];

const STATIC_PRESETS = [
  { id: "aurora", name: "Aurora", color: "#22d3ee", brightness: 3, effect: "none", zone: "main", tag: "Cool" },
  { id: "ember", name: "Ember", color: "#ff7a45", brightness: 4, effect: "none", zone: "main", tag: "Warm" },
  { id: "ice", name: "Ice", color: "#7dd3fc", brightness: 2, effect: "none", zone: "main", tag: "Soft" },
  { id: "matrix", name: "Matrix", color: "#22c55e", brightness: 3, effect: "none", zone: "main", tag: "Green" },
  { id: "pulse", name: "Pulse", color: "#f472b6", brightness: 3, effect: "breath", zone: "main", tag: "Motion" },
  { id: "disco", name: "Disco", color: "#a855f7", brightness: 4, effect: "dance", zone: "main", tag: "Motion" },
];

const KEY_ROWS = [
  [["esc", 1.35], ["1", 1], ["2", 1], ["3", 1], ["4", 1], ["5", 1], ["6", 1], ["7", 1], ["8", 1], ["9", 1], ["0", 1], ["-", 1], ["=", 1], ["back", 1.9]],
  [["tab", 1.5], ["Q", 1], ["W", 1], ["E", 1], ["R", 1], ["T", 1], ["Y", 1], ["U", 1], ["I", 1], ["O", 1], ["P", 1], ["[", 1], ["]", 1], ["\\", 1.4]],
  [["caps", 1.75], ["A", 1], ["S", 1], ["D", 1], ["F", 1], ["G", 1], ["H", 1], ["J", 1], ["K", 1], ["L", 1], [";", 1], ["'", 1], ["enter", 2.05]],
  [["shift", 2.15], ["Z", 1], ["X", 1], ["C", 1], ["V", 1], ["B", 1], ["N", 1], ["M", 1], [",", 1], [".", 1], ["/", 1], ["shift", 1.75]],
  [["ctrl", 1.35], ["fn", 1.1], ["alt", 1.1], ["space", 5.4], ["alt", 1.1], ["ctrl", 1.35], ["←", 1], ["↑", 1], ["↓", 1], ["→", 1]],
];

const ZONE_LABELS = {
  main: "Main",
  left: "Left",
  mid: "Mid",
  right: "Right",
  bar: "Light bar",
  logo: "Logo",
};

const EFFECT_LABELS = Object.fromEntries(EFFECTS.map((effect) => [effect.id, effect.name]));

const state = {
  color: "#ffffff",
  brightness: 3,
  effect: "none",
  power: true,
  zone: "main",
  activity: [],
  health: null,
  lastProbe: null,
  probing: false,
};

let colorTimer = null;
let brightnessTimer = null;
let mutationId = 0;
let appliedMutation = 0;
let toastTimer = null;

const els = {};

function cacheElements() {
  [
    "conn-pill", "conn-pill-text", "side-status", "side-status-dot", "side-device",
    "hero-effect", "keyboard-deck", "keyboard", "deck-halo", "deck-state", "deck-zone",
    "deck-led", "deck-color", "color-input", "color-picker", "color-picker-face",
    "hex-input", "color-badge", "swatches", "zone-select", "brightness",
    "brightness-value", "power-toggle", "power-desc", "effect-grid", "effect-badge",
    "preset-grid", "save-preset", "save-preset-top", "activity-list", "activity-count",
    "probe-button", "probe-copy", "probe-checks", "probe-json", "probe-time",
    "perkey-badge", "sys-badge", "sys-cli", "sys-module", "sys-device", "sys-acpi",
    "sys-kernel", "sys-version", "sys-caps", "refresh-btn", "preset-dialog",
    "preset-form", "preset-name", "preset-cancel", "toast",
  ].forEach((id) => {
    els[id] = $(id);
  });
}

/* -------------------------------------------------------------------------- */
/* Utilities                                                                  */
/* -------------------------------------------------------------------------- */

function normalizeHex(value) {
  if (typeof value !== "string") return "#ffffff";
  let text = value.trim();
  if (text.startsWith("#")) text = text.slice(1);
  else if (text.toLowerCase().startsWith("0x")) text = text.slice(2);
  if (/^[0-9a-f]{6}$/i.test(text)) return `#${text.toLowerCase()}`;
  return "#ffffff";
}

function isValidHex(value) {
  return /^#[0-9a-f]{6}$/i.test(value);
}

function hexToRgba(hex, alpha) {
  const value = normalizeHex(hex).slice(1);
  const number = Number.parseInt(value, 16);
  const r = (number >> 16) & 255;
  const g = (number >> 8) & 255;
  const b = number & 255;
  return `rgba(${r}, ${g}, ${b}, ${alpha})`;
}

function clamp(value, minimum, maximum) {
  return Math.min(maximum, Math.max(minimum, value));
}

function effectLabel(effect) {
  return EFFECT_LABELS[effect] || "Static";
}

function zoneLabel(zone) {
  return ZONE_LABELS[zone] || "Main";
}

function setConnection(kind, text, detail) {
  const pill = els["conn-pill"];
  pill.className = `pill ${kind}`;
  els["conn-pill-text"].textContent = text;
  els["side-status-dot"].className = `status-dot ${kind}`;
  els["side-status"].textContent = text;
  if (detail) els["side-device"].textContent = detail;
}

function toast(message, kind = "good") {
  const node = els.toast;
  node.textContent = message;
  node.className = `toast show ${kind}`;
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => {
    node.className = "toast";
  }, 3200);
}

async function request(path, options = {}, allowFail = false) {
  const response = await fetch(path, {
    cache: "no-store",
    headers: { "Content-Type": "application/json" },
    ...options,
  });

  let data = null;
  try {
    data = await response.json();
  } catch {
    data = null;
  }

  if (!response.ok) {
    if (allowFail && data) return data;
    throw new Error(data?.error || `Request failed (${response.status})`);
  }

  if (!allowFail && data && data.ok === false) {
    throw new Error(data.error || "Request failed");
  }

  return data;
}

/* -------------------------------------------------------------------------- */
/* State rendering                                                            */
/* -------------------------------------------------------------------------- */

function applyStatus(status) {
  if (!status || status.ok === false) return;

  state.color = normalizeHex(status.color);
  const brightness = Number.parseInt(status.brightness, 10);
  state.brightness = Number.isFinite(brightness) ? clamp(brightness, 0, 4) : 3;
  state.effect = typeof status.effect === "string" ? status.effect : "none";
  state.power = status.power === "on";
  state.zone = status.zone_name && status.zone_name !== "unknown" ? status.zone_name : "main";

  renderAll();
}

function renderAll() {
  renderColor();
  renderBrightness();
  renderEffect();
  renderZone();
  renderPower();
  renderKeyboardColors();
}

function renderColor() {
  const color = state.color;
  els["color-input"].value = color;
  els["hex-input"].value = color.slice(1).toUpperCase();
  els["color-badge"].textContent = color.toUpperCase();
  els["deck-color"].textContent = color.toUpperCase();
  els["color-picker-face"].style.setProperty("--pick", color);
  els["color-picker"].style.background = color;

  document.querySelectorAll(".swatch").forEach((swatch) => {
    swatch.classList.toggle("active", swatch.dataset.hex === color);
  });
}

function renderBrightness() {
  const level = clamp(state.brightness, 0, 4);
  els.brightness.value = String(level);
  els["brightness-value"].textContent = `${level} / 4`;
  els.brightness.style.setProperty("--range", `${(level / 4) * 100}%`);
}

function renderEffect() {
  const active = state.effect || "none";
  document.querySelectorAll(".effect-card").forEach((card) => {
    card.classList.toggle("active", card.dataset.effect === active);
  });
  els["effect-badge"].textContent = effectLabel(active);
  els["hero-effect"].textContent = effectLabel(active);
}

function renderZone() {
  const zone = state.zone in ZONE_LABELS ? state.zone : "main";
  els["zone-select"].value = zone;
}

function renderPower() {
  const on = state.power;
  els["power-toggle"].classList.toggle("on", on);
  els["power-toggle"].setAttribute("aria-checked", String(on));
  els["power-desc"].textContent = on
    ? "The keyboard backlight is on."
    : "The keyboard backlight is off.";
}

function renderKeyboardColors() {
  const color = state.color;
  const intensity = state.power ? state.brightness / 4 : 0;
  const dim = state.power ? 0.035 + intensity * 0.22 : 0.015;
  const glow = state.power ? 0.025 + intensity * 0.22 : 0;

  const deck = els["keyboard-deck"];
  deck.style.setProperty("--kb-color", color);
  deck.style.setProperty("--kb-dim", hexToRgba(color, dim));
  deck.style.setProperty("--kb-glow", hexToRgba(color, glow));
  els["deck-halo"].style.background = hexToRgba(color, state.power ? 0.08 + intensity * 0.2 : 0.03);

  els["deck-state"].textContent = state.power ? "LIVE" : "STANDBY";
  els["deck-zone"].textContent = `${zoneLabel(state.zone).toUpperCase()} ZONE`;
  els["deck-led"].style.background = state.power ? color : "#5f6a80";
  els["deck-led"].style.boxShadow = state.power
    ? `0 0 10px ${hexToRgba(color, 0.9)}`
    : "none";
}

/* -------------------------------------------------------------------------- */
/* Health / system                                                            */
/* -------------------------------------------------------------------------- */

function renderHealth(health) {
  if (!health) return;
  state.health = health;

  const ready =
    health.cli.found &&
    health.acpi.present &&
    health.module.present &&
    health.device.accessible;

  if (ready) {
    setConnection("ok", "Connected", `${health.device.path} · ready`);
  } else if (health.device.present && !health.device.accessible) {
    setConnection("warn", "Permission needed", `${health.device.path} · no access`);
  } else if (!health.cli.found) {
    setConnection("bad", "CLI missing", "install alg-rgb");
  } else {
    setConnection("bad", "Hardware offline", health.device.path);
  }

  els["sys-cli"].textContent = health.cli.found ? health.cli.path : "not found";
  els["sys-module"].textContent = health.module.present
    ? `loaded · ${health.module.version || "unknown"}`
    : "not loaded";
  els["sys-device"].textContent = health.device.present
    ? health.device.accessible
      ? "ready · read/write"
      : "present · no access"
    : "missing";
  els["sys-acpi"].textContent = health.acpi.present
    ? `CLV0001 · status ${health.acpi.status || "?"}`
    : "missing";
  els["sys-kernel"].textContent = health.kernel || "—";
  els["sys-version"].textContent = health.app_version || "—";

  const healthy =
    health.cli.found &&
    health.acpi.present &&
    health.module.present &&
    health.device.accessible;
  const attention =
    health.cli.found &&
    health.acpi.present &&
    (health.module.present || health.device.present);

  els["sys-badge"].className = `badge ${healthy ? "good" : attention ? "warn" : "bad"}`;
  els["sys-badge"].textContent = healthy ? "Healthy" : attention ? "Attention" : "Offline";

  const perkeyBadge = els["perkey-badge"];
  if (health.perkey_hid) {
    perkeyBadge.className = "badge good";
    perkeyBadge.textContent = "HID detected";
  } else {
    perkeyBadge.className = "badge warn";
    perkeyBadge.textContent = "Zone RGB only";
  }

  const caps = health.capabilities || [];
  els["sys-caps"].replaceChildren();
  if (caps.length === 0) {
    const chip = document.createElement("span");
    chip.className = "cap-chip";
    chip.textContent = "capabilities file unavailable";
    els["sys-caps"].append(chip);
  } else {
    caps.slice(0, 9).forEach((line) => {
      const chip = document.createElement("span");
      chip.className = "cap-chip";
      chip.textContent = line;
      chip.title = line;
      els["sys-caps"].append(chip);
    });
  }
}

function renderProbe(data) {
  els["probe-checks"].replaceChildren();
  (data.checks || []).forEach((check) => {
    const item = document.createElement("li");
    const icon = document.createElement("span");
    icon.className = `check-icon ${check.ok ? "ok" : "fail"}`;
    const text = document.createElement("span");
    text.textContent = `${check.label} — ${check.detail}`;
    item.append(icon, text);
    els["probe-checks"].append(item);
  });

  if (data.perkey?.available) {
    els["perkey-badge"].className = "badge good";
    els["perkey-badge"].textContent = "HID detected";
  } else {
    els["perkey-badge"].className = "badge warn";
    els["perkey-badge"].textContent = "Zone RGB only";
  }

  els["probe-json"].textContent = JSON.stringify(data, null, 2);
  els["probe-time"].textContent = new Date().toLocaleTimeString([], {
    hour: "2-digit",
    minute: "2-digit",
  });
}

/* -------------------------------------------------------------------------- */
/* Data loading                                                               */
/* -------------------------------------------------------------------------- */

async function refreshStatus({ silent = false, notify = false } = {}) {
  try {
    const data = await request("/api/status", {}, true);
    if (data.health) renderHealth(data.health);
    if (data.ok) {
      applyStatus(data);
      if (notify) toast("Status refreshed", "good");
    } else if (!silent) {
      toast(data.error || "Keyboard is not available", "bad");
    }
  } catch (error) {
    setConnection("bad", "Dashboard offline", "check the local service");
    if (!silent) toast(error.message, "bad");
  }
}

async function refreshActivity() {
  try {
    const data = await request("/api/activity");
    state.activity = data.activity || [];
    renderActivity();
  } catch {
    /* Activity is non-critical. */
  }
}

async function sendAction(path, payload, successMessage) {
  const id = ++mutationId;
  try {
    const data = await request(path, {
      method: "POST",
      body: JSON.stringify(payload),
    });

    if (data.status && id >= appliedMutation) {
      appliedMutation = id;
      applyStatus(data.status);
    }
    if (data.activity) {
      state.activity = data.activity;
      renderActivity();
    }
    if (successMessage) toast(successMessage, "good");
    return data;
  } catch (error) {
    state.activity.unshift({
      id: Date.now(),
      time: new Date().toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" }),
      action: "Error",
      detail: error.message,
      ok: false,
    });
    renderActivity();
    toast(error.message, "bad");
    await refreshStatus({ silent: true });
    return null;
  }
}

/* -------------------------------------------------------------------------- */
/* Actions                                                                    */
/* -------------------------------------------------------------------------- */

function scheduleColorApply() {
  clearTimeout(colorTimer);
  colorTimer = setTimeout(() => {
    applyColor(state.color, { silent: true });
  }, 220);
}

async function applyColor(hex, { brightness = state.brightness, zone = state.zone, silent = false } = {}) {
  const color = normalizeHex(hex);
  if (!isValidHex(color)) {
    toast("Enter a valid #RRGGBB color", "bad");
    return;
  }

  clearTimeout(colorTimer);
  state.color = color;
  renderColor();
  renderKeyboardColors();

  await sendAction(
    "/api/color",
    { color, brightness, zone },
    silent ? null : `Color set to ${color.toUpperCase()}`,
  );
}

async function applyBrightness(level, { silent = false } = {}) {
  const value = clamp(Number.parseInt(level, 10) || 0, 0, 4);
  clearTimeout(brightnessTimer);
  state.brightness = value;
  renderBrightness();
  renderKeyboardColors();

  await sendAction(
    "/api/brightness",
    { brightness: value },
    silent ? null : `Brightness ${value}/4`,
  );
}

async function applyEffect(effect) {
  state.effect = effect;
  renderEffect();
  renderKeyboardColors();

  const label = effect === "none" ? "Static color" : `${effectLabel(effect)} effect`;
  await sendAction("/api/effect", { effect }, label);
}

async function applyZone(zone) {
  state.zone = zone in ZONE_LABELS ? zone : "main";
  renderZone();
  renderKeyboardColors();
  await applyColor(state.color, { zone: state.zone });
}

async function togglePower() {
  const on = !state.power;
  state.power = on;
  renderPower();
  renderKeyboardColors();
  await sendAction("/api/power", { on }, on ? "Backlight on" : "Backlight off");
}

async function applyPreset(preset) {
  state.color = normalizeHex(preset.color);
  state.brightness = clamp(Number.parseInt(preset.brightness, 10) || 0, 0, 4);
  state.effect = preset.effect || "none";
  state.zone = preset.zone || "main";
  renderAll();

  await sendAction(
    "/api/preset",
    {
      color: state.color,
      brightness: state.brightness,
      effect: state.effect,
      zone: state.zone,
    },
    `${preset.name} applied`,
  );
}

/* -------------------------------------------------------------------------- */
/* Probe                                                                      */
/* -------------------------------------------------------------------------- */

async function runProbe() {
  if (state.probing) return;
  state.probing = true;
  els["probe-button"].disabled = true;
  els["probe-button"].textContent = "Probing…";
  els["probe-json"].textContent = "Reading ACPI, kernel module, device permissions and HID enumeration…";

  try {
    const data = await request("/api/probe");
    state.lastProbe = data;
    renderProbe(data);
    els["probe-copy"].disabled = false;
    await refreshStatus({ silent: true });
    toast("Hardware probe complete", "good");
  } catch (error) {
    els["probe-json"].textContent = `Probe failed:\n${error.message}`;
    toast(error.message, "bad");
  } finally {
    state.probing = false;
    els["probe-button"].disabled = false;
    els["probe-button"].textContent = "Run hardware probe";
  }
}

async function copyProbe() {
  if (!state.lastProbe) return;
  const text = JSON.stringify(state.lastProbe, null, 2);

  try {
    if (navigator.clipboard?.writeText) {
      await navigator.clipboard.writeText(text);
    } else {
      const area = document.createElement("textarea");
      area.value = text;
      area.style.position = "fixed";
      area.style.opacity = "0";
      document.body.append(area);
      area.select();
      document.execCommand("copy");
      area.remove();
    }
    toast("Diagnostics copied to clipboard", "good");
  } catch {
    toast("Could not access the clipboard", "bad");
  }
}

/* -------------------------------------------------------------------------- */
/* Presets                                                                    */
/* -------------------------------------------------------------------------- */

function loadCustomPresets() {
  try {
    const raw = localStorage.getItem("lumen.presets.v1");
    const parsed = raw ? JSON.parse(raw) : [];
    return Array.isArray(parsed) ? parsed.filter((item) => item && item.name && item.color) : [];
  } catch {
    return [];
  }
}

function saveCustomPresets(presets) {
  try {
    localStorage.setItem("lumen.presets.v1", JSON.stringify(presets));
  } catch {
    toast("Could not save the preset in this browser", "bad");
  }
}

function makePresetCard(preset, custom = false) {
  const card = document.createElement("div");
  card.className = "preset-card";
  card.tabIndex = 0;
  card.setAttribute("role", "button");
  card.setAttribute("aria-label", `Apply ${preset.name} preset`);

  const swatch = document.createElement("span");
  swatch.className = "preset-swatch";
  swatch.style.setProperty("--preset-color", preset.color);
  swatch.style.setProperty("--preset-glow", hexToRgba(preset.color, 0.35));

  const name = document.createElement("strong");
  name.textContent = preset.name;

  const detail = document.createElement("small");
  const effect = preset.effect && preset.effect !== "none" ? effectLabel(preset.effect) : "Static";
  detail.textContent = `${preset.brightness}/4 · ${effect}${custom ? " · Custom" : ` · ${preset.tag || "Preset"}`}`;

  card.append(swatch, name, detail);

  if (custom) {
    const remove = document.createElement("button");
    remove.type = "button";
    remove.className = "preset-delete";
    remove.textContent = "×";
    remove.setAttribute("aria-label", `Delete ${preset.name} preset`);
    remove.addEventListener("click", (event) => {
      event.stopPropagation();
      const remaining = loadCustomPresets().filter((item) => item.id !== preset.id);
      saveCustomPresets(remaining);
      renderPresets();
      toast(`${preset.name} removed`, "good");
    });
    card.append(remove);
  }

  const activate = () => applyPreset(preset);
  card.addEventListener("click", activate);
  card.addEventListener("keydown", (event) => {
    if (event.key === "Enter" || event.key === " ") {
      event.preventDefault();
      activate();
    }
  });

  return card;
}

function renderPresets() {
  els["preset-grid"].replaceChildren();
  STATIC_PRESETS.forEach((preset) => {
    els["preset-grid"].append(makePresetCard(preset, false));
  });
  loadCustomPresets().forEach((preset) => {
    els["preset-grid"].append(makePresetCard(preset, true));
  });
}

function openPresetDialog() {
  els["preset-name"].value = "";
  els["preset-dialog"].showModal();
  setTimeout(() => els["preset-name"].focus(), 50);
}

function saveCurrentPreset() {
  const name = els["preset-name"].value.trim() || "My lighting";
  const presets = loadCustomPresets();
  presets.push({
    id: `custom-${Date.now()}`,
    name,
    color: state.color,
    brightness: state.brightness,
    effect: state.effect,
    zone: state.zone,
  });
  saveCustomPresets(presets);
  renderPresets();
  els["preset-dialog"].close();
  toast(`${name} saved`, "good");
}

/* -------------------------------------------------------------------------- */
/* Activity / keyboard / effects / swatches                                   */
/* -------------------------------------------------------------------------- */

function renderActivity() {
  els["activity-list"].replaceChildren();
  const items = state.activity || [];
  els["activity-count"].textContent = String(items.length);

  if (items.length === 0) {
    const empty = document.createElement("li");
    empty.className = "empty";
    empty.textContent = "No actions yet. Pick a color to begin.";
    els["activity-list"].append(empty);
    return;
  }

  items.slice(0, 8).forEach((item) => {
    const row = document.createElement("li");
    row.className = `activity-item${item.ok === false ? " bad" : ""}`;

    const dot = document.createElement("span");
    dot.className = "activity-dot";

    const main = document.createElement("div");
    main.className = "activity-main";
    const title = document.createElement("strong");
    title.textContent = item.action || "Action";
    const detail = document.createElement("span");
    detail.textContent = item.detail || "";
    main.append(title, detail);

    const time = document.createElement("span");
    time.className = "activity-time";
    time.textContent = item.time || "";

    row.append(dot, main, time);
    els["activity-list"].append(row);
  });
}

function renderKeyboard() {
  els.keyboard.replaceChildren();

  KEY_ROWS.forEach((keys) => {
    const row = document.createElement("div");
    row.className = "kb-row";

    keys.forEach(([label, width]) => {
      const key = document.createElement("div");
      key.className = "key";
      key.style.setProperty("--w", String(width));
      key.textContent = label;
      if (label.length > 2) key.classList.add("small-label");
      row.append(key);
    });

    els.keyboard.append(row);
  });
}

function renderEffects() {
  els["effect-grid"].replaceChildren();

  EFFECTS.forEach((effect) => {
    const card = document.createElement("button");
    card.type = "button";
    card.className = "effect-card";
    card.dataset.effect = effect.id;
    card.setAttribute("aria-label", `Apply ${effect.name} effect`);

    const icon = document.createElement("span");
    icon.className = "fx-icon";
    icon.textContent = effect.icon;

    const text = document.createElement("span");
    const name = document.createElement("strong");
    name.textContent = effect.name;
    const desc = document.createElement("small");
    desc.textContent = effect.desc;
    text.append(name, desc);

    card.append(icon, text);
    card.addEventListener("click", () => applyEffect(effect.id));
    els["effect-grid"].append(card);
  });

  renderEffect();
}

function renderSwatches() {
  els.swatches.replaceChildren();

  SWATCHES.forEach(([hex, name]) => {
    const swatch = document.createElement("button");
    swatch.type = "button";
    swatch.className = "swatch";
    swatch.dataset.hex = hex;
    swatch.title = name;
    swatch.setAttribute("aria-label", name);
    swatch.style.setProperty("--swatch", hex);
    swatch.addEventListener("click", () => {
      state.color = hex;
      renderColor();
      applyColor(hex);
    });
    els.swatches.append(swatch);
  });

  renderColor();
}

/* -------------------------------------------------------------------------- */
/* Events                                                                     */
/* -------------------------------------------------------------------------- */

function bindEvents() {
  els["color-input"].addEventListener("input", () => {
    const color = normalizeHex(els["color-input"].value);
    if (!isValidHex(color)) return;
    state.color = color;
    renderColor();
    renderKeyboardColors();
    scheduleColorApply();
  });

  els["color-input"].addEventListener("change", () => {
    const color = normalizeHex(els["color-input"].value);
    if (isValidHex(color)) applyColor(color);
  });

  els["hex-input"].addEventListener("input", () => {
    const raw = els["hex-input"].value.trim();
    if (!/^#?[0-9a-f]{6}$/i.test(raw)) return;
    const color = normalizeHex(raw);
    state.color = color;
    renderColor();
    renderKeyboardColors();
  });

  const commitHex = () => {
    const raw = els["hex-input"].value.trim();
    const color = normalizeHex(raw);
    if (!isValidHex(color) || !/^#?[0-9a-f]{6}$/i.test(raw)) {
      toast("Hex colors look like #22D3EE", "bad");
      renderColor();
      return;
    }
    applyColor(color);
  };

  els["hex-input"].addEventListener("change", commitHex);
  els["hex-input"].addEventListener("keydown", (event) => {
    if (event.key === "Enter") {
      event.preventDefault();
      commitHex();
    }
  });

  els["zone-select"].addEventListener("change", () => applyZone(els["zone-select"].value));

  els["brightness"].addEventListener("input", () => {
    const level = clamp(Number.parseInt(els.brightness.value, 10) || 0, 0, 4);
    state.brightness = level;
    renderBrightness();
    renderKeyboardColors();
    clearTimeout(brightnessTimer);
    brightnessTimer = setTimeout(() => applyBrightness(level, { silent: true }), 180);
  });

  els["brightness"].addEventListener("change", () => {
    applyBrightness(els.brightness.value);
  });

  els["power-toggle"].addEventListener("click", togglePower);
  els["probe-button"].addEventListener("click", runProbe);
  els["probe-copy"].addEventListener("click", copyProbe);
  els["refresh-btn"].addEventListener("click", () => {
    refreshStatus({ notify: true });
    refreshActivity();
  });

  els["save-preset"].addEventListener("click", openPresetDialog);
  els["save-preset-top"].addEventListener("click", openPresetDialog);
  els["preset-cancel"].addEventListener("click", () => els["preset-dialog"].close());
  els["preset-form"].addEventListener("submit", (event) => {
    event.preventDefault();
    saveCurrentPreset();
  });
  els["preset-dialog"].addEventListener("click", (event) => {
    if (event.target === els["preset-dialog"]) els["preset-dialog"].close();
  });

  document.querySelectorAll(".nav-link").forEach((link) => {
    link.addEventListener("click", () => {
      document.querySelectorAll(".nav-link").forEach((item) => item.classList.remove("active"));
      link.classList.add("active");
    });
  });
}

/* -------------------------------------------------------------------------- */
/* Init                                                                       */
/* -------------------------------------------------------------------------- */

function init() {
  cacheElements();
  renderKeyboard();
  renderEffects();
  renderSwatches();
  renderPresets();
  renderActivity();
  renderBrightness();
  renderPower();
  renderKeyboardColors();
  bindEvents();
  refreshStatus({ silent: true });
  refreshActivity();
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", init);
} else {
  init();
}
