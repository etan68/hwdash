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

#include "nvtop/plot_legend.h"

#include <string.h>

// The layout of the legend row is pure text work: no terminal, no color, so
// that the placement and the truncation of the entries can be checked without
// a screen. The caller draws each key with the color of its plot line.

// The length of a legend as it can be held by its slot, whether or not the
// slot happens to be terminated.
static unsigned legend_text_length(const char *text) {
  unsigned length = 0u;
  while (length < PLOT_MAX_LEGEND_SIZE - 1u && text[length] != '\0')
    ++length;
  return length;
}

static void legend_row_append(char *row, size_t limit, size_t *written, const char *text, size_t length) {
  if (*written >= limit || length == 0u)
    return;
  size_t room = limit - *written;
  size_t copied = length < room ? length : room;
  memcpy(row + *written, text, copied);
  *written += copied;
}

static void legend_row_append_repeat(char *row, size_t limit, size_t *written, char character, unsigned count) {
  for (unsigned i = 0; i < count; ++i)
    legend_row_append(row, limit, written, &character, 1u);
}

unsigned nvtop_format_plot_legend_row(char *buffer, size_t size, unsigned num_entries,
                                      char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE],
                                      struct plot_legend_key *keys, unsigned max_keys) {
  if (keys) {
    for (unsigned i = 0; i < max_keys; ++i) {
      keys[i].offset = 0u;
      keys[i].length = 0u;
    }
  }
  if (!buffer || size == 0u)
    return 0u;
  buffer[0] = '\0';
  if (!legend || num_entries == 0u)
    return 0u;
  if (num_entries > MAX_LINES_PER_PLOT)
    num_entries = MAX_LINES_PER_PLOT;

  // The row is never longer than the buffer it is written to, nor than the
  // widest row a chart can ask for.
  const size_t limit = (size < PLOT_LEGEND_ROW_SIZE ? size : (size_t)PLOT_LEGEND_ROW_SIZE) - 1u;
  size_t written = 0u;

  // The complete row, as if it had all the room it needs: what the caller uses
  // to align the row at the right of the chart when it fits.
  unsigned needed = 0u;
  for (unsigned i = 0; i < num_entries; ++i) {
    unsigned text_length = legend_text_length(legend[i]);
    needed += (text_length ? 1u : 0u) + PLOT_LEGEND_SWATCH_SIZE + text_length;
    if (i + 1u < num_entries)
      needed += PLOT_LEGEND_ENTRY_GAP;
  }

  for (unsigned i = 0; i < num_entries; ++i) {
    const char *text = legend[i];
    unsigned text_length = legend_text_length(text);
    // "<legend> ---", the swatch separated from the text by one blank column.
    unsigned entry_length = text_length + (text_length ? 1u : 0u) + PLOT_LEGEND_SWATCH_SIZE;
    unsigned gap = (i == 0u) ? 0u : PLOT_LEGEND_ENTRY_GAP;
    unsigned room = written < limit ? (unsigned)(limit - written) : 0u;

    if (room < gap + entry_length) {
      // What is left cannot hold the whole entry. It may still hold a
      // recognizable piece of its text: cut it, with a marker, and stop there:
      // an entry after a cut one would be drawn past the edge of the chart.
      unsigned available = room > gap ? room - gap : 0u;
      if (available >= PLOT_LEGEND_MIN_ENTRY_SIZE) {
        legend_row_append_repeat(buffer, limit, &written, ' ', gap);
        const unsigned cut_offset = (unsigned)written;
        // The last column of what is left is for the marker, the rest for the
        // text, which may well be shorter than what the window had for it.
        const unsigned keep = text_length < available - 1u ? text_length : available - 1u;
        legend_row_append(buffer, limit, &written, text, keep);
        legend_row_append(buffer, limit, &written, "+", 1u);
        if (keys && i < max_keys) {
          keys[i].offset = cut_offset;
          keys[i].length = (unsigned)written - cut_offset;
        }
      }
      break;
    }

    legend_row_append_repeat(buffer, limit, &written, ' ', gap);
    unsigned offset = (unsigned)written;
    legend_row_append(buffer, limit, &written, text, text_length);
    if (text_length)
      legend_row_append(buffer, limit, &written, " ", 1u);
    legend_row_append(buffer, limit, &written, PLOT_LEGEND_SWATCH, PLOT_LEGEND_SWATCH_SIZE);
    if (keys && i < max_keys) {
      keys[i].offset = offset;
      keys[i].length = (unsigned)written - offset;
    }
  }

  // The room was counted in columns, the terminator included, so the row is
  // always a string that ends inside the buffer it was written to.
  buffer[written > limit ? limit : written] = '\0';
  return needed;
}
