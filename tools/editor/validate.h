#pragma once

/*
 * Save-time validation. Runs the checks listed in
 * documents/m5/02-level-editor.md §"Validation on save" plus the alcove-
 * sizing pass from documents/m5/07-maps.md §"Sizing — the mech has to fit".
 *
 * Returns the number of problems found. Each problem is a short
 * human-readable string in `out_buf` — one line per problem, '\n'-
 * separated. Caller passes a buffer big enough; 4 KB is plenty
 * (worst-case ~20 lines × 80 chars).
 */

#include "doc.h"

int validate_doc(const EditorDoc *d, char *out_buf, int out_cap);
