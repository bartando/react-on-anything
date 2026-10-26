// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once

// The directory app paths resolve under (/app0, /download0, /temp0 below it), on the SD card.
void switch_set_root(const char* path);
const char* switch_root();
