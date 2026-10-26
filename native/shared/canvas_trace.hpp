// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

// Watches the engine draw (er_canvas.h): every drawing primitive of a commit, one line each, written to a
// file per commit that drew anything. The software rasterizer still draws every pixel; the trace only
// looks, so a traced run must show exactly what an untraced one does. At the end it lists how many ops of
// each kind it saw, and any pixels that reached the screen outside a primitive (ER_CANVAS_UNHOOKED):
// what a GPU canvas would not be told about.
//
// Ops of one commit are in drawing order only with a single render worker; start() reports when there are
// more.
namespace canvas_trace {

bool start(const char* path, int render_workers);
void commit_begin();
void commit_end();
void stop();

}  // namespace canvas_trace
