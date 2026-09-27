/*
 *
 * Copyright (C) 2025 Maxime Schmitt <maxime.schmitt91@gmail.com>
 *
 * This file is part of Nvtop.
 *
 * Nvtop is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nvtop is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nvtop.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifndef PLOT_LEGEND_H__
#define PLOT_LEGEND_H__

#include "nvtop/common.h"

#include <stddef.h>

// A plot legend is a key, not a readout: it says which curve of the chart is
// which metric, and it never carries the current value of that metric, which
// the detail block of the device already shows.
// All the entries of a chart are laid out on a single row, horizontally, in
// plot line order, and every entry is followed by a short horizontal swatch so
// that it is visibly the key of a line. Text and swatch of an entry are drawn
// in the color of the plot line they stand for.

#define PLOT_LEGEND_SWATCH "---"
#define PLOT_LEGEND_SWATCH_SIZE (sizeof(PLOT_LEGEND_SWATCH) - 1u)
// Blank columns between two entries of the row.
#define PLOT_LEGEND_ENTRY_GAP 4u
// An entry that does not fit the row is cut rather than left out as long as
// that many columns are available for it: below that it identifies no line at
// all and it is dropped like the entries after it.
#define PLOT_LEGEND_MIN_ENTRY_SIZE 4u

// Upper bound of the row the widest possible chart needs: every entry at its
// widest legend, with its swatch, and the gaps between them.
#define PLOT_LEGEND_ROW_SIZE                                                                                           \
  (MAX_LINES_PER_PLOT * (PLOT_MAX_LEGEND_SIZE + PLOT_LEGEND_SWATCH_SIZE) +                                             \
   (MAX_LINES_PER_PLOT - 1u) * PLOT_LEGEND_ENTRY_GAP + 1u)

// Where an entry lies in the formatted row so that it can be drawn with the
// color of its own plot line. The index of a key is the index of the plot line
// it belongs to: an entry that did not fit the row keeps a zero length.
struct plot_legend_key {
  unsigned offset; // Column of the entry in the row
  unsigned length; // Columns of the entry, its swatch included
};

// Format the legend row of a chart: "<legend> ---" for each of the num_entries
// legends, in plot line order, separated by PLOT_LEGEND_ENTRY_GAP columns.
// num_entries above MAX_LINES_PER_PLOT is clamped, and a legend that is not a
// terminated string inside its slot is treated as the shortest terminated
// string it holds.
// The row holds at most `size - 1` columns, so `size` is the number of columns
// of the chart plus one: the entries that do not fit are left out, and the last
// one that has room is cut with a trailing marker instead of overflowing.
// keys, when given, receives the placement of every entry written, for up to
// max_keys entries, and the entries that were not written are zeroed.
// Like snprintf, the return value is the number of columns the complete row
// would have needed, so that the caller can right align it when it fits.
unsigned nvtop_format_plot_legend_row(char *buffer, size_t size, unsigned num_entries,
                                      char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE],
                                      struct plot_legend_key *keys, unsigned max_keys);

#endif // PLOT_LEGEND_H__
