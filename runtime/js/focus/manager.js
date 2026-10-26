// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Focus state without React: focusable nodes, nested scopes, D-pad moves, press, back and recovery.
// Components keep one node or scope each and re-render only when their own node changes.
import {reachable, screenRect, scrolledBy, shownIn} from './frames.js';
import {findClosest, findNearest, findWrap} from './geometry.js';

/**
 * @typedef {import('./geometry.js').Rect} Rect
 * @typedef {import('./geometry.js').Direction} Direction
 * @typedef {{parent: Frame | null, x: number, y: number, viewport: Rect | null,
 *   reveal: (rect: Rect, anchor: Rect | null) => void}} Frame
 *   A scrolling ancestor; `x`/`y` is how far its content is scrolled, `viewport` its pre-scroll rectangle.
 * @typedef {{parent: Anchor | null, frame: Frame | null, rect: Rect | null}} Anchor
 *   An element with `scrollAnchor`, whose start its ScrollView aligns when revealing a descendant.
 * @typedef {{focusKey?: string, onPress?: () => void, onFocus?: () => void, onBlur?: () => void,
 *   autoFocus?: boolean, disabled?: boolean, nextFocusUp?: string, nextFocusDown?: string,
 *   nextFocusLeft?: string, nextFocusRight?: string, onDrag?: (drag: Drag) => void}} NodeProps
 *   `onDrag`: a finger landing on the element grabs it, as a slider's thumb, rather than tapping it or
 *   scrolling what is around it (see screen-touch.js).
 * @typedef {{phase: 'start' | 'move' | 'end', x: number, y: number, rect: Rect | null}} Drag
 *   The finger in screen px, and the element's rectangle on screen.
 * @typedef {{focusKey?: string, autoFocus?: boolean, trap?: boolean, wrap?: boolean, row?: boolean,
 *   restoreFocus?: boolean, inert?: boolean, onBack?: () => boolean | void,
 *   onAction?: (action: Action) => boolean | void, onTap?: (tap: {node: Node | null}) => boolean | void}} ScopeProps
 *   `inert` takes the scope's subtree out of navigation (a hidden layer that stays mounted). `onTap` sees
 *   a touchscreen tap first and takes it by returning true (see `tap`).
 * @typedef {import('../input.js').ButtonAction} Action a button beyond D-pad, Cross and Circle
 * @typedef {{kind: 'scope', parent: Scope | null, props: ScopeProps, remembered: Node | null,
 *   returnTo: Node | null, dead: boolean}} Scope
 * @typedef {{kind: 'node', scope: Scope, frame: Frame | null, anchor: Anchor | null, props: NodeProps,
 *   rect: Rect | null, order: number, pressed: boolean, dead: boolean, listeners: Set<() => void>,
 *   pressTimer: unknown}} Node
 */

const NEXT = {up: 'nextFocusUp', down: 'nextFocusDown', left: 'nextFocusLeft', right: 'nextFocusRight'};

/** @param {Scope | null} scope @param {Scope} ancestor */
function within(scope, ancestor) {
  for (; scope; scope = scope.parent) if (scope === ancestor) return true;
  return false;
}

/** Whether no scope from `scope` up is inert. @param {Scope | null} scope */
function navigable(scope) {
  for (; scope; scope = scope.parent) if (scope.props.inert) return false;
  return true;
}

export class FocusManager {
  /**
   * @param {{schedule?: (fn: () => void) => void, setTimer?: typeof setTimeout,
   *   clearTimer?: typeof clearTimeout, pressMs?: number}} [options]
   *   `schedule` runs focus recovery after the commit that unmounted the focused node.
   */
  constructor({schedule = fn => { Promise.resolve().then(fn); }, setTimer = setTimeout,
    clearTimer = clearTimeout, pressMs = 120} = {}) {
    Object.assign(this, {schedule, setTimer, clearTimer, pressMs});
    this.root = this.createScope(null, {});
    /** @type {Set<Node>} */
    this.nodes = new Set();
    /** @type {Map<string, Node | Scope>} */
    this.keys = new Map();
    /** @type {Node | null} */
    this.focused = null;
    /** @type {Set<() => void>} */
    this.listeners = new Set();
    this.order = 0;
    /** @type {Set<object>} owners about to focus something that is still mounting */
    this.claims = new Set();
    /** What recovery would have started from while a claim held it off. */
    this.lostWhileClaimed = null;
    /** Since the touchscreen was last used, until a button: focus is kept but neither shown nor revealed. */
    this.touching = false;
    /** @type {Node | null} what a resting finger shows pressed */
    this.held = null;
    /** @type {Set<(touching: boolean) => void>} */
    this.modeListeners = new Set();
  }

