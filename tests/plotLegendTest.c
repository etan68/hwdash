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

// Standalone check of the chart legend row: the keys of the lines of a chart,
// all on one row, each one with its swatch. No GPU, no ncurses and no gtest: the
// renderer draws what this lays out, in the color of each plot line.

#include "nvtop/plot_legend.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks = 0, failures = 0;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    ++checks;                                                                                                          \
    if (!(cond)) {                                                                                                     \
      ++failures;                                                                                                      \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                           \
    }                                                                                                                  \
  } while (0)

// A row with a guard behind it: the formatter must never write past the room it
// was given, whatever the window and whatever the legends.
#define GUARD_SIZE 32u
#define GUARD_BYTE 0xA5u

struct guarded_row {
  char row[PLOT_LEGEND_ROW_SIZE];
  unsigned char guard[GUARD_SIZE];
};

static void guarded_init(struct guarded_row *buffer) {
  memset(buffer->row, 0, sizeof(buffer->row));
  memset(buffer->guard, GUARD_BYTE, sizeof(buffer->guard));
}

static bool guard_intact(const struct guarded_row *buffer) {
  for (unsigned i = 0; i < GUARD_SIZE; ++i)
    if (buffer->guard[i] != GUARD_BYTE)
      return false;
  return true;
}

// The invariants of a formatted row: it stays inside the room it was given, the
// keys it reports point inside it, they are in plot line order and they never
// overlap each other.
static void check_row(const struct guarded_row *buffer, size_t size, const struct plot_legend_key *keys,
                      unsigned max_keys) {
  CHECK(guard_intact(buffer));
  CHECK(strlen(buffer->row) + 1u <= size);
  unsigned previous_end = 0u;
  for (unsigned i = 0; i < max_keys; ++i) {
    if (keys[i].length == 0u) {
      CHECK(keys[i].offset == 0u);
      continue;
    }
    CHECK(keys[i].offset + keys[i].length <= strlen(buffer->row));
    CHECK(keys[i].offset >= previous_end);
    previous_end = keys[i].offset + keys[i].length;
  }
}

static void fill_legend(char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE], unsigned count,
                        const char *const *names) {
  for (unsigned i = 0; i < MAX_LINES_PER_PLOT; ++i)
    legend[i][0] = '\0';
  for (unsigned i = 0; i < count; ++i)
    snprintf(legend[i], PLOT_MAX_LEGEND_SIZE, "%s", names[i]);
}

// The row the default GPU chart draws, and the row of the CPU chart.
static void test_default_rows(void) {
  char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE];
  struct plot_legend_key keys[MAX_LINES_PER_PLOT];
  struct guarded_row row;

  static const char *const gpu_names[] = {"GPU0 %", "GPU0 mem%"};
  fill_legend(legend, 2u, gpu_names);
  guarded_init(&row);
  unsigned needed = nvtop_format_plot_legend_row(row.row, sizeof(row.row), 2u, legend, keys, MAX_LINES_PER_PLOT);
  CHECK(strcmp(row.row, "GPU0 % ---    GPU0 mem% ---") == 0);
  CHECK(needed == strlen(row.row));
  CHECK(keys[0].offset == 0u);
  CHECK(keys[0].length == strlen("GPU0 % ---"));
  CHECK(keys[1].offset == strlen("GPU0 % ---") + PLOT_LEGEND_ENTRY_GAP);
  CHECK(keys[1].length == strlen("GPU0 mem% ---"));
  // Every entry carries the swatch: it is what makes it a key of a line.
  CHECK(strstr(row.row + keys[0].offset, PLOT_LEGEND_SWATCH) != NULL);
  CHECK(strstr(row.row + keys[1].offset, PLOT_LEGEND_SWATCH) != NULL);
  check_row(&row, sizeof(row.row), keys, MAX_LINES_PER_PLOT);

  static const char *const host_names[] = {"CPU %", "RAM %"};
  fill_legend(legend, 2u, host_names);
  guarded_init(&row);
  needed = nvtop_format_plot_legend_row(row.row, sizeof(row.row), 2u, legend, keys, MAX_LINES_PER_PLOT);
  CHECK(strcmp(row.row, "CPU % ---    RAM % ---") == 0);
  CHECK(needed == strlen(row.row));
  CHECK(keys[0].length == strlen("CPU % ---"));
  CHECK(keys[1].offset == strlen("CPU % ---") + PLOT_LEGEND_ENTRY_GAP);
  check_row(&row, sizeof(row.row), keys, MAX_LINES_PER_PLOT);

  // A legend is a key, so it holds no number: none of the keys of a chart
  // carries a digit of its own, only the device index does.
  CHECK(strcmp(host_names[0], "CPU %") == 0 && strcmp(host_names[1], "RAM %") == 0);
}

