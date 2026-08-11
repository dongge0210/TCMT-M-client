// REST API router. Wire protocol is compatible with TCMT-M's ServerProbe
// (POST /api/register + /api/ingest) and the C++ tcmt-server v0.1.0 routes.
import fs from 'node:fs';
import path from 'node:path';

const MAX_BODY = 1024 * 1024;

function httpError(status, message) {
  const err = new Error(message);
  err.status = status;
  return err;
}

function json(res, status, obj) {
  const body = JSON.stringify(obj);
  res.writeHead(status, {
    'Content-Type': 'application/json; charset=utf-8',
    'Content-Length': Buffer.byteLength(body),
  });
  res.end(body);
}

function readBody(req) {
  return new Promise((resolve, reject) => {
    let size = 0;
    const chunks = [];
    req.on('data', chunk => {
      size += chunk.length;
      if (size > MAX_BODY) {
        reject(httpError(413, 'payload too large'));
        req.destroy();
        return;
      }
      chunks.push(chunk);
    });
    req.on('end', () => resolve(Buffer.concat(chunks).toString('utf8')));
    req.on('error', reject);
  });
}

function parseJsonBody(body) {
  try {
    return JSON.parse(body);
  } catch {
    throw httpError(400, 'invalid JSON body');
  }
}

// Accepts "-1h"/"-30m"/"-7d"/"-90s" or an absolute unix-ms timestamp.
function parseTime(value, fallback) {
  if (!value) return fallback;
  if (value[0] === '-') {
    const num = Number(value.slice(1, -1));
    const unit = value[value.length - 1];
    const ms = {
      s: 1000, m: 60000, h: 3600000, d: 86400000,
    }[unit] || 1000;
    return Date.now() - num * ms;
  }
  return Number(value) || fallback;
}

function serveStatic(res, root, pathname) {
  const rel = pathname === '/' ? 'index.html' : pathname.replace(/^\/+/, '');
  const file = path.resolve(root, rel);
  if (file !== root && !file.startsWith(root + path.sep)) {
    res.writeHead(403);
    res.end('forbidden');
    return;
  }
  try {
    const data = fs.readFileSync(file);
    const ext = path.extname(file).toLowerCase();
    const mime = {
      '.html': 'text/html; charset=utf-8',
      '.css': 'text/css; charset=utf-8',
      '.js': 'text/javascript; charset=utf-8',
      '.mjs': 'text/javascript; charset=utf-8',
      '.json': 'application/json; charset=utf-8',
      '.svg': 'image/svg+xml',
      '.png': 'image/png',
      '.ico': 'image/x-icon',
      '.woff2': 'font/woff2',
    }[ext] || 'application/octet-stream';
    res.writeHead(200, { 'Content-Type': mime, 'Content-Length': data.length });
    res.end(data);
  } catch {
    res.writeHead(404);
    res.end('not found');
  }
}

function authorized(authToken, req, url) {
  if (!authToken) return true;
  const header = req.headers.authorization || '';
  if (header.startsWith('Bearer ')) return header.slice(7) === authToken;
  return url.searchParams.get('access_token') === authToken;
}

export function createHandler({ store, staticDir, onSnapshot, authToken = '' }) {
  return async function handle(req, res) {
    res.setHeader('Access-Control-Allow-Origin', '*');
    res.setHeader('Access-Control-Allow-Methods', 'GET, POST, OPTIONS');
    res.setHeader('Access-Control-Allow-Headers', 'Content-Type, Authorization');
    if (req.method === 'OPTIONS') {
      res.writeHead(204);
      res.end();
      return;
    }

    try {
      const url = new URL(req.url, 'http://localhost');
      const pathname = url.pathname;

      if (req.method === 'GET' && pathname === '/ping') {
        return json(res, 200, { status: 'ok', time: Date.now() });
      }

      if (req.method === 'POST' && pathname === '/api/register') {
        const body = parseJsonBody(await readBody(req));
        const device = store.register(body.clientKey || '', body.name, body.os, body.model);
        return json(res, 200, { id: device.id, token: device.token, name: device.name });
      }

      if (req.method === 'POST' && pathname === '/api/ingest') {
        const body = parseJsonBody(await readBody(req));
        // Only device tokens can write; admin token (--auth-token) protects reads.
        if (!store.auth(body.token)) throw httpError(401, 'unauthorized');
        const device = store.ingest(body.token, body, Date.now());
        if (!device) throw httpError(401, 'unauthorized');
        if (onSnapshot) onSnapshot(device);
        return json(res, 200, { status: 'ok' });
      }

      if (req.method === 'GET' && pathname.startsWith('/api/')) {
        if (!authorized(authToken, req, url)) throw httpError(401, 'missing or invalid access token');
      }

      if (req.method === 'GET' && pathname === '/api/devices') {
        return json(res, 200, store.list());
      }

      const deviceMatch = pathname.match(/^\/api\/devices\/([^/]+)(?:\/([^?/]+))?/);
      if (req.method === 'GET' && deviceMatch) {
        const id = decodeURIComponent(deviceMatch[1]);
        const sub = deviceMatch[2] || '';
        const device = store.get(id);
        if (!device) throw httpError(404, 'device not found');
        if (!sub) {
          const now = Date.now();
          return json(res, 200, {
            id: device.id,
            name: device.name,
            os: device.os,
            model: device.model,
            online: now - device.lastSeen < 15000,
            lastSeen: device.lastSeen,
          });
        }
        if (sub === 'latest') return json(res, 200, device.latest);
        if (sub === 'summary') return json(res, 200, store.summary(id));
        if (sub === 'fields') return json(res, 200, { deviceId: id, fields: store.fields(id) || [] });
        if (sub === 'temperatures') {
          const summary = store.summary(id);
          return json(res, 200, (summary && summary.temperatures) || []);
        }
        if (sub === 'history') {
          const field = url.searchParams.get('field') || '';
          if (!field) throw httpError(400, "missing 'field' query parameter");
          const from = parseTime(url.searchParams.get('from'), Date.now() - 3600000);
          const to = parseTime(url.searchParams.get('to'), Date.now());
          const requested = Number(url.searchParams.get('limit'));
          const limit = Math.max(1, Math.min(5000, Number.isFinite(requested) ? requested : 1000));
          const history = store.history(id, field, from, to, limit) || [];
          return json(res, 200, {
            deviceId: id, field, from, to, count: history.length, history,
          });
        }
        if (Object.prototype.hasOwnProperty.call(device.latest, sub)) {
          return json(res, 200, device.latest[sub]);
        }
        throw httpError(404, 'unknown sub-resource');
      }

      if (req.method === 'GET') {
        return serveStatic(res, staticDir, pathname);
      }

      throw httpError(404, 'not found');
    } catch (err) {
      const status = err.status || 500;
      if (status >= 500) console.error('[api]', err);
      return json(res, status, { error: err.message || 'internal error' });
    }
  };
}
