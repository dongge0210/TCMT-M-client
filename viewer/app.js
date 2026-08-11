// TCMT Viewer — pure display. Reads everything from tcmt-server.
const API = location.origin && location.origin.startsWith('http')
  ? location.origin
  : 'http://127.0.0.1:8080';

let accessToken = localStorage.getItem('tcmt_viewer_token') || '';
let promptShown = false;

const state = {
  devices: [],
  currentId: null,
  latest: {},
  devicesNow: {},   // id -> latest snapshot (multi-device overview)
  fields: [],
  ws: null,
  reconnectTimer: null,
  spark: {},
};

const MAX_SPARK = 90;
const SPARK_FIELDS = ['cpu_usage', 'cpu_temp', 'gpu_usage', 'memory_percent'];
const GAUGES = { cpu_usage: 'cpuGauge', gpu_usage: 'gpuGauge' };

const $ = id => document.getElementById(id);

/* ── helpers ─────────────────────────────────────────── */
async function api(path, opts) {
  const headers = { ...((opts && opts.headers) || {}) };
  if (accessToken) headers['Authorization'] = 'Bearer ' + accessToken;
  const res = await fetch(API + path, { ...opts, headers });
  if (res.status === 401) {
    await askAccessToken();
    throw new Error('401 unauthorized');
  }
  if (!res.ok) throw new Error(`${res.status} ${res.statusText}`);
  return res.json();
}

async function askAccessToken() {
  if (promptShown) return;
  promptShown = true;
  const token = window.prompt('该 server 需要访问令牌（--auth-token 配置的值）：');
  if (token) {
    accessToken = token.trim();
    localStorage.setItem('tcmt_viewer_token', accessToken);
    location.reload();
  }
}

function fmtNum(v, digits = 1) {
  return v === undefined || v === null || Number.isNaN(Number(v)) ? '--' : Number(v).toFixed(digits);
}

function fmtBytes(v) {
  if (v === undefined || v === null) return '--';
  const n = Number(v);
  if (n < 1024) return n + ' B';
  const units = ['KB', 'MB', 'GB', 'TB'];
  let i = -1;
  let x = n;
  while (x >= 1024 && i < units.length - 1) { x /= 1024; i += 1; }
  return x.toFixed(1) + ' ' + units[i];
}

function setGauge(id, pct) {
  const el = $(id);
  if (!el) return;
  const clamped = Math.max(0, Math.min(100, Number.isFinite(pct) ? pct : 0));
  el.style.background = `conic-gradient(var(--accent) ${clamped}%, var(--panel-2) ${clamped}% 100%)`;
}

function setConn(mode) {
  const dot = $('connDot');
  const text = $('connText');
  dot.className = 'dot ' + mode;
  if (mode === 'on') text.textContent = '实时连接';
  else if (mode === 'poll') text.textContent = '轮询模式';
  else text.textContent = '未连接';
}

/* ── device list / selection ─────────────────────────── */
async function loadDevices() {
  try {
    state.devices = await api('/api/devices');
    renderDeviceSelect();
    renderDeviceStrip();
    // Backfill overview snapshots for devices we haven't seen yet.
    for (const d of state.devices) {
      if (!state.devicesNow[d.id]) fetchLatestFor(d.id);
    }
    if (!state.currentId && state.devices.length) {
      selectDevice(state.devices[0].id);
    } else if (state.devices.length && !state.devices.some(d => d.id === state.currentId)) {
      selectDevice(state.devices[0].id);
    }
  } catch {
    setConn('');
  }
}

function renderDeviceSelect() {
  const sel = $('deviceSelect');
  const previous = sel.value;
  sel.innerHTML = '';
  if (!state.devices.length) {
    const opt = document.createElement('option');
    opt.value = '';
    opt.textContent = '暂无设备';
    sel.appendChild(opt);
    return;
  }
  for (const d of state.devices) {
    const opt = document.createElement('option');
    opt.value = d.id;
    opt.textContent = `${d.name} (${d.online ? '在线' : '离线'})`;
    sel.appendChild(opt);
  }
  if (previous && state.devices.some(d => d.id === previous)) sel.value = previous;
}

function selectDevice(id) {
  state.currentId = id;
  state.latest = {};
  state.spark = {};
  $('deviceSelect').value = id;
  renderDeviceStrip();
  refreshLatest();
  loadFields();
  loadHistory();
}

$('deviceSelect').addEventListener('change', e => {
  if (e.target.value) selectDevice(e.target.value);
});

/* ── multi-device overview strip ─────────────────────── */
async function fetchLatestFor(id) {
  try {
    const data = await api('/api/devices/' + id + '/latest');
    state.devicesNow[id] = data;
    renderDeviceStrip();
  } catch { /* device may be gone */ }
}

