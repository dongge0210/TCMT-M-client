// In-memory device registry + snapshot ring buffer with JSON persistence.
// The client (TCMT-M --http) registers once, then POSTs a snapshot every 2s.
import fs from 'node:fs';
import path from 'node:path';
import crypto from 'node:crypto';

// Snapshots kept per device in memory (~1h at the client's 2s cadence).
const RING_CAPACITY = 1800;

function randomHex(len) {
  return crypto.randomBytes(Math.ceil(len / 2)).toString('hex').slice(0, len);
}

// Flatten a nested JSON snapshot into dotted numeric paths, e.g.
// { cpu: { usage: 12 } } -> { "cpu.usage": 12 }. Arrays use [i] keys.
export function flatten(obj, prefix = '', out = {}) {
  for (const [key, value] of Object.entries(obj)) {
    const name = prefix ? `${prefix}.${key}` : key;
    if (typeof value === 'number' && Number.isFinite(value)) {
      out[name] = value;
    } else if (value && typeof value === 'object') {
      if (Array.isArray(value)) {
        value.forEach((item, i) => {
          if (typeof item === 'number' && Number.isFinite(item)) out[`${name}[${i}]`] = item;
          else if (item && typeof item === 'object') flatten(item, `${name}[${i}]`, out);
        });
      } else {
        flatten(value, name, out);
      }
    }
  }
  return out;
}

function freshDevice(seed = {}) {
  return {
    id: seed.id || '',
    clientKey: seed.clientKey || '',
    name: seed.name || 'Unknown',
    os: seed.os || 'Unknown',
    model: seed.model || 'Unknown',
    token: seed.token || '',
    lastSeen: seed.lastSeen || 0,
    latest: {},        // most recent snapshot (token stripped)
    series: [],        // [{ ts, data, flat }]
    stats: new Map(),  // field -> { min, max, last, count }
  };
}

export class Store {
  constructor({ dataDir }) {
    this.dataDir = dataDir;
    this.devicesFile = path.join(dataDir, 'devices.json');
    this.devices = new Map();
    this._load();
  }

  _load() {
    try {
      const raw = fs.readFileSync(this.devicesFile, 'utf8');
      const arr = JSON.parse(raw);
      if (Array.isArray(arr)) {
        for (const d of arr) {
          if (d && d.id && d.token) this.devices.set(d.id, freshDevice(d));
        }
      }
    } catch {
      /* first run */
    }
  }

  _save() {
    try {
      fs.mkdirSync(this.dataDir, { recursive: true });
      const tmp = this.devicesFile + '.tmp';
      const arr = [...this.devices.values()].map(d => ({
        id: d.id, clientKey: d.clientKey, name: d.name, os: d.os, model: d.model,
        token: d.token, lastSeen: d.lastSeen,
      }));
      fs.writeFileSync(tmp, JSON.stringify(arr, null, 2));
      fs.renameSync(tmp, this.devicesFile);
    } catch (err) {
      console.error('[store] save failed:', err.message);
    }
  }

  register(clientKey, name, os, model) {
    // Stable identity across restarts: prefer the random per-machine clientKey
    // (works across NAT rebinds / IP changes / shared public servers); fall
    // back to name for older clients without a key.
    for (const existing of this.devices.values()) {
      if ((clientKey && existing.clientKey === clientKey) || (!clientKey && existing.name === name)) {
        existing.clientKey = clientKey || existing.clientKey;
        existing.os = os || existing.os;
        existing.model = model || existing.model;
        existing.lastSeen = Date.now();
        this._save();
        return existing;
      }
    }
    let id = 'dev_' + randomHex(6);
    while (this.devices.has(id)) id = 'dev_' + randomHex(6);
    const device = freshDevice();
    device.id = id;
    device.clientKey = clientKey || '';
    device.token = 'tcmt_' + randomHex(7);
    device.name = name || 'Unknown';
    device.os = os || 'Unknown';
    device.model = model || 'Unknown';
    device.lastSeen = Date.now();
    this.devices.set(id, device);
    this._save();
    return device;
  }

  auth(token) {
    if (!token) return false;
    for (const d of this.devices.values()) if (d.token === token) return true;
    return false;
  }

  get(id) {
    return this.devices.get(id) || null;
  }