// A chart with one line, and a chart with all of them.
static void test_entry_count(void) {
  char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE];
  struct plot_legend_key keys[MAX_LINES_PER_PLOT];
  struct guarded_row row;

  static const char *const one[] = {"GPU0 %"};
  fill_legend(legend, 1u, one);
  guarded_init(&row);
  unsigned needed = nvtop_format_plot_legend_row(row.row, sizeof(row.row), 1u, legend, keys, MAX_LINES_PER_PLOT);
  CHECK(strcmp(row.row, "GPU0 % ---") == 0);
  CHECK(needed == strlen(row.row));
  CHECK(keys[1].length == 0u); // No line, no key
  check_row(&row, sizeof(row.row), keys, MAX_LINES_PER_PLOT);

  static const char *const all[] = {"GPU0 %", "GPU0 mem%", "GPU0 encode%", "GPU1 mem%"};
  fill_legend(legend, MAX_LINES_PER_PLOT, all);
  guarded_init(&row);
  needed =
      nvtop_format_plot_legend_row(row.row, sizeof(row.row), MAX_LINES_PER_PLOT, legend, keys, MAX_LINES_PER_PLOT);
  CHECK(needed == strlen(row.row));
  for (unsigned i = 0; i < MAX_LINES_PER_PLOT; ++i) {
    // The keys stay in plot line order, and each one is its legend and a swatch.
    CHECK(keys[i].length == strlen(all[i]) + 1u + PLOT_LEGEND_SWATCH_SIZE);
    CHECK(strncmp(row.row + keys[i].offset, all[i], strlen(all[i])) == 0);
  }
  CHECK(keys[0].offset == 0u);
  CHECK(strcmp(row.row + keys[MAX_LINES_PER_PLOT - 1u].offset, "GPU1 mem% ---") == 0);
  check_row(&row, sizeof(row.row), keys, MAX_LINES_PER_PLOT);

  // More entries than a chart can have are clamped, not overflowed.
  guarded_init(&row);
  needed = nvtop_format_plot_legend_row(row.row, sizeof(row.row), 100u, legend, keys, MAX_LINES_PER_PLOT);
  CHECK(needed == strlen(row.row));
  check_row(&row, sizeof(row.row), keys, MAX_LINES_PER_PLOT);

  // No entry at all is an empty row, and no key.
  guarded_init(&row);
  memset(keys, 0xFF, sizeof(keys));
  CHECK(nvtop_format_plot_legend_row(row.row, sizeof(row.row), 0u, legend, keys, MAX_LINES_PER_PLOT) == 0u);
  CHECK(row.row[0] == '\0');
  for (unsigned i = 0; i < MAX_LINES_PER_PLOT; ++i)
    CHECK(keys[i].length == 0u);
}