function deviceCpuPercent(data) {
  return data && data.cpu_usage !== undefined ? Number(data.cpu_usage) : null;
}

function deviceMemPercent(data) {
  if (data && data.memory_total && data.memory_used !== undefined) {
    return (data.memory_used / data.memory_total) * 100;
  }
  return null;
}

function renderDeviceStrip() {
  const strip = $('deviceStrip');
  if (!state.devices.length) {
    strip.innerHTML = '<div class="strip-empty">等待设备接入…（启动 TCMT-M --http 后自动出现）</div>';
    return;
  }
  const now = Date.now();
  strip.innerHTML = state.devices.map(d => {
    const data = state.devicesNow[d.id];
    const online = d.online || (now - d.lastSeen < 15000);
    const cpu = deviceCpuPercent(data);
    const mem = deviceMemPercent(data);
    const cpuTxt = cpu === null ? '--' : cpu.toFixed(0) + '%';
    const memTxt = mem === null ? '--' : mem.toFixed(0) + '%';
    const gpuTxt = data && data.gpu_usage !== undefined ? data.gpu_usage.toFixed(0) + '%' : '--';
    const tempTxt = data && data.cpu_temp !== undefined ? data.cpu_temp.toFixed(0) + '°C' : '--';
    const stateTxt = online ? '在线' : '离线';
    return (
      `<div class="device-card ${d.id === state.currentId ? 'active' : ''}" data-device="${d.id}">` +
      `<div class="dhead">` +
      `<span class="dot ${online ? 'on' : ''}" style="box-shadow:none"></span>` +
      `<span class="dname" title="${d.name}">${d.name}</span>` +
      `<span class="dstate">${stateTxt}</span>` +
      `</div>` +
      `<div class="dmeta">` +
      `<span>CPU</span><b>${cpuTxt}</b>` +
      `<span>内存</span><b>${memTxt}</b>` +
      `<span>GPU</span><b>${gpuTxt}</b>` +
      `<span>CPU温度</span><b>${tempTxt}</b>` +
      `</div>` +
      `<div class="mini"><div class="mini-bar"><i style="width:${Math.max(0, Math.min(100, cpu ?? 0))}%"></i></div>` +
      `<span style="font-size:11px;color:var(--muted)">${cpuTxt}</span></div>` +
      `</div>`
    );
  }).join('');
}

$('deviceStrip').addEventListener('click', e => {
  const card = e.target.closest('.device-card');
  if (card && card.dataset.device) selectDevice(card.dataset.device);
});

/* ── live data ───────────────────────────────────────── */
function pushSpark(summaryOrLatest) {
  for (const field of SPARK_FIELDS) {
    const value = valueOf(field, summaryOrLatest);
    if (value === undefined) continue;
    if (!state.spark[field]) state.spark[field] = [];
    state.spark[field].push(value);
    if (state.spark[field].length > MAX_SPARK) state.spark[field].shift();
  }
  drawSparks();
}

function valueOf(field, data) {
  if (field === 'memory_percent') {
    if (data && data.memory_total && data.memory_used !== undefined) {
      return (data.memory_used / data.memory_total) * 100;
    }
    return undefined;
  }
  return data ? data[field] : undefined;
}

function render(data) {
  const cpu = data.cpu_usage, cpuT = data.cpu_temp;
  $('cpuUsage').textContent = fmtNum(cpu, 0);
  $('cpuTemp').textContent = cpuT !== undefined ? fmtNum(cpuT, 1) + '°C' : '--';
  $('cpuLoad').textContent = data.load_avg !== undefined ? fmtNum(data.load_avg, 2) : '--';
  setGauge('cpuGauge', cpu);

  const memPct = valueOf('memory_percent', data);
  $('memPct').textContent = fmtNum(memPct, 0);
  $('memUsed').textContent = fmtBytes(data.memory_used);
  $('memTotal').textContent = fmtBytes(data.memory_total);
  setGauge('memGauge', memPct);

  const gpu = data.gpu_usage, gpuT = data.gpu_temp;
  $('gpuUsage').textContent = fmtNum(gpu, 0);
  $('gpuTemp').textContent = gpuT !== undefined ? fmtNum(gpuT, 1) + '°C' : '--';
  $('gpuState').textContent = data.gpu_name || '--';
  setGauge('gpuGauge', gpu);

  $('motionAcc').textContent = [data.ax, data.ay, data.az].every(v => v !== undefined)
    ? `${fmtNum(data.ax, 2)} / ${fmtNum(data.ay, 2)} / ${fmtNum(data.az, 2)}` : '--';
  $('motionGyro').textContent = [data.gx, data.gy, data.gz].every(v => v !== undefined)
    ? `${fmtNum(data.gx, 2)} / ${fmtNum(data.gy, 2)} / ${fmtNum(data.gz, 2)}` : '--';
  $('motionLid').textContent = data.lidAngle !== undefined ? fmtNum(data.lidAngle, 1) + '°' : '--';
  $('motionRate').textContent = [data.imut, data.hb].every(v => v !== undefined)
    ? `${fmtNum(data.imut, 0)} Hz / ${fmtNum(data.hb, 0)}` : '--';

  renderTemps(data);
}