  /** Whether `node` shows as focused: it has focus, and the controller rather than a finger is in use. */
  shows(node) {
    return this.focused === node && !this.touching;
  }

  /** The touchscreen took over (true) or a button took back (false). @param {boolean} touching */
  setTouching(touching) {
    if (this.touching === touching) return;
    this.touching = touching;
    if (this.focused) this.notify(this.focused);
    for (const listener of this.modeListeners) listener(touching);
  }

  /**
   * The first button after the touchscreen: focus shows again where it was, scrolled into view, unless a
   * finger scrolled it off the screen, in which case it goes to the element nearest the screen's middle.
   * @param {Rect} bounds the screen
   */
  resume(bounds) {
    // Judged where the finger left things, before the pages it scrolled hand back to the app.
    const node = this.focused;
    const shown = candidate => shownIn(candidate.node.rect, candidate.node.frame, bounds);
    // Not on a layer now covered by another (a page under the player a tap opened).
    const stays = node && !node.dead && navigable(node.scope) && shown({node});
    const nearest = stays ? null : findClosest(bounds, this.candidates(this.trapScope() ?? this.root).filter(shown));
    this.setTouching(false);
    if (stays) this.reveal(node);
    else if (nearest) this.setFocus(nearest.node);
  }

  /**
   * Shows `node` pressed while a finger rests on it, and no longer once it lifts or drags (null).
   * @param {Node | null} node
   */
  hold(node) {
    const held = this.held;
    if (held === node) return;
    this.held = node;
    if (held && !held.dead) {
      held.pressed = false;
      this.notify(held);
    }
    if (node && !node.dead) {
      this.clearTimer(node.pressTimer);
      node.pressed = true;
      this.notify(node);
    }
  }

  /**
   * The touchscreen's tap, on `node` or on nothing focusable (null). Each scope from the node's (else
   * the trapping one focus is in, else the root) outward may take it first with `onTap`, as a video
   * player brings its hidden controls back rather than pressing what is under them. Otherwise `node`
   * takes focus, unshown, and its onPress runs after the press feedback Cross gives.
   * @param {Node | null} node @returns {boolean | null} as press(); null when nothing was pressed
   */
  tap(node) {
    this.hold(null);
    for (let scope = node?.scope ?? this.trapScope() ?? this.root; scope; scope = scope.parent) {
      if (scope.props.onTap?.({node}) === true) return null;
    }
    if (!node || !this.focus(node)) return null;
    return this.press();
  }

  /** The `trap` scope focus is in, if any. @returns {Scope | null} */
  trapScope() {
    for (let scope = this.focused?.scope; scope; scope = scope.parent) if (scope.props.trap) return scope;
    return null;
  }

  /**
   * Holds focus recovery and D-pad moves from nowhere off while `owner` (a VirtualList jumping to an
   * item it is still mounting) is about to focus something: otherwise the focused row unmounting
   * under the jump sends focus to whatever survives nearest, often another page's tab. The returned
   * release recovers as usual if nothing took focus meanwhile.
   * @param {object} owner @returns {() => void} release
   */
  claimFocus(owner) {
    this.claims.add(owner);
    return () => {
      if (!this.claims.delete(owner) || this.claims.size || this.focused) return;
      const lost = this.lostWhileClaimed;
      this.lostWhileClaimed = null;
      if (lost) this.recover(lost);
    };
  }

