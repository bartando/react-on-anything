// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
import assert from 'node:assert/strict';
import {test} from 'node:test';
import {frameAt, hitNode, ScreenTouch} from './screen-touch.js';

const SLOP = 12, STRIDE = 100;

/** What a finger at each point does: the presses and scrolls it causes, in order. */
function gesture(points, {hit = () => 'card', frameAt: at = (x, y, horizontal) => (horizontal ? 'row' : 'page')} = {}) {
  const done = [];
  const touch = new ScreenTouch({
    hit,
    frameAt: at,
    press: node => done.push(['press', node]),
    scroll: (frame, dx, dy) => done.push(['scroll', frame, dx, dy]),
    step: direction => done.push(['step', direction]),
  }, SLOP, STRIDE);
  for (const point of points) touch.touch(...point);
  return done;
}

test('a tap presses what is under it, once the finger lifts', () => {
  assert.deepEqual(gesture([[100, 200], [103, 198], [null]]), [['press', 'card']]);
  assert.deepEqual(gesture([[100, 200], [null]], {hit: () => null}), []);
});

test('the content follows a drag along its axis, starting where the slop ends', () => {
  const done = gesture([[300, 200], [300 - SLOP, 202], [300 - SLOP - 40, 210], [300 - SLOP - 100, 190], [null]]);
  assert.deepEqual(done, [['scroll', 'row', -40, 0], ['scroll', 'row', -60, 0]]);
  assert.deepEqual(gesture([[300, 200], [301, 200 + SLOP], [301, 200 + SLOP + 30]]), [['scroll', 'page', 0, 30]]);
});

test('with nothing to scroll, a drag steps focus against the finger, a step per stride', () => {
  const none = {frameAt: () => null};
  // Up by two and a half strides past the slop: what is below comes up, so focus goes down twice.
  assert.deepEqual(gesture([[300, 600], [300, 600 - SLOP], [300, 600 - SLOP - 2.5 * STRIDE], [null]], none),
                   [['step', 'down'], ['step', 'down']]);
  // Back over a step takes it back; a drag presses nothing.
  assert.deepEqual(gesture([[300, 600], [300 + SLOP, 600], [300 + SLOP - 1.2 * STRIDE, 600], [300 + SLOP - 0.5 * STRIDE, 600],
                            [null]], none), [['step', 'right'], ['step', 'left']]);
});

test('lifting the finger starts the next touch afresh', () => {
  assert.deepEqual(gesture([[300, 200], [300, 260], [null], [50, 50], [null]]),
                   [['press', 'card']]);
});

// A focus manager's nodes and frames as screen-touch reads them.
function node(rect, {frame = null, scope = {props: {}, parent: null}, disabled = false} = {}) {
  return {rect, frame, scope, props: {disabled}, dead: false};
}
const frame = (viewport, {x = 0, y = 0, horizontal = false, parent = null} = {}) =>
  ({viewport, x, y, parent, handle: 1, timer: null, props: {horizontal}});

test('the smallest reachable element under the finger is hit, scrolling applied', () => {
  const page = frame({x: 0, y: 0, width: 1280, height: 720}, {y: 300});
  const row = frame({x: 0, y: 600, width: 1280, height: 200}, {x: 100, horizontal: true, parent: page});
  const banner = node({x: 0, y: 0, width: 1280, height: 400});
  const card = node({x: 300, y: 620, width: 200, height: 120}, {frame: row});
  const off = node({x: 1500, y: 620, width: 200, height: 120}, {frame: row});
  const manager = {nodes: new Set([banner, card, off]), focused: null};
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
  const manager = {nodes: new Set([outside, inside, layer]), focused: inside};
  assert.equal(hitNode(manager, 100, 100), null);
  assert.equal(hitNode(manager, 900, 100), inside);
  assert.equal(hitNode(manager, 100, 550), null);
});

test('a drag scrolls the innermost ScrollView along its axis', () => {
  const page = frame({x: 0, y: 0, width: 1280, height: 720});
  const row = frame({x: 0, y: 400, width: 1280, height: 200}, {horizontal: true, parent: page});
  const manager = {nodes: new Set([node({x: 0, y: 420, width: 10, height: 10}, {frame: row})])};
  assert.equal(frameAt(manager, 100, 500, true), row);
  assert.equal(frameAt(manager, 100, 500, false), page);
  assert.equal(frameAt(manager, 100, 100, true), null);
});
