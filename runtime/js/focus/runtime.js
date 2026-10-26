// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The app's focus manager, its React contexts, and the host input that drives it.
import {createContext} from 'react';
import {subscribeInput, subscribeScreenTouch, subscribeTouch} from '../input.js';
import {dispatchBackPress} from '../native.js';
import {installInspector} from './inspect.js';
import {screenRect} from './frames.js';
import {FocusManager} from './manager.js';
import {frameAt, hitNode, ScreenTouch} from './screen-touch.js';
import {TouchNavigator} from './touch.js';

export const manager = new FocusManager();
installInspector(manager);

/** The enclosing FocusScope; the manager's root outside any. */
export const ScopeContext = createContext(manager.root);
/** @type {import('react').Context<import('./manager.js').Frame | null>} the enclosing ScrollView */
export const FrameContext = createContext(null);
/** @type {import('react').Context<import('./manager.js').Node | null>} the nearest focusable ancestor */
export const NodeContext = createContext(null);
/** @type {import('react').Context<import('./manager.js').Anchor | null>} the nearest `scrollAnchor` ancestor */
export const AnchorContext = createContext(null);

/**
 * @typedef {{type: 'move' | 'blocked', direction: import('./geometry.js').Direction, source?: 'touchpad'}
 *   | {type: 'press' | 'back', handled: boolean}
 *   | {type: 'action', action: import('./manager.js').Action, handled: boolean}} NavigationEvent
 *   `blocked`: nothing to move to; `source` is 'touchpad' for a step of a swipe. `press`: handled when
 *   an enabled element with onPress took Cross.
 */
/** @type {Set<(event: NavigationEvent) => void>} */
export const navigationListeners = new Set();

/** @param {NavigationEvent} event */
function emit(event) {
  for (const listener of navigationListeners) listener(event);
}

const screenBounds = () => ({x: 0, y: 0, width: globalThis.screen?.width ?? 1920, height: globalThis.screen?.height ?? 1080});

subscribeInput(action => {
  // The first button after the touchscreen brings focus back; a D-pad press or Cross only shows it.
  if (manager.touching) {
    manager.resume(screenBounds());
    if (action === 'confirm' || action === 'up' || action === 'down' || action === 'left' || action === 'right') return;
  }
  if (action === 'confirm') {
    const handled = manager.press();
    if (handled !== null) emit({type: 'press', handled});
  } else if (action === 'back') {
    emit({type: 'back', handled: manager.back() || dispatchBackPress()});
  } else if (action === 'up' || action === 'down' || action === 'left' || action === 'right') {
    emit({type: manager.move(action) ? 'move' : 'blocked', direction: action});
  } else {
    emit({type: 'action', action, handled: manager.action(action)});
  }
});

const touchpad = new TouchNavigator(direction => {
  if (manager.touching) return manager.resume(screenBounds());
  return emit({type: manager.move(direction) ? 'move' : 'blocked', direction, source: 'touchpad'});
});
subscribeTouch((x, y) => touchpad.touch(x, y));

// 12 logical px of a 1280-wide layout before a touch is a drag, and a release at 0.3 logical px per ms
// or faster flings. A resting finger shows what it is on pressed after HOLD_MS, as a phone waits to
// tell a tap from the start of a scroll; a quicker tap gets Cross's press feedback.
const HOLD_MS = 80;
const scale = (globalThis.screen?.width ?? 1920) / 1280;
let holding = null;
const screenTouch = new ScreenTouch({
  hit: (x, y) => hitNode(manager, x, y),
  frameAt: (x, y, horizontal) => frameAt(manager, x, y, horizontal),
  down: () => manager.setTouching(true),
  hold: node => {
    clearTimeout(holding);
    if (node) holding = setTimeout(() => manager.hold(node), HOLD_MS);
    else manager.hold(null);
  },
  tap: node => {
    const handled = manager.tap(node);
    if (handled !== null) emit({type: 'press', handled});
  },
  grab: (node, phase, x, y) => {
    if (!node.dead) node.props.onDrag?.({phase, x, y, rect: screenRect(node)});
  },
  scroll: (frame, dx, dy) => frame.drag(dx, dy),
  fling: (frame, vx, vy) => frame.fling(vx, vy),
  halt: frame => frame.halt(),
}, Math.round(12 * scale), 0.3 * scale);
subscribeScreenTouch((x, y) => screenTouch.touch(x, y));