// The width the renderer hands over is the width of the chart: what does not fit
// is cut, never wrapped, and never written outside of the row.
static void test_narrow_rows(void) {
  char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE];
  struct plot_legend_key keys[MAX_LINES_PER_PLOT];
  struct guarded_row row;

  static const char *const gpu_names[] = {"GPU0 %", "GPU0 mem%"};
  fill_legend(legend, 2u, gpu_names);
  const unsigned full = 27u; // "GPU0 % ---    GPU0 mem% ---"

  // A window that fits the whole row leaves it untouched.
  guarded_init(&row);
  CHECK(nvtop_format_plot_legend_row(row.row, full + 1u, 2u, legend, keys, MAX_LINES_PER_PLOT) == full);
  CHECK(strlen(row.row) == full);
  check_row(&row, full + 1u, keys, MAX_LINES_PER_PLOT);

  // A window that cuts the second entry: the first entry is kept whole, with its
  // swatch, and the second one is cut short with a marker.
  guarded_init(&row);
  CHECK(nvtop_format_plot_legend_row(row.row, 23u, 2u, legend, keys, MAX_LINES_PER_PLOT) == full);
  CHECK(strcmp(row.row, "GPU0 % ---    GPU0 me+") == 0);
  CHECK(keys[0].length == strlen("GPU0 % ---"));
  CHECK(keys[1].length > 0u);
  CHECK(row.row[keys[1].offset + keys[1].length - 1u] == '+');
  check_row(&row, 23u, keys, MAX_LINES_PER_PLOT);

  // A window that cannot hold the swatch of the first entry keeps as much of the
  // entry as it can, and stops: nothing is written past the edge.
  for (unsigned width = 1u; width < 12u; ++width) {
    guarded_init(&row);
    unsigned needed = nvtop_format_plot_legend_row(row.row, width + 1u, 2u, legend, keys, MAX_LINES_PER_PLOT);
    CHECK(needed == full); // Like snprintf: the row that would have been written
    CHECK(strlen(row.row) <= width);
    CHECK(keys[1].length == 0u);
    check_row(&row, width + 1u, keys, MAX_LINES_PER_PLOT);
  }

  // An entry too short to identify anything is left out entirely.
  guarded_init(&row);
  CHECK(nvtop_format_plot_legend_row(row.row, 3u, 2u, legend, keys, MAX_LINES_PER_PLOT) == full);
  CHECK(row.row[0] == '\0');
  CHECK(keys[0].length == 0u);
  check_row(&row, 3u, keys, MAX_LINES_PER_PLOT);

  // The widest legends a chart can carry, in the narrowest chart.
  char wide_legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE];
  for (unsigned i = 0; i < MAX_LINES_PER_PLOT; ++i)
    memset(wide_legend[i], 'x', PLOT_MAX_LEGEND_SIZE - 1u);
  for (unsigned i = 0; i < MAX_LINES_PER_PLOT; ++i)
    wide_legend[i][PLOT_MAX_LEGEND_SIZE - 1u] = '\0';
  guarded_init(&row);
  CHECK(nvtop_format_plot_legend_row(row.row, sizeof(row.row), MAX_LINES_PER_PLOT, wide_legend, keys,
                                     MAX_LINES_PER_PLOT) == PLOT_LEGEND_ROW_SIZE - 1u);
  CHECK(strlen(row.row) == PLOT_LEGEND_ROW_SIZE - 1u);
  check_row(&row, sizeof(row.row), keys, MAX_LINES_PER_PLOT);

  // A legend slot that is not terminated is taken for the shortest terminated
  // string it holds: it cannot run the row past its own buffer.
  char loose_legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE];
  memset(loose_legend, 'x', sizeof(loose_legend));
  guarded_init(&row);
  CHECK(nvtop_format_plot_legend_row(row.row, sizeof(row.row), 1u, loose_legend, keys, MAX_LINES_PER_PLOT) ==
        PLOT_MAX_LEGEND_SIZE - 1u + 1u + PLOT_LEGEND_SWATCH_SIZE);
  check_row(&row, sizeof(row.row), keys, MAX_LINES_PER_PLOT);
}