  getByToken(token) {
    for (const d of this.devices.values()) if (d.token === token) return d;
    return null;
  }

  // Public device list with live online status (15s freshness window).
  list() {
    const now = Date.now();
    return [...this.devices.values()].map(d => ({
      id: d.id,
      name: d.name,
      os: d.os,
      model: d.model,
      online: now - d.lastSeen < 15000,
      lastSeen: d.lastSeen,
    }));
  }

  ingest(token, data, ts = Date.now()) {
    const device = this.getByToken(token);
    if (!device) return null;
    const { token: _dropped, ...rest } = data; // never expose the token in snapshots
    device.latest = rest;
    device.lastSeen = ts;
    const flat = flatten(rest);
    device.series.push({ ts, data: rest, flat });
    if (device.series.length > RING_CAPACITY) {
      device.series.splice(0, device.series.length - RING_CAPACITY);
    }
    for (const [field, value] of Object.entries(flat)) {
      let stat = device.stats.get(field);
      if (!stat) {
        stat = { min: value, max: value, last: value, count: 0 };
        device.stats.set(field, stat);
      }
      stat.min = Math.min(stat.min, value);
      stat.max = Math.max(stat.max, value);
      stat.last = value;
      stat.count += 1;
    }
    this._save();
    return device;
  }

  // Numeric fields seen for a device, with min/max/last/count ("整理" index).
  fields(id) {
    const device = this.get(id);
    if (!device) return null;
    return [...device.stats.entries()].map(([field, stat]) => ({
      field, min: stat.min, max: stat.max, last: stat.last, count: stat.count,
    }));
  }

  history(id, field, from, to, limit = 1000) {
    const device = this.get(id);
    if (!device) return null;
    const points = [];
    for (const p of device.series) {
      if (p.ts < from || p.ts > to) continue;
      const value = p.flat[field];
      if (value === undefined) continue;
      points.push({ ts: p.ts, value });
    }
    if (limit > 0 && points.length > limit) {
      // Evenly-spaced downsampling to keep the response bounded.
      const step = points.length / limit;
      const sampled = [];
      for (let i = 0; i < limit; i += 1) sampled.push(points[Math.floor(i * step)]);
      return sampled;
    }
    return points;
  }

  // Organized view: pulls the fields the UI cares about out of the raw
  // snapshot, derives memory percent and a temperatures list.
  summary(id) {
    const device = this.get(id);
    if (!device) return null;
    const latest = device.latest || {};
    const out = {
      id: device.id,
      name: device.name,
      os: device.os,
      model: device.model,
      lastSeen: device.lastSeen,
      cpu: {},
      memory: {},
      gpu: {},
      motion: {},
      temperatures: [],
    };
    if (latest.cpu_usage !== undefined) out.cpu.usage = latest.cpu_usage;
    if (latest.cpu_temp !== undefined) out.cpu.temp = latest.cpu_temp;
    if (latest.memory_total && latest.memory_used !== undefined) {
      out.memory.total = latest.memory_total;
      out.memory.used = latest.memory_used;
      out.memory.percent = Math.round((latest.memory_used / latest.memory_total) * 1000) / 10;
    }
    if (latest.gpu_usage !== undefined) out.gpu.usage = latest.gpu_usage;
    if (latest.gpu_temp !== undefined) out.gpu.temp = latest.gpu_temp;
    for (const key of ['ax', 'ay', 'az', 'gx', 'gy', 'gz', 'lidAngle', 'hb', 'imut']) {
      if (latest[key] !== undefined) out.motion[key] = latest[key];
    }
    if (latest.cpu_temp !== undefined) out.temperatures.push({ name: 'CPU', value: latest.cpu_temp, unit: '°C' });
    if (latest.gpu_temp !== undefined) out.temperatures.push({ name: 'GPU', value: latest.gpu_temp, unit: '°C' });
    if (Array.isArray(latest.temperatures)) {
      for (const t of latest.temperatures) {
        if (t && typeof t === 'object' && t.value !== undefined) {
          out.temperatures.push({
            name: t.name || t.sensor || t.id || 'Sensor',
            value: t.value,
            unit: t.unit || '°C',
          });
        }
      }
    }
    return out;
  }
}