  /** @param {Scope | null} parent @param {ScopeProps} props @returns {Scope} */
  createScope(parent, props) {
    return {kind: 'scope', parent, props, remembered: null, returnTo: null, dead: false};
  }

  /**
   * Called while rendering, so `order` follows document order for elements mounted together.
   * @param {Scope} scope @param {Frame | null} frame @param {NodeProps} props
   * @param {Anchor | null} [anchor] the nearest `scrollAnchor` ancestor (or the element itself) @returns {Node}
   */
  createNode(scope, frame, props, anchor = null) {
    return {kind: 'node', scope, frame, anchor, props, rect: null, order: this.order++, pressed: false, dead: false,
      listeners: new Set(), pressTimer: null};
  }

  /** Replaces an entry's props, keeping its `focusKey` registered. @param {Node | Scope} entry @param {object} props */
  setProps(entry, props) {
    const previous = entry.props.focusKey;
    const becameInert = props.inert && !entry.props.inert;
    entry.props = props;
    // Deferred like recovery, so a screen mounted in the same commit can take focus first.
    if (becameInert) this.schedule(() => this.evict());
    if (entry.dead || previous === props.focusKey) return;
    if (previous != null && this.keys.get(previous) === entry) this.keys.delete(previous);
    if (props.focusKey != null) this.keys.set(props.focusKey, entry);
  }

  /** @param {Node} node */
  register(node) {
    node.dead = false;
    this.nodes.add(node);
    if (node.props.focusKey != null) this.keys.set(node.props.focusKey, node);
    if (node.props.autoFocus && navigable(node.scope) && !(this.focused && within(this.focused.scope, node.scope))) {
      this.setFocus(node);
    }
  }

  /** @param {Node} node */
  unregister(node) {
    node.dead = true;
    this.nodes.delete(node);
    if (this.keys.get(node.props.focusKey) === node) this.keys.delete(node.props.focusKey);
    this.clearTimer(node.pressTimer);
    if (this.focused !== node) return;
    this.focused = null;
    const lost = {scope: node.scope, rect: screenRect(node)};
    this.schedule(() => this.recover(lost));
  }

  /** @param {Scope} scope */
  mountScope(scope) {
    scope.dead = false;
    if (scope.props.focusKey != null) this.keys.set(scope.props.focusKey, scope);
    if (scope.props.autoFocus && navigable(scope) && !(this.focused && within(this.focused.scope, scope))) {
      const target = this.firstOf(scope);
      if (target) this.setFocus(target);
    }
  }

  /** @param {Scope} scope */
  unmountScope(scope) {
    scope.dead = true;
    if (this.keys.get(scope.props.focusKey) === scope) this.keys.delete(scope.props.focusKey);
  }

  /** Laid-out screen rectangle, before transforms and scrolling. @param {Node} node @param {Rect} rect */
  setRect(node, rect) {
    node.rect = rect;
  }

  /** @param {() => void} listener @returns {() => void} */
  subscribe(listener) {
    this.listeners.add(listener);
    return () => { this.listeners.delete(listener); };
  }

  /**
   * Focuses a node, a scope (its remembered or first element), or the entry registered under a key.
   * @param {Node | Scope | string} target
   * @returns {boolean} whether something took focus
   */
  focus(target) {
    const entry = typeof target === 'string' ? this.keys.get(target) : target;
    const node = entry?.kind === 'scope' ? this.firstOf(entry) : entry;
    if (!node || node.dead || !navigable(node.scope)) return false;
    this.setFocus(node);
    return true;
  }

  blur() {
    this.setFocus(null);
  }

