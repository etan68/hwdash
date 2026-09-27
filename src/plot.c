/*
 *
 * Copyright (C) 2019-2021 Maxime Schmitt <maxime.schmitt91@gmail.com>
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

#include "nvtop/plot.h"
#include "nvtop/common.h"
#include "nvtop/plot_legend.h"

#include <assert.h>
#include <ncurses.h>
#include <stdbool.h>
#include <string.h>
#include <tgmath.h>

static inline int data_level(double rows, double data, double increment) {
  return (int)(rows - round(data / increment));
}

// A sample that is not a number is a hole in the history: the metric was not
// available for that sample and it must not be drawn as an idle zero.
static inline bool is_missing_sample(double data) { return isnan(data); }

void nvtop_line_plot(WINDOW *win, size_t num_data, const double *data, unsigned num_lines, bool legend_left,
                     char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE]) {
  if (num_data == 0 || num_lines == 0)
    return;
  int rows, cols;
  getmaxyx(win, rows, cols);
  rows -= 1;
  double increment = 100. / (double)(rows);

  assert(num_lines <= MAX_LINES_PER_PLOT && "Cannot plot more than " EXPAND_AND_QUOTE(MAX_LINES_PER_PLOT) " lines");
  static const short plot_line_colors[MAX_LINES_PER_PLOT] = {7, 8, 9, 10};
  unsigned lvl_before[MAX_LINES_PER_PLOT];
  bool has_lvl_before[MAX_LINES_PER_PLOT];
  for (size_t k = 0; k < num_lines; ++k)
    has_lvl_before[k] = !is_missing_sample(data[k]);

  for (size_t i = 0; i < num_data || i < (size_t)cols; i += num_lines) {
    for (unsigned k = 0; k < num_lines; ++k) {
      if (is_missing_sample(data[i + k])) {
        // No value for this line here: leave the place empty and break the
        // line, the next defined sample starts a new segment.
        has_lvl_before[k] = false;
        continue;
      }
      if (!has_lvl_before[k]) {
        // First defined sample of a line, or sample coming back from a hole:
        // there is nothing to connect the sample to, draw it as a point.
        lvl_before[k] = data_level(rows, data[i + k], increment);
        has_lvl_before[k] = true;
        wcolor_set(win, plot_line_colors[k], NULL);
        mvwhline(win, lvl_before[k], i + k, 0, 1);
        continue;
      }
      unsigned lvl_now_k = data_level(rows, data[i + k], increment);
      wcolor_set(win, plot_line_colors[k], NULL);
      // Three cases: has increased, has decreased and remained level
      if (lvl_before[k] < lvl_now_k || lvl_before[k] > lvl_now_k) {
        // Case 1 and 2: has increased/decreased

        // An increase goes down on the plot because (0,0) is top left
        bool drawing_down = lvl_before[k] < lvl_now_k;
        unsigned bottom = drawing_down ? lvl_before[k] : lvl_now_k;
        unsigned top = drawing_down ? lvl_now_k : lvl_before[k];

        // Draw the vertical line corners
        mvwaddch(win, bottom, i + k, drawing_down ? ACS_URCORNER : ACS_ULCORNER);
        mvwaddch(win, top, i + k, drawing_down ? ACS_LLCORNER : ACS_LRCORNER);
        // Draw the vertical line between the corners
        if (top - bottom > 1) {
          mvwvline(win, bottom + 1, i + k, 0, top - bottom - 1);
        }

        // Draw the continuation of the other metrics
        for (unsigned j = 0; j < num_lines; ++j) {
          if (j != k && has_lvl_before[j]) {
            if (lvl_before[j] == top)
              // The continuation is at the same level as the bottom corner
              mvwaddch(win, top, i + k, ACS_BTEE);
            else if (lvl_before[j] == bottom)
              // The continuation is at the same level as the top corner
              mvwaddch(win, bottom, i + k, ACS_TTEE);
            else if (lvl_before[j] > bottom && lvl_before[j] < top)
              // The continuation lies on the vertical line
              mvwaddch(win, lvl_before[j], i + k, ACS_PLUS);
            else {
              // The continuation lies outside the update interval so keep the
              // color
              wcolor_set(win, plot_line_colors[j], NULL);
              mvwaddch(win, lvl_before[j], i + k, ACS_HLINE);
              wcolor_set(win, plot_line_colors[k], NULL);
            }
          }
        }
      } else {
        // Case 3: stayed level
        mvwhline(win, lvl_now_k, i + k, 0, 1);
        for (unsigned j = 0; j < num_lines; ++j) {
          if (j != k && has_lvl_before[j]) {
            if (lvl_before[j] != lvl_now_k) {
              // Add the continuation of other metric lines
              wcolor_set(win, plot_line_colors[j], NULL);
              mvwaddch(win, lvl_before[j], i + k, ACS_HLINE);
              wcolor_set(win, plot_line_colors[k], NULL);
            }
          }
        }
      }
      lvl_before[k] = lvl_now_k;
    }
  }
  // A legend is a key for the plot lines, not a readout of their current
  // value: all the entries share the same row, horizontally, and each entry is
  // followed by a short horizontal swatch. Text and swatch of an entry use the
  // color of the plot line they stand for. The current values belong to the
  // detail block of the device, right above the chart.
  if (cols > 0) {
    char legend_row[PLOT_LEGEND_ROW_SIZE];
    struct plot_legend_key keys[MAX_LINES_PER_PLOT];
    // The row is limited to the width of the chart: what does not fit is cut by
    // the formatter instead of wrapping or leaving the window.
    unsigned needed = nvtop_format_plot_legend_row(legend_row, (size_t)cols + 1u, num_lines, legend, keys,
                                                   MAX_LINES_PER_PLOT);
    // Anchored where the legends belong: at the left edge of the chart, or, for
    // the legends on the right, the complete row aligned at the right edge as
    // long as it fits.
    unsigned start = 0u;
    if (!legend_left && needed <= (unsigned)cols)
      start = (unsigned)cols - needed;
    for (unsigned i = 0; i < num_lines; ++i) {
      if (keys[i].length == 0u)
        continue;
      unsigned column = start + keys[i].offset;
      if (column >= (unsigned)cols)
        break;
      unsigned length = keys[i].length;
      if (column + length > (unsigned)cols)
        length = (unsigned)cols - column;
      wcolor_set(win, plot_line_colors[i], NULL);
      mvwaddnstr(win, 0, (int)column, legend_row + keys[i].offset, (int)length);
    }
    wstandend(win);
  }
}

void draw_rectangle(WINDOW *win, unsigned startX, unsigned startY, unsigned sizeX, unsigned sizeY) {
  mvwhline(win, startY, startX + 1, 0, sizeX - 2);
  mvwhline(win, startY + sizeY - 1, startX + 1, 0, sizeX - 2);

  mvwvline(win, startY + 1, startX, 0, sizeY - 2);
  mvwvline(win, startY + 1, startX + sizeX - 1, 0, sizeY - 2);

  mvwaddch(win, startY, startX, ACS_ULCORNER);
  mvwaddch(win, startY, startX + sizeX - 1, ACS_URCORNER);
  mvwaddch(win, startY + sizeY - 1, startX, ACS_LLCORNER);
  mvwaddch(win, startY + sizeY - 1, startX + sizeX - 1, ACS_LRCORNER);
}
