/**
 * Unit tests for the companion's pure helpers: HTML escaping and templating (js/ui.js), the stats maths
 * (js/views/stats.js), zone geometry (js/overlay.js) and address parsing (js/api.js). None of these touch the
 * DOM, so they run in plain Node.
 */
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { esc, raw, html, band, passPills } from '../js/ui.js';
import { pct, summarize } from '../js/views/stats.js';
import { geometry, applyHandle } from '../js/overlay.js';

describe('ui.js esc', () => {
  it('escapes the five HTML metacharacters', () => {
    assert.equal(esc(`<a href="x" title='y'>&</a>`), '&lt;a href=&quot;x&quot; title=&#39;y&#39;&gt;&amp;&lt;/a&gt;');
  });
  it('turns null and undefined into empty strings, and everything else into text', () => {
    assert.equal(esc(null), '');
    assert.equal(esc(undefined), '');
    assert.equal(esc(0), '0');
    assert.equal(esc(false), 'false');
  });
  it('escapes & first, so entities are not double-decoded', () => {
    assert.equal(esc('&lt;'), '&amp;lt;');
  });
});

describe('ui.js html', () => {
  it('escapes interpolated values', () => {
    const name = '<img src=x onerror=alert(1)>';
    assert.equal(html`<b>${name}</b>`.__html, '<b>&lt;img src=x onerror=alert(1)&gt;</b>');
  });
  it('escapes inside attributes', () => {
    assert.equal(html`<a title="${'" onclick="x'}">`.__html, '<a title="&quot; onclick=&quot;x">');
  });
  it('leaves raw() and nested templates alone, without double escaping', () => {
    const inner = html`<i>${'a&b'}</i>`;
    assert.equal(html`<p>${inner}${raw('<br>')}</p>`.__html, '<p><i>a&amp;b</i><br></p>');
  });
  it('joins arrays, escaping each plain item', () => {
    assert.equal(html`<ul>${['<x>', html`<li>ok</li>`]}</ul>`.__html, '<ul>&lt;x&gt;<li>ok</li></ul>');
  });
  it('drops false, null and undefined so conditionals read naturally', () => {
    assert.equal(html`a${false}b${null}c${undefined}d`.__html, 'abcd');
    assert.equal(html`${0}`.__html, '0');
  });
  it('escapes site names coming from the API', () => {
    assert.equal(html`<h1>${"7½ St <SW> & 'Main'"}</h1>`.__html, '<h1>7½ St &lt;SW&gt; &amp; &#39;Main&#39;</h1>');
  });
});

describe('ui.js band and passPills', () => {
  it('orders severity: wrong way, then way too fast, then speeding', () => {
    assert.equal(band({ wrongWay: true, egregious: true, speeding: true }), 'wrong');
    assert.equal(band({ egregious: true, speeding: true }), 'bad');
    assert.equal(band({ speeding: true }), 'warn');
    assert.equal(band({}), 'ok');
  });
  it('says how far over the limit a speeder was', () => {
    const pills = passPills({ speed: 31, speeding: true }, 25).map((p) => p.__html).join('');
    assert.match(pills, /\+6 over/);
    assert.doesNotMatch(passPills({ speed: 50, speeding: true, egregious: true }, 25).map((p) => p.__html).join(''), /over/);
  });
});

describe('stats.js percentile', () => {
  const sorted = Array.from({ length: 20 }, (_, i) => i + 1);   // 1..20
  it('uses the nearest rank', () => {
    assert.equal(pct(sorted, 85), 17);   // ceil(0.85 * 20) = 17th value
    assert.equal(pct(sorted, 50), 10);
    assert.equal(pct(sorted, 100), 20);
    assert.equal(pct(sorted, 1), 1);
  });
  it('handles tiny and empty inputs', () => {
    assert.equal(pct([], 85), 0);
    assert.equal(pct([30], 85), 30);
    assert.equal(pct([20, 40], 85), 40);
    assert.equal(pct([20, 40], 50), 20);
  });
});

