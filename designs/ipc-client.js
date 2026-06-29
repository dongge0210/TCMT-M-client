// TCMT IPC Client — connects to tcmt-server REST API
// Uses device registration and per-device polling for motion/system/temperature data.

const BASE = 'http://127.0.0.1:8080';
let deviceId = null;
let deviceToken = null;

/**
 * Register this client as a device on tcmt-server.
 * Must be called before any polling client can fetch data.
 * @param {string} name - Friendly device name
 * @returns {Promise<{id: string, token: string}>}
 */
export async function registerDevice(name) {
  const res = await fetch(BASE + '/api/register', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ name: name || 'Browser Client', os: navigator.platform, model: 'Web' })
  });
  const d = await res.json();
  deviceId = d.id;
  deviceToken = d.token;
  return d;
}

/* ── Motion Client ─────────────────────────────── */
export class MotionClient {
  constructor() { this._cb = null; this._timer = null; }
  onData(fn) { this._cb = fn; }
  start(intervalMs = 200) {
    if (!deviceId) { console.warn('[MotionClient] No device registered yet'); return; }
    this._timer = setInterval(async () => {
      try {
        const res = await fetch(BASE + '/api/devices/' + deviceId + '/latest');
        if (res.ok) { const d = await res.json(); if (this._cb) this._cb(d); }
      } catch (_) { /* not connected */ }
    }, intervalMs);
  }
  stop() { clearInterval(this._timer); this._timer = null; }
}

/* ── System Client ─────────────────────────────── */
export class SystemClient {
  constructor() { this._cb = null; }
  onData(fn) { this._cb = fn; }
  async poll() {
    if (!deviceId) return;
    try {
      const res = await fetch(BASE + '/api/devices/' + deviceId + '/latest');
      if (res.ok && this._cb) this._cb(await res.json());
    } catch (_) {}
  }
}

/* ── Temperature Client ────────────────────────── */
export class TemperatureClient {
  constructor() { this._cb = null; }
  onData(fn) { this._cb = fn; }
  async poll() {
    if (!deviceId) return;
    try {
      const res = await fetch(BASE + '/api/devices/' + deviceId + '/temperatures');
      if (res.ok && this._cb) this._cb(await res.json());
    } catch (_) {}
  }
}

/* ── Connection monitor ────────────────────────── */
export async function connectionState() {
  try {
    const res = await fetch(BASE + '/ping');
    if (res.ok) { const d = await res.json(); return 'connected · ' + (d.time ? 'ok' : '?'); }
    return 'disconnected';
  } catch (_) { return 'disconnected'; }
}

/* ── Data ingest (for TCMT-M agents) ──────────── */
export async function ingestData(data) {
  if (!deviceToken) return false;
  const payload = { ...data, token: deviceToken };
  const res = await fetch(BASE + '/api/ingest', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload)
  });
  return res.ok;
}
