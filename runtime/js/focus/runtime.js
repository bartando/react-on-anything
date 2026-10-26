// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// The app's focus manager, its React contexts, and the host input that drives it.
import {createContext} from 'react';
import {subscribeInput, subscribeScreenTouch, subscribeTouch} from '../input.js';
import {dispatchBackPress} from '../native.js';
import {installInspector} from './inspect.js';
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
 * @typedef {{type: 'move' | 'blocked', direction: import('./geometry.js').Direction, source?: 'touchpad' | 'touchscreen'}
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

subscribeInput(action => {
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

const touchpad = new TouchNavigator(direction =>
  emit({type: manager.move(direction) ? 'move' : 'blocked', direction, source: 'touchpad'}));
subscribeTouch((x, y) => touchpad.touch(x, y));

// 12 logical px of a 1280-wide layout before a touch is a drag, and 200 per focus step where a drag has
// nothing to scroll (about a row of cards).
const logical = px => Math.round((globalThis.screen?.width ?? 1920) * px / 1280);
const screenTouch = new ScreenTouch({
  hit: (x, y) => hitNode(manager, x, y),
  frameAt: (x, y, horizontal) => frameAt(manager, x, y, horizontal),
  press: node => {
    if (!manager.focus(node)) return;
    const handled = manager.press();
    if (handled !== null) emit({type: 'press', handled});
  },
  scroll: (frame, dx, dy) => frame.drag(dx, dy),
  step: direction => emit({type: manager.move(direction) ? 'move' : 'blocked', direction, source: 'touchscreen'}),
}, logical(12), logical(200));
subscribeScreenTouch((x, y) => screenTouch.touch(x, y));
