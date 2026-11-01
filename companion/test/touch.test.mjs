/**
 * Unit tests for the phone editing maths: pan/zoom (js/touch.js zoomAbout, clampView), the car-length ruler
 * (rulerBlocks, dragRulerEnd) and where each zone handle is drawn (js/overlay.js handlePoint), which the nudge
 * arrows and offset drags start from. The DOM parts of touch.js are exercised by driving the app with touch
 * emulation; these pin the arithmetic they rely on.
 */
import { describe, it } from 'node:test';
import assert from 'node:assert/strict';
import { zoomAbout, clampView, rulerBlocks, dragRulerEnd, blockPx, nFromBlock, MAX_ZOOM } from '../js/touch.js';
import { applyHandle, handlePoint, HANDLE_AXES, W } from '../js/overlay.js';

const near = (a, b, eps = 1e-6) => assert.ok(Math.abs(a - b) < eps, `${a} != ${b}`);

describe('touch.js zoomAbout', () => {
  it('keeps the point under the fingers fixed', () => {
    const v = zoomAbout({ z: 1, tx: 0, ty: 0 }, 2, 100, 50, 400, 225);
    // Stage point (100, 50) must still be at screen (100, 50): 100 = tx + 100 * 2.
    near(v.tx + 100 * v.z, 100);
    near(v.ty + 50 * v.z, 50);
  });
  it('clamps zoom to 1..MAX_ZOOM and never shows a black edge', () => {
    assert.equal(zoomAbout({ z: 1, tx: 0, ty: 0 }, 0.3, 0, 0, 400, 225).z, 1);
    assert.equal(zoomAbout({ z: 1, tx: 0, ty: 0 }, 99, 0, 0, 400, 225).z, MAX_ZOOM);
    const v = clampView({ z: 2, tx: 50, ty: -9999 }, 400, 225);
    assert.deepEqual(v, { z: 2, tx: 0, ty: -225 });
  });
});

describe('touch.js car-length ruler', () => {
  it('converts between car lengths and block width over the 1280 px frame', () => {
    assert.equal(blockPx(4), 320);
    assert.equal(nFromBlock(320), 4);
    assert.equal(nFromBlock(5000), 1);    // never under one car across
    assert.equal(nFromBlock(10), 20);     // nor over twenty
  });
  it('tiles blocks across the whole frame, phase-locked to the offset measuring block', () => {
    const b = rulerBlocks(4, 100);
    assert.equal(b.filter((x) => x.main).length, 1);
    assert.equal(b.find((x) => x.main).x, 100);
    assert.ok(b[0].x <= 0 && b[0].x > -320, 'first block starts at or left of the frame edge');
    assert.ok(b.at(-1).x + b.at(-1).w >= W, 'last block reaches the right edge');
  });
  it('moves one end and keeps the other where it is', () => {
    const r = dragRulerEnd(4, 100, 'right', 100 + 256);   // 256 px block = 5 car lengths
    near(r.n, 5); assert.equal(r.x, 100);
    const l = dragRulerEnd(4, 100, 'left', 260);          // right end stays at 420
    near(l.x + blockPx(l.n), 420); near(l.n, W / 160);
  });
  it('stops an end dragged past the other one at the shortest block', () => {
    const r = dragRulerEnd(4, 100, 'right', 0);
    near(r.n, 20);
  });
});

describe('overlay.js handlePoint', () => {
  const cfg = {
    AnalysisBoxLeft: 100, AnalysisBoxTop: 200, AnalysisBoxWidth: 1000, AnalysisBoxHeight: 200,
    speedLineLeft: 200, speedLineRight: 800, R2LStreetY: 80, L2RStreetY: 140, R2LStreetYRight: 0, L2RStreetYRight: 0,
    obstruction: [300, 360],
  };
  it('puts handles where zonesSvg draws them', () => {
    assert.deepEqual(handlePoint(cfg, 'tl'), [100, 200]);
    assert.deepEqual(handlePoint(cfg, 'br'), [1100, 400]);
    assert.deepEqual(handlePoint(cfg, 'sl'), [300, 300]);
    assert.deepEqual(handlePoint(cfg, 'sr'), [900, 300]);
    assert.equal(handlePoint(cfg, 'r2l')[1], 280);
    assert.equal(handlePoint(cfg, 'o0')[0], 400);
    assert.equal(handlePoint({ ...cfg, obstruction: [0, 0] }, 'o0'), null);
  });
  it('round-trips through applyHandle, so a one-pixel nudge moves a handle one pixel', () => {
    for (const h of Object.keys(HANDLE_AXES)) {
      const [x, y] = handlePoint(cfg, h);
      const dx = HANDLE_AXES[h].includes('x') ? 1 : 0, dy = HANDLE_AXES[h].includes('y') ? 1 : 0;
      const moved = { ...cfg, ...applyHandle(cfg, h, x + dx, y + dy) };
      const [nx, ny] = handlePoint(moved, h);
      near(nx, x + dx, 0.51); near(ny, y + dy, 0.51);
    }
  });
});
