#!/usr/bin/env node
// TCMT MCP → HTTP bridge
// Spawns TCMT-M --mcp, bridges its JSON-RPC stdio to HTTP REST on port 9876.
// Usage: node mcp-http-bridge.js [--port 9876] [--tcmt ./build/src/TCMT-M]

import { spawn } from 'child_process';
import { createServer } from 'http';
import { createInterface } from 'readline';

const PORT = parseInt(process.env.PORT || '9876');
const TCMT = process.env.TCMT || './build/src/TCMT-M';

// ── Spawn TCMT-M ──────────────────────────────────────
const tcmt = spawn(TCMT, ['--mcp'], { stdio: ['pipe', 'pipe', 'inherit'] });
const rl = createInterface({ input: tcmt.stdout });
const pending = new Map();

tcmt.on('error', err => console.error('[bridge] TCMT spawn failed:', err.message));
tcmt.on('exit', code => { console.error('[bridge] TCMT exited:', code); process.exit(code || 0); });

// Parse JSON-RPC responses from TCMT stdout
rl.on('line', line => {
  try {
    const j = JSON.parse(line);
    if (j.id != null && pending.has(j.id)) {
      const { resolve } = pending.get(j.id);
      pending.delete(j.id);
      resolve(j.result ?? j.error);
    }
  } catch (_) {}
});

// Send JSON-RPC to TCMT and wait for response
let nextId = 1;
function call(method, params = {}) {
  return new Promise((resolve, reject) => {
    const id = nextId++;
    const req = JSON.stringify({ jsonrpc: '2.0', id, method, params }) + '\n';
    pending.set(id, { resolve, reject });
    setTimeout(() => { if (pending.has(id)) { pending.delete(id); reject(new Error('timeout')); } }, 5000);
    tcmt.stdin.write(req);
  });
}

// ── HTTP server ───────────────────────────────────────
const server = createServer(async (req, res) => {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');
  if (req.method === 'OPTIONS') { res.writeHead(204); res.end(); return; }
  if (req.method !== 'POST') { res.writeHead(405); res.end('POST only'); return; }

  let body = '';
  req.on('data', c => body += c);
  req.on('end', async () => {
    try {
      const { method, params } = JSON.parse(body);
      const result = await call(method, params || {});
      res.writeHead(200, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ jsonrpc: '2.0', id: null, result }));
    } catch (e) {
      res.writeHead(500, { 'Content-Type': 'application/json' });
      res.end(JSON.stringify({ jsonrpc: '2.0', id: null, error: { code: -1, message: e.message } }));
    }
  });
});

server.listen(PORT, () => console.error(`[bridge] TCMT MCP → HTTP  on port ${PORT}`));
