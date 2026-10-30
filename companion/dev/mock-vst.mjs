#!/usr/bin/env node
/**
 * Mock VST: serves the companion and the companion API (docs/companion-api.md) with simulated
 * traffic from js/demo.js. No dependencies beyond Node 18+.
 *
 *   node companion/dev/mock-vst.mjs [--port 8787] [--token devtoken] [--host 0.0.0.0]
 *
 * Then open the printed pairing URL on a phone on the same network (or in a desktop browser with
 * device emulation). It behaves like VST Studio with Remote companion turned on: Bearer or ?token=
 * auth on /api, /api/hello open, CORS allowed, Server-Sent Events on /api/live, and an MJPEG stream
 * (the demo frame re-sent at ~2 fps; live motion comes from the `tracks` events drawn by the app).
 *
 * VST Studio's implementation can be checked against the same contract by pointing the companion
 * at it instead.
 */
import http from 'node:http';
import { readFile } from 'node:fs/promises';
import { networkInterfaces } from 'node:os';
import { extname, join, normalize, dirname } from 'node:path';
import { fileURLToPath } from 'node:url';
import { createDemoVst } from '../js/demo.js';

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const arg = (name, dflt) => { const i = process.argv.indexOf(`--${name}`); return i > 0 ? process.argv[i + 1] : dflt; };
const PORT = Number(arg('port', 8787));
const HOST = arg('host', '0.0.0.0');
const TOKEN = arg('token', 'devtoken');

const TYPES = { '.html': 'text/html; charset=utf-8', '.js': 'text/javascript; charset=utf-8', '.mjs': 'text/javascript; charset=utf-8',
  '.css': 'text/css; charset=utf-8', '.json': 'application/json', '.webmanifest': 'application/manifest+json', '.svg': 'image/svg+xml',
  '.png': 'image/png', '.jpg': 'image/jpeg', '.mp4': 'video/mp4' };

const vst = createDemoVst();
const CORS = { 'Access-Control-Allow-Origin': '*', 'Access-Control-Allow-Headers': 'Authorization, Content-Type', 'Access-Control-Allow-Methods': 'GET, PUT, POST, OPTIONS' };

function send(res, status, body, type = 'application/json') {
  res.writeHead(status, { 'Content-Type': type, 'Cache-Control': 'no-store', ...CORS });
  res.end(type === 'application/json' ? JSON.stringify(body) : body);
}

/** Static file under companion/, refusing anything that escapes it. */
async function sendFile(res, rel, req) {
  const path = normalize(join(ROOT, rel));
  if (!path.startsWith(ROOT)) return send(res, 403, { error: 'Forbidden' });
  try {
    const data = await readFile(path);
    const type = TYPES[extname(path)] || 'application/octet-stream';
    // Videos need byte ranges for Safari to play them.
    const range = req.headers.range?.match(/bytes=(\d*)-(\d*)/);
    if (range && type === 'video/mp4') {
      const start = Number(range[1] || 0), end = range[2] ? Number(range[2]) : data.length - 1;
      res.writeHead(206, { 'Content-Type': type, 'Content-Range': `bytes ${start}-${end}/${data.length}`, 'Accept-Ranges': 'bytes', 'Content-Length': end - start + 1, ...CORS });
      return res.end(data.subarray(start, end + 1));
    }
    res.writeHead(200, { 'Content-Type': type, 'Content-Length': data.length, 'Accept-Ranges': 'bytes', ...CORS });
    res.end(data);
  } catch { send(res, 404, { error: 'Not found' }); }
}

const authorized = (req, url) => (req.headers.authorization === `Bearer ${TOKEN}`) || url.searchParams.get('token') === TOKEN;

async function readJson(req) {
  let s = '';
  for await (const chunk of req) s += chunk;
  try { return s ? JSON.parse(s) : {}; } catch { return null; }
}

const server = http.createServer(async (req, res) => {
  const url = new URL(req.url, 'http://x');
  const path = url.pathname;
  if (req.method === 'OPTIONS') return send(res, 204, '', 'text/plain');
  if (!path.startsWith('/api/')) return sendFile(res, path === '/' ? 'index.html' : path.slice(1), req);
  if (path === '/api/hello') return send(res, 200, vst.handle('GET', path).json);
  if (!authorized(req, url)) return send(res, 401, { error: 'Pair this device: scan the QR code in VST Studio' });

  if (path === '/api/live') {
    res.writeHead(200, { 'Content-Type': 'text/event-stream', 'Cache-Control': 'no-store', Connection: 'keep-alive', ...CORS });
    res.write('retry: 3000\n\n');
    const off = vst.subscribe((event, data) => res.write(`event: ${event}\ndata: ${JSON.stringify(data)}\n\n`));
    const ping = setInterval(() => res.write(': ping\n\n'), 15e3);
    req.on('close', () => { off(); clearInterval(ping); });
    return;
  }
  if (path === '/api/stream.mjpg') {
    const frame = await readFile(join(ROOT, 'demo/frame.jpg'));
    res.writeHead(200, { 'Content-Type': 'multipart/x-mixed-replace; boundary=frame', 'Cache-Control': 'no-store', ...CORS });
    const push = () => res.write(Buffer.concat([Buffer.from(`--frame\r\nContent-Type: image/jpeg\r\nContent-Length: ${frame.length}\r\n\r\n`), frame, Buffer.from('\r\n')]));
    push();
    const t = setInterval(push, 500);
    req.on('close', () => clearInterval(t));
    return;
  }

  const body = req.method === 'PUT' || req.method === 'POST' ? await readJson(req) : undefined;
  if (body === null) return send(res, 400, { error: 'Body must be JSON' });
  const r = vst.handle(req.method, path, url.searchParams, body);
  if (r.file) return sendFile(res, r.file, req);
  if (r.text != null) return send(res, r.status, r.text, r.type);
  return send(res, r.status, r.json);
});

server.listen(PORT, HOST, () => {
  const ips = Object.values(networkInterfaces()).flat().filter((i) => i && i.family === 'IPv4' && !i.internal).map((i) => i.address);
  console.log(`Mock VST on port ${PORT}. Pair a phone by opening:`);
  for (const ip of ips.length ? ips : ['localhost']) console.log(`  http://${ip}:${PORT}/#token=${TOKEN}`);
  console.log(`  http://localhost:${PORT}/#token=${TOKEN}`);
});