function renderTemps(data) {
  const list = $('tempList');
  const temps = [];
  if (data.cpu_temp !== undefined) temps.push({ name: 'CPU', value: data.cpu_temp, unit: '°C' });
  if (data.gpu_temp !== undefined) temps.push({ name: 'GPU', value: data.gpu_temp, unit: '°C' });
  if (Array.isArray(data.temperatures)) {
    for (const t of data.temperatures) {
      if (t && t.value !== undefined) {
        temps.push({ name: t.name || t.sensor || t.id || 'Sensor', value: t.value, unit: t.unit || '°C' });
      }
    }
  }
  if (!temps.length) {
    list.innerHTML = '<li class="empty">暂无数据</li>';
    return;
  }
  list.innerHTML = temps.map(t =>
    `<li><span>${t.name}</span><b>${fmtNum(t.value, 1)}${t.unit || '°C'}</b></li>`
  ).join('');
}

async function refreshLatest() {
  if (!state.currentId) return;
  try {
    state.latest = await api('/api/devices/' + state.currentId + '/latest');
    render(state.latest);
    pushSpark(state.latest);
    setConn(state.ws && state.ws.readyState === WebSocket.OPEN ? 'on' : 'poll');
  } catch {
    setConn('');
  }
}

/* ── fields / sensors table ──────────────────────────── */
async function loadFields() {
  if (!state.currentId) return;
  try {
    const res = await api('/api/devices/' + state.currentId + '/fields');
    state.fields = res.fields || [];
    $('fieldCount').textContent = `${state.fields.length} 个数值字段`;
    const tbody = $('fieldTable').querySelector('tbody');
    tbody.innerHTML = state.fields.map(f =>
      `<tr><td>${f.field}</td><td>${fmtNum(f.last)}</td><td>${fmtNum(f.min)}</td><td>${fmtNum(f.max)}</td></tr>`
    ).join('');
    renderFieldSelect();
  } catch { /* server may not have data yet */ }
}

function renderFieldSelect() {
  const sel = $('fieldSelect');
  const previous = sel.value;
  const preferred = ['cpu_usage', 'cpu_temp', 'gpu_usage', 'gpu_temp', 'memory_used', 'ax', 'gy', 'lidAngle'];
  sel.innerHTML = '';
  const names = state.fields.map(f => f.field);
  const ordered = [...new Set([...preferred.filter(f => names.includes(f)), ...names])];
  for (const name of ordered) {
    const opt = document.createElement('option');
    opt.value = name;
    opt.textContent = name;
    sel.appendChild(opt);
  }
  if (previous && names.includes(previous)) sel.value = previous;
  else if (ordered.length) sel.value = ordered[0];
}

$('fieldSelect').addEventListener('change', loadHistory);

/* ── history chart ───────────────────────────────────── */
async function loadHistory() {
  const field = $('fieldSelect').value;
  if (!field || !state.currentId) return;
  try {
    const res = await api(`/api/devices/${state.currentId}/history?field=${encodeURIComponent(field)}&from=-15m&limit=800`);
    drawChart(res.history || [], field);
    $('historyHint').textContent = `${res.count} 点 · ${res.from} → ${res.to}`;
  } catch {
    drawChart([], field);
  }
}