describe('stats.js summarize', () => {
  const at = (h) => new Date(2026, 9, 30, h, 15).toISOString();
  const vs = [
    { speed: 22, time: at(8), wrongWay: false },
    { speed: 25, time: at(8), wrongWay: false },
    { speed: 26, time: at(9), wrongWay: false },
    { speed: 31, time: at(17), wrongWay: true },
    { speed: 44, time: at(17), wrongWay: false },
  ];
  const s = summarize(vs, 25, 40, true);
  it('computes the headline numbers', () => {
    assert.equal(s.n, 5);
    assert.equal(s.p85, 44);       // ceil(0.85 * 5) = 5th of 22, 25, 26, 31, 44
    assert.equal(s.median, 26);
    assert.equal(s.top, 44);
    assert.equal(s.pctSpeeding, 60);   // 26, 31, 44 are over 25
    assert.equal(s.egregious, 1);
    assert.equal(s.wrong, 1);
  });
  it('bins speeds and hours', () => {
    assert.equal(s.histogram.reduce((t, b) => t + b.n, 0), 5);
    assert.equal(s.histogram.find((b) => b.speed === 44).n, 1);
    assert.deepEqual(s.hours[8], { h: 8, ok: 2, over: 0 });
    assert.deepEqual(s.hours[17], { h: 17, ok: 0, over: 2 });
  });
  it('is all zeros with no passes', () => {
    const e = summarize([], 25, 40, true);
    assert.equal(e.n, 0);
    assert.equal(e.p85, 0);
    assert.equal(e.pctSpeeding, 0);
    assert.equal(e.top, 0);
  });
});

describe('overlay.js geometry', () => {
  const cfg = {
    AnalysisBoxLeft: 10, AnalysisBoxTop: 220, AnalysisBoxWidth: 1269, AnalysisBoxHeight: 190,
    speedLineLeft: 350, speedLineRight: 909, obstruction: [251, 311], R2LStreetY: 122, L2RStreetY: 158,
  };
  it('turns VST.cfg offsets into frame coordinates', () => {
    const g = geometry(cfg);
    assert.deepEqual(g.box, { x: 10, y: 220, w: 1269, h: 190 });
    assert.equal(g.sl, 360);
    assert.equal(g.sr, 919);
    assert.equal(g.r2l, 342);
    assert.equal(g.l2r, 378);
    assert.deepEqual(g.obs, [261, 321]);
    assert.equal(geometry({ ...cfg, obstruction: [0, 0] }).obs, null);
  });
  it('keeps dragged speed lines inside the box and apart', () => {
    assert.deepEqual(applyHandle(cfg, 'sr', 5000, 0), { speedLineRight: 1269 });
    assert.deepEqual(applyHandle(cfg, 'sl', 5000, 0), { speedLineLeft: 909 - 40 });
    assert.deepEqual(applyHandle(cfg, 'sl', -50, 0), { speedLineLeft: 0 });
  });
  it('keeps lines still on screen when the box corner moves', () => {
    const c = applyHandle(cfg, 'tl', 110, 240);
    const before = geometry(cfg), after = geometry({ ...cfg, ...c });
    assert.equal(after.sl, before.sl);
    assert.equal(after.r2l, before.r2l);
    assert.equal(after.box.x + after.box.w, before.box.x + before.box.w);
  });
});

describe('api.js normalizeBase', async () => {
  // api.js compares against the page's origin; give Node one before importing it.
  globalThis.location = { origin: 'http://phone.local:8000' };
  const { normalizeBase } = await import('../js/api.js');
  it('adds http:// and the default port', () => {
    assert.equal(normalizeBase('192.168.1.20'), 'http://192.168.1.20:8787');
    assert.equal(normalizeBase(' 192.168.1.20/ '), 'http://192.168.1.20:8787');
  });
  it('keeps an explicit port and scheme', () => {
    assert.equal(normalizeBase('192.168.1.20:9000'), 'http://192.168.1.20:9000');
    assert.equal(normalizeBase('https://vst.tailnet.ts.net'), 'https://vst.tailnet.ts.net');
  });
  it('is same-origin ("") for blank input or this page', () => {
    assert.equal(normalizeBase(''), '');
    assert.equal(normalizeBase('http://phone.local:8000/'), '');
  });
});