  /** @param {Direction} direction @returns {boolean} whether focus moved */
  move(direction) {
    const current = this.focused;
    if (!current) return this.claims.size ? false : this.focus(this.root);
    const override = current.props[NEXT[direction]];
    if (override != null) return this.focus(override);
    const from = screenRect(current);
    if (!from) return false;
    const frames = new Set();
    for (let frame = current.frame; frame; frame = frame.parent) frames.add(frame);
    const chain = [];
    for (let scope = current.scope; scope; scope = scope.parent) chain.push(scope);
    const placed = this.placed(chain).filter(candidate => candidate.node !== current && reachable(candidate.node, frames));
    for (const [level, scope] of chain.entries()) {
      const candidates = placed.filter(candidate => candidate.level <= level);
      const winner = findNearest(from, candidates, direction)
        ?? (scope.props.wrap ? findWrap(from, candidates, direction) : null);
      if (winner) {
        this.setFocus(this.entryPoint(winner.node, current));
        return true;
      }
      if (scope.props.trap) return false;
      // A row ends at its ends: Left and Right never land in the rows around it.
      if (scope.props.row && (direction === 'left' || direction === 'right')) return false;
    }
    return false;
  }

  /**
   * Cross: press feedback for `pressMs`, then the focused element's onPress.
   * @returns {boolean | null} whether an enabled element with `onPress` took it; null without focus
   */
  press() {
    const node = this.focused;
    if (!node) return null;
    this.clearTimer(node.pressTimer);
    node.pressed = true;
    node.pressTimer = this.setTimer(() => {
      node.pressed = false;
      this.notify(node);
    }, this.pressMs);
    this.notify(node);
    if (node.props.disabled || !node.props.onPress) return false;
    node.props.onPress();
    return true;
  }

  /** Circle: each enclosing scope's `onBack`, innermost first, until one consumes it. @returns {boolean} */
  back() {
    for (let scope = this.focused?.scope ?? this.root; scope; scope = scope.parent) {
      if (scope.props.onBack?.() === true) return true;
    }
    return false;
  }

  /**
   * Shoulders, triggers, Triangle, Square and Options: each enclosing scope's `onAction`, innermost first,
   * until one consumes it. @param {Action} action @returns {boolean}
   */
  action(action) {
    for (let scope = this.focused?.scope ?? this.root; scope; scope = scope.parent) {
      if (scope.props.onAction?.(action) === true) return true;
    }
    return false;
  }

  /** @param {Scope} scope @returns {Array<{node: Node, rect: Rect, order: number}>} */
  candidates(scope) {
    const result = [];
    for (const node of this.nodes) {
      if (!within(node.scope, scope) || !navigable(node.scope)) continue;
      const rect = screenRect(node);
      if (rect && (rect.width > 0 || rect.height > 0)) result.push({node, rect, order: node.order});
    }
    return result;
  }

  /**
   * The navigable nodes with a box under any of `chain`'s scopes (innermost first), each with the index
   * of the innermost one it is in, so it is a candidate from that level out. One pass per move: each
   * level scanning every node, walking its scopes and working out its rect again was most of a move
   * with a few hidden screens of cards mounted.
   * @param {Scope[]} chain @returns {{node: Node, rect: Rect, order: number, level: number}[]}
   */
  placed(chain) {
    const levels = new Map(chain.map((scope, index) => [scope, index]));
    const result = [];
    for (const node of this.nodes) {
      let level = -1;
      let scope = node.scope;
      for (; scope && !scope.props.inert; scope = scope.parent) if (level < 0 && levels.has(scope)) level = levels.get(scope);
      if (scope || level < 0) continue;
      const rect = screenRect(node);
      if (rect && (rect.width > 0 || rect.height > 0)) result.push({node, rect, order: node.order, level});
    }
    return result;
  }

  /** A row's remembered element, else its `selected` one, else its first. @param {Scope} scope @returns {Node | null} */
  rowEntry(scope) {
    const remembered = scope.remembered;
    if (remembered && !remembered.dead && navigable(remembered.scope) && scope.props.restoreFocus !== false) {
      return remembered;
    }
    for (const node of this.nodes) {
      if (node.props.selected && within(node.scope, scope) && navigable(node.scope)) return node;
    }
    return this.firstOf(scope);
  }

  /** @param {Scope} scope @returns {Node | null} */
  firstOf(scope) {
    const remembered = scope.remembered;
    if (remembered && !remembered.dead && navigable(remembered.scope) && scope.props.restoreFocus !== false) {
      return remembered;
    }
    let first = null;
    for (const node of this.nodes) {
      if (within(node.scope, scope) && navigable(node.scope) && (!first || node.order < first.order)) first = node;
    }
    return first;
  }

