/**
 * Where the contract tests send their requests.
 *
 *   node --test companion/test/                                   the mock (dev/mock-vst.mjs), started here
 *   BASE=http://127.0.0.1:8787 TOKEN=<pairing code> node --test companion/test/    a running VST Studio
 *
 * Against a real VST Studio the tests only read, plus requests VST must refuse (bad config, bad actions), so they
 * are safe to point at a live install. MUTATE=1 also runs the ones that change things (a valid PUT /api/config,
 * POST /api/run); those always run against the mock.
 */
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const MOCK = fileURLToPath(new URL('../dev/mock-vst.mjs', import.meta.url));

export async function startTarget() {
  if (process.env.BASE) {
    return { base: process.env.BASE.replace(/\/+$/, ''), token: process.env.TOKEN || '', mock: false, mutate: process.env.MUTATE === '1', stop: async () => {} };
  }
  const token = `test-${Math.random().toString(36).slice(2)}`;
  const child = spawn(process.execPath, [MOCK, '--port', '0', '--host', '127.0.0.1', '--token', token], { stdio: ['ignore', 'pipe', 'inherit'] });
  // The mock's first line is "Mock VST on port N. ..." once it is listening.
  const port = await new Promise((resolve, reject) => {
    let out = '';
    const timer = setTimeout(() => reject(new Error(`mock didn't start: ${out}`)), 10e3);
    child.stdout.on('data', (d) => {
      out += d;
      const m = out.match(/Mock VST on port (\d+)/);
      if (m) { clearTimeout(timer); resolve(Number(m[1])); }
    });
    child.on('exit', (code) => { clearTimeout(timer); reject(new Error(`mock exited (${code}): ${out}`)); });
  });
  child.stdout.resume();
  return {
    base: `http://127.0.0.1:${port}`, token, mock: true, mutate: true,
    stop: () => new Promise((resolve) => { child.once('exit', resolve); child.kill(); }),
  };
}

/** A small client: `t.get('/api/status')` with the token, `{ auth: false }` without, `{ token }` to use another. */
export function client(t) {
  async function call(method, path, { body, auth = true, token = t.token, raw } = {}) {
    const headers = {};
    if (auth) headers.Authorization = `Bearer ${token}`;
    if (body !== undefined) headers['Content-Type'] = 'application/json';
    const res = await fetch(`${t.base}${path}`, { method, headers, body: body === undefined ? undefined : typeof body === 'string' ? body : JSON.stringify(body) });
    const type = res.headers.get('content-type') || '';
    const data = raw ? Buffer.from(await res.arrayBuffer()) : type.includes('json') ? await res.json() : await res.text();
    return { status: res.status, type, headers: res.headers, data };
  }
  return {
    get: (p, o) => call('GET', p, o),
    put: (p, body, o) => call('PUT', p, { ...o, body }),
    post: (p, body, o) => call('POST', p, { ...o, body }),
    options: (p, o) => call('OPTIONS', p, { auth: false, ...o }),
  };
}

/**
 * Reads Server-Sent Events from `path` until `want(events)` is true or `ms` passes, then closes the stream.
 * Returns the events as `{ event, data }` with data parsed from JSON.
 */
export async function readEvents(t, path, want, ms = 4000) {
  const ac = new AbortController();
  const timer = setTimeout(() => ac.abort(), ms);
  const events = [];
  try {
    const res = await fetch(`${t.base}${path}`, { signal: ac.signal });
    const decoder = new TextDecoder();
    let buf = '';
    for await (const chunk of res.body) {
      buf += decoder.decode(chunk, { stream: true });
      // Events end with a blank line; each has "event:" and "data:" lines (comments and retry: are skipped).
      let end;
      while ((end = buf.indexOf('\n\n')) >= 0) {
        const block = buf.slice(0, end);
        buf = buf.slice(end + 2);
        const event = block.match(/^event: (.*)$/m)?.[1];
        const data = block.match(/^data: (.*)$/m)?.[1];
        if (event && data) events.push({ event, data: JSON.parse(data) });
      }
      if (want(events)) break;
    }
  } catch (e) {
    if (e.name !== 'AbortError') throw e;
  } finally {
    clearTimeout(timer);
    ac.abort();
  }
  return events;
}