// The renderer right aligns the row when the legends belong on its right, and
// left aligns it otherwise: the row itself says what it needs to do that.
static void test_alignment(void) {
  char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE];
  struct plot_legend_key keys[MAX_LINES_PER_PLOT];
  char row[PLOT_LEGEND_ROW_SIZE];

  static const char *const gpu_names[] = {"GPU0 %", "GPU0 mem%"};
  fill_legend(legend, 2u, gpu_names);
  const unsigned chart_cols = 40u;
  unsigned needed =
      nvtop_format_plot_legend_row(row, chart_cols + 1u, 2u, legend, keys, MAX_LINES_PER_PLOT);
  CHECK(needed == 27u);

  // Legends on the left: the row starts at the left edge of the chart.
  CHECK(keys[0].offset == 0u);

  // Legends on the right: the whole row is anchored at the right edge, and the
  // renderer only has to shift it by what the chart has left over.
  const unsigned start = chart_cols - needed;
  CHECK(keys[MAX_LINES_PER_PLOT - 1u].length == 0u);
  CHECK(start + keys[1].offset + keys[1].length == chart_cols);

  // A row wider than the chart is not pushed out of it: the formatter cuts it to
  // the chart width and the renderer anchors it at the left edge.
  // A row that is not pre-zeroed: the formatter terminates it by itself.
  char narrow_row[PLOT_LEGEND_ROW_SIZE];
  memset(narrow_row, 'x', sizeof(narrow_row));
  const unsigned narrow_cols = 20u;
  needed = nvtop_format_plot_legend_row(narrow_row, narrow_cols + 1u, 2u, legend, keys, MAX_LINES_PER_PLOT);
  CHECK(needed > narrow_cols);
  CHECK(strlen(narrow_row) == narrow_cols);
  const unsigned shifted_start = needed <= narrow_cols ? narrow_cols - needed : 0u;
  CHECK(shifted_start == 0u);
  for (unsigned i = 0; i < MAX_LINES_PER_PLOT; ++i)
    CHECK(keys[i].offset + keys[i].length <= narrow_cols);
}

// An empty legend is still a key: the swatch alone says a line is there.
static void test_empty_legend(void) {
  char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE];
  struct plot_legend_key keys[MAX_LINES_PER_PLOT];
  struct guarded_row row;

  fill_legend(legend, 2u, (const char *const []){"GPU0 %", ""});
  guarded_init(&row);
  unsigned needed = nvtop_format_plot_legend_row(row.row, sizeof(row.row), 2u, legend, keys, MAX_LINES_PER_PLOT);
  CHECK(strcmp(row.row, "GPU0 % ---    ---") == 0);
  CHECK(needed == strlen(row.row));
  CHECK(keys[1].length == PLOT_LEGEND_SWATCH_SIZE);
  check_row(&row, sizeof(row.row), keys, MAX_LINES_PER_PLOT);

  // Unusable input: no crash, no write, no key.
  guarded_init(&row);
  CHECK(nvtop_format_plot_legend_row(NULL, sizeof(row.row), 2u, legend, keys, MAX_LINES_PER_PLOT) == 0u);
  CHECK(nvtop_format_plot_legend_row(row.row, 0u, 2u, legend, keys, MAX_LINES_PER_PLOT) == 0u);
  CHECK(nvtop_format_plot_legend_row(row.row, sizeof(row.row), 2u, NULL, keys, MAX_LINES_PER_PLOT) == 0u);
  CHECK(nvtop_format_plot_legend_row(row.row, sizeof(row.row), 2u, legend, NULL, 0u) > 0u);
  CHECK(guard_intact(&row));
}

int main(void) {
  test_default_rows();
  test_entry_count();
  test_narrow_rows();
  test_alignment();
  test_empty_legend();
  printf("%s: %u checks, %u failures\n", failures ? "FAILED" : "PASSED", checks, failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
