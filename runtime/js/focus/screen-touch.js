// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// A touchscreen (the Switch's), as on a phone: the content follows the finger, detached from focus. A
// finger resting on an element shows it pressed, and lifting it there presses it; a drag scrolls the
// ScrollView under it along the drag's axis, and let go of while moving, flings it on; a touch on a
// coasting list catches it. An element with `onDrag` (a seek bar) takes the finger itself from where it
// lands until it lifts. Focus is neither shown nor revealed meanwhile: the next button press brings
// it back (FocusManager.resume). Where nothing scrolls along the drag, nothing moves. See
// docs/NAVIGATION.md.
import {screenRect, scrolledBy, shownIn} from './frames.js';

/**
 * @typedef {import('./manager.js').Node} Node
 * @typedef {import('./manager.js').Frame} Frame
 * @typedef {import('./manager.js').FocusManager} FocusManager
 */

// The finger's speed at release is its travel over the last VELOCITY_MS; one that rested longer than
// REST_MS before lifting flings nothing.
const VELOCITY_MS = 100;
const REST_MS = 50;

export class ScreenTouch {
  /**
   * @param {{hit: (x: number, y: number) => Node | null, frameAt: (x: number, y: number, horizontal: boolean) => Frame | null,
   *   down: () => void, hold: (node: Node | null) => void, tap: (node: Node | null) => void,
   *   grab: (node: Node, phase: 'start' | 'move' | 'end', x: number, y: number) => void,
   *   scroll: (frame: Frame, dx: number, dy: number) => void,
   *   fling: (frame: Frame, vx: number, vy: number) => void, halt: (frame: Frame) => boolean}} actions
   *   `down` when a finger lands; `hold` shows what it rests on pressed (null: nothing); `tap` on an
   *   element or on nothing focusable (null); `fling` in screen px per ms; `halt` stops a fling and says
   *   whether there was one
   * @param {number} slop travel in screen px before a touch is a drag rather than a tap
   * @param {number} minFling the slowest release in screen px per ms that flings
   * @param {() => number} [now] ms
   */
  constructor(actions, slop, minFling, now = () => performance.now()) {
    this.actions = actions;
    this.slop = slop;
    this.minFling = minFling;
    this.now = now;
    this.flung = null;
    this.release();
  }

  /** The finger at (x, y) in screen px; `null` x when it lifts. */
  touch(x, y) {
    const time = this.now();
    if (x == null) {
      if (this.grabbed) this.actions.grab(this.grabbed, 'end', this.last.x, this.last.y);
      else if (this.start && !this.dragging && !this.caught) {
        this.actions.hold(null);
        this.actions.tap(this.target);
      }
      if (this.dragging && this.frame) this.letGo(time);
      return this.release();
    }
    this.samples.push({time, x, y});
    while (this.samples.length > 2 && time - this.samples[0].time > VELOCITY_MS) this.samples.shift();
    if (!this.start) {
      this.start = this.last = {x, y};
      this.actions.down();
      // A touch on a coasting list stops it there and presses nothing, as on a phone.
      this.caught = Boolean(this.flung && this.actions.halt(this.flung));
      this.flung = null;
      this.target = this.caught ? null : this.actions.hit(x, y);
      if (this.target?.props?.onDrag) {
        this.grabbed = this.target;
        this.target = null;
        this.actions.grab(this.grabbed, 'start', x, y);
      } else if (this.target) this.actions.hold(this.target);
      return undefined;
    }
    if (this.grabbed) {
      this.last = {x, y};
      this.actions.grab(this.grabbed, 'move', x, y);
      return undefined;
    }
    if (!this.dragging) {
      const dx = x - this.start.x, dy = y - this.start.y;
      if (Math.max(Math.abs(dx), Math.abs(dy)) < this.slop) return undefined;
      this.dragging = true;
      if (this.target) this.actions.hold(null);
      this.target = null;
      this.horizontal = Math.abs(dx) >= Math.abs(dy);
      this.frame = this.actions.frameAt(this.start.x, this.start.y, this.horizontal);
      // The drag starts where the finger left the slop, so the content does not jump by it.
      this.last = {x, y};
      return undefined;
    }
    const moved = this.horizontal ? x - this.last.x : y - this.last.y;
    this.last = {x, y};
    if (this.frame && moved) this.actions.scroll(this.frame, this.horizontal ? moved : 0, this.horizontal ? 0 : moved);
    return undefined;
  }

  // Flings the dragged view at the finger's speed along the drag's axis, if it was still moving.
  letGo(time) {
    const first = this.samples[0], last = this.samples[this.samples.length - 1];
    if (!first || last === first || time - last.time > REST_MS) return;
    const speed = (this.horizontal ? last.x - first.x : last.y - first.y) / (last.time - first.time);
    if (!(Math.abs(speed) >= this.minFling)) return;
    this.actions.fling(this.frame, this.horizontal ? speed : 0, this.horizontal ? 0 : speed);
    this.flung = this.frame;
  }

  release() {
    this.samples = [];
    this.target = null;
    this.grabbed = null;
    this.caught = false;
    this.start = this.last = null;
    this.dragging = false;
    this.horizontal = false;
    this.frame = null;
  }
}

function inert(scope) {
  for (; scope; scope = scope.parent) if (scope.props.inert) return true;
  return false;
}

function within(scope, ancestor) {
  for (; scope; scope = scope.parent) if (scope === ancestor) return true;
  return false;
}

/**
 * The focusable element at (x, y) on screen: the smallest one there that the D-pad could reach, shown
 * through its ScrollViews and not in a hidden layer.
 * @param {FocusManager} manager @returns {Node | null}
 */
export function hitNode(manager, x, y) {
  // A tap does not reach past the trapping scope focus is in.
  const trap = manager.trapScope();
  const point = {x, y, width: 1, height: 1};
  let best = null, area = Infinity;
  for (const node of manager.nodes) {
    if (node.dead || node.props.disabled || inert(node.scope) || (trap && !within(node.scope, trap))) continue;
    const rect = screenRect(node);
    if (!rect || x < rect.x || y < rect.y || x >= rect.x + rect.width || y >= rect.y + rect.height) continue;
    if (!shownIn(node.rect, node.frame, point)) continue;
    const size = rect.width * rect.height;
    if (size < area) [best, area] = [node, size];
  }
  return best;
}

/** `frame`'s viewport on screen, its ancestors' scrolling applied. @param {Frame} frame */
function frameRect(frame) {
  let rect = frame.viewport;
  for (let parent = frame.parent; rect && parent; parent = parent.parent) rect = scrolledBy(rect, parent);
  return rect;
}

/**
 * The innermost ScrollView at (x, y) that scrolls along the axis: horizontal ones for a sideways drag,
 * the others for an up or down one. @param {FocusManager} manager @returns {Frame | null}
 */
export function frameAt(manager, x, y, horizontal) {
  const frames = new Set();
  for (const node of manager.nodes) {
    if (inert(node.scope)) continue;
    for (let frame = node.frame; frame; frame = frame.parent) frames.add(frame);
  }
  let best = null, depth = -1;
  for (const frame of frames) {
    if (Boolean(frame.props.horizontal) !== horizontal || frame.handle == null) continue;
    const rect = frameRect(frame);
    if (!rect || x < rect.x || y < rect.y || x >= rect.x + rect.width || y >= rect.y + rect.height) continue;
    let level = 0;
    for (let parent = frame.parent; parent; parent = parent.parent) level++;
    if (level > depth) [best, depth] = [frame, level];
  }
  return best;
}
