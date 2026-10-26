// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
// Touch scrolling for a page the app moves itself (one column translated to its focused row, so other
// views can move with it in lockstep) instead of a ScrollView. A finger scrolls it as it would a
// ScrollView, dragging and flinging, through an engine value bound to the page and to the views that
// move with it: no React render per frame. The finger's offset adds to the app's own; the first button
// after the touchscreen eases it back out while the app scrolls to focus. See docs/NAVIGATION.md.
import {createElement, useContext, useLayoutEffect, useState} from 'react';
import {Animated, View} from 'embedded-react';
import {FrameContext, manager} from './runtime.js';
import {createFrame} from './scroll-frame.js';

const BASE_WIDTH = 1280;
const HAND_BACK_MS = 300;
const FILL = {position: 'absolute', left: 0, top: 0, right: 0, bottom: 0};

/**
 * @typedef {{frame: import('./manager.js').Frame, style: object, offset: number, max: number,
 *   delta: number, horizontal: boolean}} Pan
 *   `style` moves a view with the page under the finger; `offset` and `max` are in screen px
 */

/** @param {boolean} horizontal @returns {Pan & {destroy: () => void}} */
function createPan(horizontal) {
  const value = new Animated.Value(0);
  let easing = null;
  const pan = {horizontal, offset: 0, max: 0, delta: 0,
    style: {transform: [horizontal ? {translateX: value} : {translateY: value}]}};
  const at = () => pan.offset + pan.delta;
  const sync = () => {
    const [x, y] = horizontal ? [at(), 0] : [0, at()];
    Object.assign(pan.frame, {x, y, position: [x, y]});
  };
  // The frame's scroller: the page's offset is the app's plus the finger's, kept within the content.
  pan.frame = createFrame(null, (handle, x, y) => {
    const to = horizontal ? x : y;
    if (!Number.isNaN(to)) {
      easing?.stop();
      easing = null;
      pan.delta = Math.min(Math.max(to, 0), pan.max) - pan.offset;
      value.setValue(-pan.delta);
    }
    return horizontal ? [at(), 0, pan.max, 0] : [0, at(), 0, pan.max];
  });
  pan.frame.handle = pan;
  pan.frame.props = {horizontal};
  // Focus does not scroll it: the app moves the page to what is focused.
  pan.frame.reveal = () => {};
  pan.place = (offset, max) => {
    pan.offset = offset;
    pan.max = Math.max(0, max);
    sync();
  };
  const handBack = touching => {
    if (touching || !pan.delta) return;
    pan.frame.stop();
    pan.delta = 0;
    sync();
    easing = Animated.timing(value, {toValue: 0, duration: HAND_BACK_MS});
    easing.start();
  };
  manager.modeListeners.add(handBack);
  pan.destroy = () => {
    manager.modeListeners.delete(handBack);
    pan.frame.stop();
    easing?.stop();
    value.destroy();
  };
  return pan;
}

/**
 * A page's touch scrolling, for a PanView and the views that move with it (`style={pan.style}`).
 * @param {{horizontal?: boolean}} [options] @returns {Pan}
 */
export function usePan({horizontal = false} = {}) {
  const [pan] = useState(() => createPan(horizontal));
  useLayoutEffect(() => () => pan.destroy(), []);
  return pan;
}

/**
 * The page `pan` scrolls under a finger: `offset` is where the app has moved its content, `max` the
 * furthest it goes, both in logical px of a 1280-wide layout like motion's `y`. Without them, a view
 * that moves with the page (a header the app translates by the same offset). Either way the focusable
 * elements inside are hit where they show.
 * @param {{pan: Pan, offset?: number, max?: number, children?: import('react').ReactNode} & object} props
 */
export function PanView({pan, offset, max, children, ...rest}) {
  const parent = useContext(FrameContext);
  const page = offset !== undefined;
  if (page) {
    pan.frame.parent = parent;
    const scale = screen.width / BASE_WIDTH;
    pan.place(Math.round(offset * scale), Math.round((max ?? 0) * scale));
  }
  return createElement(View, page ? {...rest, onLayout: pan.frame.onLayout} : rest,
    createElement(View, {style: [FILL, pan.style], pointerEvents: 'box-none'},
      createElement(FrameContext.Provider, {value: pan.frame}, children)));
}
