// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// A touchscreen (the Switch's): a tap presses the element under the finger, as a phone's would, and a
// drag scrolls the ScrollView under it along the drag's axis, the content following the finger. Focus
// stays where it was through a drag; the next D-pad press goes on from there. Where no ScrollView
// scrolls that way (a page that moves to its focused row by itself), the drag steps focus instead, a
// move per stretch of travel against the finger, as the content would go. See docs/NAVIGATION.md.
import {screenRect, scrolledBy, shownIn} from './frames.js';

/**
 * @typedef {import('./manager.js').Node} Node
 * @typedef {import('./manager.js').Frame} Frame
 * @typedef {import('./manager.js').FocusManager} FocusManager
 */

export class ScreenTouch {
  /**
   * @param {{hit: (x: number, y: number) => Node | null, frameAt: (x: number, y: number, horizontal: boolean) => Frame | null,
   *   press: (node: Node) => void, scroll: (frame: Frame, dx: number, dy: number) => void,
   *   step: (direction: import('./geometry.js').Direction) => void}} actions
   * @param {number} slop travel in screen px before a touch is a drag rather than a tap
   * @param {number} stride travel in screen px per focus step, where a drag has nothing to scroll
   */
  constructor(actions, slop, stride) {
    this.actions = actions;
    this.slop = slop;
    this.stride = stride;
    this.release();
  }

  /** The finger at (x, y) in screen px; `null` x when it lifts. */
  touch(x, y) {
    if (x == null) {
      if (this.start && !this.dragging) {
        const node = this.actions.hit(this.start.x, this.start.y);
        if (node) this.actions.press(node);
      }
      return this.release();
    }
    if (!this.start) {
      this.start = this.last = {x, y};
      return undefined;
    }
    if (!this.dragging) {
      const dx = x - this.start.x, dy = y - this.start.y;
      if (Math.max(Math.abs(dx), Math.abs(dy)) < this.slop) return undefined;
      this.dragging = true;
      this.horizontal = Math.abs(dx) >= Math.abs(dy);
      this.frame = this.actions.frameAt(this.start.x, this.start.y, this.horizontal);
      // The drag starts where the finger left the slop, so the content does not jump by it.
      this.last = {x, y};
      return undefined;
    }
    const moved = this.horizontal ? x - this.last.x : y - this.last.y;
    this.last = {x, y};
    if (this.frame) {
      if (moved) this.actions.scroll(this.frame, this.horizontal ? moved : 0, this.horizontal ? 0 : moved);
      return undefined;
    }
    // Steps against the finger: dragging up brings what is below, so focus goes down.
    this.travel += moved;
    const [back, forward] = this.horizontal ? ['left', 'right'] : ['up', 'down'];
    const due = Math.trunc(-this.travel / this.stride);
    for (; this.taken < due; this.taken++) this.actions.step(forward);
    for (; this.taken > due; this.taken--) this.actions.step(back);
    return undefined;
  }

  release() {
    this.start = this.last = null;
    this.dragging = false;
    this.horizontal = false;
    this.frame = null;
    this.travel = 0;
    this.taken = 0;
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

/** The trapping scope focus is in, if any: a tap does not reach past it. @param {FocusManager} manager */
function trapOf(manager) {
  for (let scope = manager.focused?.scope; scope; scope = scope.parent) if (scope.props.trap) return scope;
  return null;
}

/**
 * The focusable element at (x, y) on screen: the smallest one there that the D-pad could reach, shown
 * through its ScrollViews and not in a hidden layer.
 * @param {FocusManager} manager @returns {Node | null}
 */
export function hitNode(manager, x, y) {
  const trap = trapOf(manager);
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