  /**
   * The node to focus when a move lands on `winner`: entering a scope restores its remembered element;
   * a row entered for the first time starts at its `selected` element, or else its first, wherever the
   * move came from.
   */
  entryPoint(winner, current) {
    let entered = null;
    for (let scope = winner.scope; scope && !within(current.scope, scope); scope = scope.parent) entered = scope;
    if (entered?.props.row) return this.rowEntry(entered) ?? winner;
    const remembered = entered?.remembered;
    return remembered && !remembered.dead && navigable(remembered.scope) && entered.props.restoreFocus !== false
      ? remembered : winner;
  }

  /**
   * Where the focused element is on screen, in render pixels: its layout rectangle moved by the
   * ScrollViews around it (to where they are scrolling), before transforms. Null with nothing focused.
   * @returns {Rect | null}
   */
  focusedRect() {
    const rect = this.focused && screenRect(this.focused);
    return rect ? {...rect} : null;
  }

  /**
   * @param {Node | null} next
   * @param {boolean} [returning] focus goes back to where a closed layer was opened from: the scopes it
   *   lands in keep where they return to, instead of returning into the closed layer
   */
  setFocus(next, returning = false) {
    const previous = this.focused;
    if (previous === next) return;
    this.focused = next;
    for (let scope = next?.scope; scope; scope = scope.parent) {
      if (!returning && (!previous || !within(previous.scope, scope))) scope.returnTo = previous;
      scope.remembered = next;
    }
    if (previous) {
      this.notify(previous);
      previous.props.onBlur?.();
    }
    if (next) {
      this.notify(next);
      next.props.onFocus?.();
      // A finger scrolls the content itself: what it taps stays where it is.
      if (!this.touching) this.reveal(next);
    }
    for (const listener of this.listeners) listener();
  }

  /**
   * Scrolls every scrolling ancestor, innermost first, until `node` is in view; each one aligns the
   * nearest `scrollAnchor` ancestor that it contains directly, if any. @param {Node} node
   */
  reveal(node) {
    let rect = node.rect;
    if (!rect) return;
    for (let frame = node.frame; frame; frame = frame.parent) {
      let anchor = node.anchor;
      while (anchor && anchor.frame !== frame) anchor = anchor.parent;
      frame.reveal(rect, anchor?.rect ?? null);
      rect = scrolledBy(rect, frame);
    }
  }

  /** After the focused node unmounted: a dead scope's previous focus, else the nearest survivor. */
  recover(lost) {
    if (this.focused) return;
    if (this.claims.size) {
      this.lostWhileClaimed = lost;
      return;
    }
    this.lostWhileClaimed = null;
    const {node, back} = this.fallback(lost);
    if (node) this.setFocus(node, back);
    else for (const listener of this.listeners) listener();
  }

  /** Moves focus out of a subtree that became inert, the way recovery leaves an unmounted one. */
  evict() {
    const node = this.focused;
    if (!node || navigable(node.scope)) return;
    const target = this.fallback({scope: node.scope, rect: screenRect(node)});
    this.setFocus(target.node, target.back);
  }

  /**
   * Where focus goes when it must leave `scope`: an unusable (dead or inert) scope's previous focus,
   * else the navigable element nearest `rect`, else the scope's remembered or first one, outward.
   * `back` tells the first from the rest.
   * @param {{scope: Scope | null, rect: Rect | null}} from
   * @returns {{node: Node | null, back: boolean}}
   */
  fallback({scope, rect}) {
    for (; scope; scope = scope.parent) {
      if (scope.dead || !navigable(scope)) {
        const back = scope.returnTo;
        if (back && !back.dead && navigable(back.scope)) return {node: back, back: true};
        continue;
      }
      const node = (rect && findClosest(rect, this.candidates(scope))?.node) ?? this.firstOf(scope);
      if (node) return {node, back: false};
    }
    return {node: null, back: false};
  }

  /** @param {Node} node */
  notify(node) {
    for (const listener of node.listeners) listener();
  }
}