function drawChart(points, field) {
  const canvas = $('historyChart');
  const dpr = window.devicePixelRatio || 1;
  const w = canvas.clientWidth || 800;
  const h = canvas.clientHeight || 280;
  canvas.width = w * dpr;
  canvas.height = h * dpr;
  const ctx = canvas.getContext('2d');
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, w, h);
  const pad = 10;
  if (!points.length) {
    ctx.fillStyle = '#7d8799';
    ctx.font = '13px sans-serif';
    ctx.textAlign = 'center';
    ctx.fillText('暂无数据（等客户端推送）', w / 2, h / 2);
    ctx.textAlign = 'left';
    return;
  }
  const values = points.map(p => p.value);
  let min = Math.min(...values);
  let max = Math.max(...values);
  if (max === min) { min -= 1; max += 1; }
  const range = max - min || 1;
  const x = i => pad + (i / (points.length - 1 || 1)) * (w - pad * 2);
  const y = v => pad + (1 - (v - min) / range) * (h - pad * 2);

  ctx.strokeStyle = '#232a38';
  ctx.lineWidth = 1;
  for (let i = 0; i <= 4; i += 1) {
    const gy = pad + (i / 4) * (h - pad * 2);
    ctx.beginPath();
    ctx.moveTo(pad, gy);
    ctx.lineTo(w - pad, gy);
    ctx.stroke();
  }

  const gradient = ctx.createLinearGradient(0, 0, 0, h);
  gradient.addColorStop(0, 'rgba(77, 208, 225, 0.28)');
  gradient.addColorStop(1, 'rgba(77, 208, 225, 0.02)');
  ctx.beginPath();
  points.forEach((p, i) => {
    const px = x(i), py = y(p.value);
    if (i === 0) ctx.moveTo(px, py);
    else ctx.lineTo(px, py);
  });
  ctx.lineTo(x(points.length - 1), h - pad);
  ctx.lineTo(x(0), h - pad);
  ctx.closePath();
  ctx.fillStyle = gradient;
  ctx.fill();

  ctx.strokeStyle = '#4dd0e1';
  ctx.lineWidth = 2;
  ctx.beginPath();
  points.forEach((p, i) => {
    const px = x(i), py = y(p.value);
    if (i === 0) ctx.moveTo(px, py);
    else ctx.lineTo(px, py);
  });
  ctx.stroke();

  ctx.fillStyle = '#7d8799';
  ctx.font = '11px sans-serif';
  ctx.fillText(`${max.toFixed(1)}`, 2, 12);
  ctx.fillText(`${min.toFixed(1)}`, 2, h - 4);
  ctx.fillText(field, w - ctx.measureText(field).width - 2, 12);
}

/* ── sparklines ──────────────────────────────────────── */
function drawSparks() {
  const map = {
    cpu_usage: 'sparkCpu',
    cpu_temp: 'sparkCpu',
    gpu_usage: 'sparkGpu',
    memory_percent: 'sparkMem',
  };
  for (const [field, canvasId] of Object.entries(map)) {
    const values = state.spark[field];
    const canvas = $(canvasId);
    if (!canvas || !values || values.length < 2) continue;
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth || 200;
    const h = canvas.clientHeight || 44;
    canvas.width = w * dpr;
    canvas.height = h * dpr;
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    let min = Math.min(...values);
    let max = Math.max(...values);
    if (max === min) { min -= 1; max += 1; }
    const x = i => (i / (values.length - 1)) * w;
    const y = v => 2 + (1 - (v - min) / (max - min)) * (h - 4);
    ctx.strokeStyle = '#4dd0e1';
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    values.forEach((v, i) => {
      if (i === 0) ctx.moveTo(x(i), y(v));
      else ctx.lineTo(x(i), y(v));
    });
    ctx.stroke();
  }
}

/* ── WebSocket ───────────────────────────────────────── */
function connectWs() {
  const wsUrl = WS_URL() + (accessToken ? '?access_token=' + encodeURIComponent(accessToken) : '');
  let ws;
  try {
    ws = new WebSocket(wsUrl);
  } catch {
    scheduleReconnect();
    return;
  }
  state.ws = ws;
  ws.onopen = () => setConn('on');
  ws.onclose = () => {
    setConn('poll');
    scheduleReconnect();
  };
  ws.onerror = () => ws.close();
  ws.onmessage = ev => {
    let msg;
    try { msg = JSON.parse(ev.data); } catch { return; }
    if (msg.type === 'devices') {
      state.devices = msg.data;
      renderDeviceSelect();
      renderDeviceStrip();
    } else if (msg.type === 'snapshot' && msg.deviceId === state.currentId) {
      render(msg.data);
      pushSpark(msg.data);
    } else if (msg.type === 'snapshot') {
      state.devicesNow[msg.deviceId] = msg.data;
      renderDeviceStrip();
    }
  };
}

function WS_URL() {
  return API.replace(/^http/, 'ws') + '/ws';
}

function scheduleReconnect() {
  clearTimeout(state.reconnectTimer);
  state.reconnectTimer = setTimeout(connectWs, 3000);
}

/* ── main loop ───────────────────────────────────────── */
setInterval(() => {
  loadDevices();
  refreshLatest();
  if ($('autoRefresh').checked) loadHistory();
}, 2000);

$('apiLabel').textContent = API;
loadDevices();
connectWs();
setInterval(drawSparks, 1000);
window.addEventListener('resize', () => { drawSparks(); loadHistory(); });
