// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import assert from 'node:assert/strict';
import {test} from 'node:test';
import {FocusManager} from './manager.js';
import {frameAt, hitNode, ScreenTouch} from './screen-touch.js';

const SLOP = 12, MIN_FLING = 0.3, FRAME_MS = 16;

/**
 * What a finger at each point, a frame apart, does: the taps, holds, scrolls and flings it causes, in
 * order. A point `{rest: ms}` lets that long pass; `coasting` is whether a fling is still going when
 * touched.
 */
function gesture(points, {hit = () => 'card', frameAt: at = (x, y, horizontal) => (horizontal ? 'row' : 'page'),
  coasting = true} = {}) {
  const done = [];
  let time = 0;
  const touch = new ScreenTouch({
    hit,
    frameAt: at,
    down: () => {},
    hold: node => done.push(['hold', node]),
    tap: node => done.push(['tap', node]),
    scroll: (frame, dx, dy) => done.push(['scroll', frame, dx, dy]),
    fling: (frame, vx, vy) => done.push(['fling', frame, vx, vy]),
    halt: frame => (done.push(['halt', frame]), coasting),
    grab: (node, phase, x) => done.push(['grab', phase, x]),
  }, SLOP, MIN_FLING, () => time);
  for (const point of points) {
    if (point.rest) time += point.rest;
    else {
      touch.touch(...point);
      time += FRAME_MS;
    }
  }
  return done;
}
const without = (done, ...kinds) => done.filter(([kind]) => !kinds.includes(kind));

test('a resting finger shows what it is on pressed, and lifting it there taps it', () => {
  assert.deepEqual(gesture([[100, 200], [103, 198], [null]]), [['hold', 'card'], ['hold', null], ['tap', 'card']]);
  // A tap on nothing focusable is still a tap, for a scope's onTap.
  assert.deepEqual(gesture([[100, 200], [null]], {hit: () => null}), [['hold', null], ['tap', null]]);
});

test('the content follows a drag along its axis, starting where the slop ends; the press is let go', () => {
  const done = gesture([[300, 200], [300 - SLOP, 202], [300 - SLOP - 40, 210], [300 - SLOP - 100, 190], {rest: 200}, [null]]);
  assert.deepEqual(done, [['hold', 'card'], ['hold', null], ['scroll', 'row', -40, 0], ['scroll', 'row', -60, 0]]);
  assert.deepEqual(without(gesture([[300, 200], [301, 200 + SLOP], [301, 200 + SLOP + 30]]), 'hold'),
                   [['scroll', 'page', 0, 30]]);
  // Where nothing scrolls that way, nothing moves.
  assert.deepEqual(without(gesture([[300, 600], [300, 600 - SLOP], [300, 300], [null]], {frameAt: () => null}), 'hold'), []);
});

test('a drag let go of while moving flings the content on at the finger\'s speed', () => {
  // 20 px a frame: 1.25 px per ms.
  const swipe = [[300, 200], [280, 200], [260, 200], [240, 200], [220, 200], [200, 200]];
  assert.deepEqual(without(gesture([...swipe, [null]]), 'scroll', 'hold'), [['fling', 'row', -1.25, 0]]);
  // A finger that stopped before lifting, or a slow drag, flings nothing.
  assert.deepEqual(without(gesture([...swipe, {rest: 200}, [null]]), 'scroll', 'hold'), []);
  const slow = Array.from({length: 12}, (_, i) => [300, 200 + SLOP + 2 * i]);
  assert.deepEqual(without(gesture([[300, 200], ...slow, [null]]), 'scroll', 'hold'), []);
});

test('a touch on a coasting list catches it and presses nothing', () => {
  const swipe = [[300, 600], [300, 570], [300, 540], [300, 510], [null]];
  assert.deepEqual(without(gesture([...swipe, [400, 300], [null]]), 'scroll', 'hold'),
                   [['fling', 'page', 0, -1.875], ['halt', 'page']]);
  // Once it has stopped by itself, the next tap presses as usual.
  assert.deepEqual(without(gesture([...swipe, [400, 300], [null]], {coasting: false}), 'scroll', 'hold'),
                   [['fling', 'page', 0, -1.875], ['halt', 'page'], ['tap', 'card']]);
});

test('an element with onDrag takes the finger from where it lands, without slop, tap or scroll', () => {
  const bar = {props: {onDrag() {}}};
  assert.deepEqual(gesture([[100, 650], [104, 652], [300, 640], [null]], {hit: () => bar}),
                   [['grab', 'start', 100], ['grab', 'move', 104], ['grab', 'move', 300], ['grab', 'end', 300]]);
});

test('lifting the finger starts the next touch afresh', () => {
  assert.deepEqual(without(gesture([[300, 200], [300, 260], {rest: 200}, [null], [50, 50], [null]]), 'hold', 'scroll'),
                   [['tap', 'card']]);
});

// A focus manager's nodes and frames as screen-touch reads them.
function node(rect, {frame = null, scope = {props: {}, parent: null}, disabled = false} = {}) {
  return {rect, frame, scope, props: {disabled}, dead: false};
}
// The focus manager's own trap lookup over stand-in nodes.
const managerOf = (nodes, focused = null) =>
  ({nodes: new Set(nodes), focused, trapScope: FocusManager.prototype.trapScope});
const frame = (viewport, {x = 0, y = 0, horizontal = false, parent = null} = {}) =>
  ({viewport, x, y, parent, handle: 1, timer: null, props: {horizontal}});

test('the smallest reachable element under the finger is hit, scrolling applied', () => {
  const page = frame({x: 0, y: 0, width: 1280, height: 720}, {y: 300});
  const row = frame({x: 0, y: 600, width: 1280, height: 200}, {x: 100, horizontal: true, parent: page});
  const banner = node({x: 0, y: 0, width: 1280, height: 400});
  const card = node({x: 300, y: 620, width: 200, height: 120}, {frame: row});
  const off = node({x: 1500, y: 620, width: 200, height: 120}, {frame: row});
  const manager = managerOf([banner, card, off]);
  // The card is at (200, 320) on screen: 100 scrolled sideways, 300 down.
  assert.equal(hitNode(manager, 250, 350), card);
  assert.equal(hitNode(manager, 50, 50), banner);
  assert.equal(hitNode(manager, 1450, 350), null); // `off` is scrolled out of the row's viewport
});

test('a tap does not reach past a trapping scope or into a hidden layer', () => {
  const root = {props: {}, parent: null};
  const panel = {props: {trap: true}, parent: root};
  const hidden = {props: {inert: true}, parent: root};
  const outside = node({x: 0, y: 0, width: 400, height: 400}, {scope: root});
  const inside = node({x: 800, y: 0, width: 400, height: 400}, {scope: panel});
  const layer = node({x: 0, y: 500, width: 400, height: 100}, {scope: hidden});
  const manager = managerOf([outside, inside, layer], inside);
  assert.equal(hitNode(manager, 100, 100), null);
  assert.equal(hitNode(manager, 900, 100), inside);
  assert.equal(hitNode(manager, 100, 550), null);
});

test('a drag scrolls the innermost ScrollView along its axis', () => {
  const page = frame({x: 0, y: 0, width: 1280, height: 720});
  const row = frame({x: 0, y: 400, width: 1280, height: 200}, {horizontal: true, parent: page});
  const manager = managerOf([node({x: 0, y: 420, width: 10, height: 10}, {frame: row})]);
  assert.equal(frameAt(manager, 100, 500, true), row);
  assert.equal(frameAt(manager, 100, 500, false), page);
  assert.equal(frameAt(manager, 100, 100, true), null);
});
