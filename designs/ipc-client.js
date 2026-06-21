// TCMT IPC Client — typed data-stream bridge
// Connects to TCMT-M MCP JSON-RPC server or HTTP bridge.
// Each client type receives only its data category.

const BASE = 'http://localhost:9876'; // TCMT-M MCP HTTP bridge

/** Generic JSON-RPC call */
async function rpc(method, params = {}) {
  const res = await fetch(BASE, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ jsonrpc: '2.0', id: Date.now(), method, params }),
  });
  if (!res.ok) throw new Error(`RPC ${method} failed: ${res.status}`);
  const j = await res.json();
  if (j.error) throw new Error(j.error.message);
  return j.result;
}

/* ── Motion Client ─────────────────────────────── */
export class MotionClient {
  constructor() { this._cb = null; this._timer = null; }
  onData(fn) { this._cb = fn; }
  start(intervalMs = 200) {
    this._timer = setInterval(async () => {
      try {
        const d = await rpc('sensors.motion');
        if (this._cb) this._cb(d);
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
    try { const d = await rpc('sensors.system_info'); if (this._cb) this._cb(d); } catch (_) {}
  }
}

/* ── Temperature Client ────────────────────────── */
export class TemperatureClient {
  constructor() { this._cb = null; }
  onData(fn) { this._cb = fn; }
  async poll() {
    try { const d = await rpc('sensors.temperature'); if (this._cb) this._cb(d); } catch (_) {}
  }
}

/* ── Connection monitor ────────────────────────── */
export function connectionState() {
  return rpc('system.ping').then(() => 'connected').catch(() => 'disconnected');
}
