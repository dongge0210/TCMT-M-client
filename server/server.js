#!/usr/bin/env node
// tcmt-server — middle layer between TCMT-M clients and viewers.
//   * relay:   ingests snapshots pushed by TCMT-M --http (ServerProbe)
//   * organize: keeps recent history, field index, and a derived summary
//   * display:  serves the pure-display viewer from ../viewer
// Usage: node server.js [--port 8080] [--host 127.0.0.1] [--data-dir data]
import http from 'node:http';
import https from 'node:https';
import fs from 'node:fs';
import path from 'node:path';
import os from 'node:os';
import { fileURLToPath } from 'node:url';
import { Store } from './lib/store.js';
import { createHandler } from './lib/api.js';
import { isWsRequest, acceptUpgrade, send } from './lib/ws.js';

const __dirname = path.dirname(fileURLToPath(import.meta.url));

function arg(name, fallback) {
  const i = process.argv.indexOf(name);
  return i >= 0 && process.argv[i + 1] ? process.argv[i + 1] : fallback;
}

const PORT = Number(arg('--port', process.env.TCMT_SERVER_PORT || '8080'));
// Cross-device default: listen on all interfaces so TCMT-M clients and
// viewers on other machines can reach the server over the network.
const HOST = arg('--host', process.env.TCMT_SERVER_HOST || '0.0.0.0');
const DATA_DIR = path.resolve(__dirname, arg('--data-dir', 'data'));
// The viewer end lives with the server (server/viewer) so the server is
// self-contained for display. --static-dir overrides it; ../viewer remains
// a fallback for older layouts.
const STATIC_DIR = (() => {
  const explicit = arg('--static-dir', '');
  if (explicit) return path.resolve(explicit);
  for (const candidate of ['viewer', '../viewer']) {
    const p = path.resolve(__dirname, candidate);
    if (fs.existsSync(path.join(p, 'index.html'))) return p;
  }
  return path.resolve(__dirname, 'viewer');
})();
const AUTH_TOKEN = arg('--auth-token', process.env.TCMT_SERVER_TOKEN || '');
const PUBLIC_URL = arg('--public-url', process.env.TCMT_SERVER_PUBLIC_URL || '');
const TLS_CERT = arg('--tls-cert', process.env.TCMT_SERVER_TLS_CERT || '');
const TLS_KEY = arg('--tls-key', process.env.TCMT_SERVER_TLS_KEY || '');
const SCHEME = TLS_CERT && TLS_KEY ? 'https' : 'http';

if (Boolean(TLS_CERT) !== Boolean(TLS_KEY)) {
  console.error('[tcmt-server] --tls-cert and --tls-key must be provided together');
  process.exit(1);
}

const store = new Store({ dataDir: DATA_DIR });
const clients = new Set();

function broadcast(obj) {
  const text = JSON.stringify(obj);
  for (const client of [...clients]) send(client, text);
}

const handler = createHandler({
  store,
  staticDir: STATIC_DIR,
  authToken: AUTH_TOKEN,
  onSnapshot(device) {
    broadcast({ type: 'snapshot', deviceId: device.id, data: device.latest });
  },
});

const server = SCHEME === 'https'
  ? https.createServer({
      key: fs.readFileSync(TLS_KEY),
      cert: fs.readFileSync(TLS_CERT),
    }, handler)
  : http.createServer(handler);

server.on('upgrade', (req, socket) => {
  if (!isWsRequest(req)) {
    socket.destroy();
    return;
  }
  acceptUpgrade(
    req,
    socket,
    client => {
      clients.add(client);
      send(client, JSON.stringify({
        type: 'hello',
        server: 'tcmt-server',
        version: '0.2.0',
        publicUrl: PUBLIC_URL || `${SCHEME}://${HOST === '0.0.0.0' ? 'localhost' : HOST}:${PORT}`,
      }));
    },
    client => clients.delete(client),
    { authToken: AUTH_TOKEN }
  );
});

// Periodic device-list broadcast (matches the C++ server's cadence).
setInterval(() => {
  broadcast({ type: 'devices', data: store.list() });
}, 500);

server.listen(PORT, HOST, () => {
  console.log(`[tcmt-server] listening on ${SCHEME}://${HOST}:${PORT}`);
  console.log(`[tcmt-server] data dir : ${DATA_DIR}`);
  console.log(`[tcmt-server] viewer   : ${STATIC_DIR}`);
  if (PUBLIC_URL) console.log(`[tcmt-server] public   : ${PUBLIC_URL}`);
  if (AUTH_TOKEN) console.log('[tcmt-server] auth     : read APIs + /ws require Bearer <token> (--auth-token)');
  if (HOST === '0.0.0.0' || HOST === '::') {
    for (const iface of lanAddresses()) {
      console.log(`[tcmt-server] LAN URL   : ${SCHEME}://${iface.address}:${PORT}/  (viewer)`);
    }
    console.log(`[tcmt-server] client    : ./build/src/TCMT-M --http --server ${SCHEME}://<this-host>:${PORT}`);
    console.log(AUTH_TOKEN
      ? '[tcmt-server] read APIs are protected by --auth-token'
      : '[tcmt-server] WARNING: read APIs are unauthenticated; set --auth-token for public exposure.');
  }
  console.log('[tcmt-server] start TCMT-M with --http to feed it snapshots');
});

function lanAddresses() {
  const out = [];
  for (const [name, addrs] of Object.entries(os.networkInterfaces())) {
    for (const addr of addrs || []) {
      if (String(addr.family).includes('4') && !addr.internal) {
        out.push({ name, address: addr.address });
      }
    }
  }
  return out;
}

function shutdown() {
  server.close(() => process.exit(0));
  setTimeout(() => process.exit(0), 1000).unref();
}
process.on('SIGINT', shutdown);
process.on('SIGTERM', shutdown);
process.on('uncaughtException', err => console.error('[tcmt-server] uncaught:', err));
