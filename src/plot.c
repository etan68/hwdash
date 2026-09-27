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
#include "nvtop/plot_geometry.h"
#include "nvtop/plot_legend.h"

#include <assert.h>
#include <ncurses.h>
#include <stdbool.h>
#include <string.h>
#include <tgmath.h>

// A sample that is not a number is a hole in the history: the metric was not
// available for that sample and it must not be drawn as an idle zero.
static inline bool is_missing_sample(double data) { return isnan(data); }

// The color of the plot line of a chart, shared by the curve, its legend and
// the readout of its current value.
static const short plot_line_colors[MAX_LINES_PER_PLOT] = {7, 8, 9, 10};

void nvtop_line_plot(WINDOW *win, size_t num_data, const double *data, unsigned num_lines, bool legend_left,
                     char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE]) {
  if (num_data == 0 || num_lines == 0)
    return;
  int rows, cols;
  getmaxyx(win, rows, cols);
  if (rows <= (int)PLOT_DATA_TOP_ROW || cols <= 0)
    return;

  assert(num_lines <= MAX_LINES_PER_PLOT && "Cannot plot more than " EXPAND_AND_QUOTE(MAX_LINES_PER_PLOT) " lines");
  const bool reversed_time_axis = !legend_left;
  unsigned lvl_before[MAX_LINES_PER_PLOT];
  bool has_lvl_before[MAX_LINES_PER_PLOT];
  for (size_t k = 0; k < num_lines; ++k)
    has_lvl_before[k] = false;

  for (size_t i = 0; i + num_lines <= num_data && i < (size_t)cols; i += num_lines) {
    const unsigned sample_column = (unsigned)(i / num_lines);
    for (unsigned k = 0; k < num_lines; ++k) {
      if (is_missing_sample(data[i + k])) {
        // No value for this line here: leave the place empty and break the
        // line, the next defined sample starts a new segment.
        has_lvl_before[k] = false;
        continue;
      }
      const int level = nvtop_plot_data_level((unsigned)rows, data[i + k]);
      if (level < 0) {
        has_lvl_before[k] = false;
        continue;
      }
      // Where the line is drawn and how it is joined to what came before, all
      // of it decided by the column the sample lies in. The oldest edge of the
      // ordinary axis is the left one: a sample that reaches it has no
      // connector left on its left and it disappears with it, so no leftover
      // mark ever hangs at the edge of the chart. The line begins at the first
      // sample that belongs to a visible segment, and the reversed axis keeps
      // the newest edge it draws there.
      switch (nvtop_plot_sample_action_at(reversed_time_axis, sample_column)) {
      case nvtop_plot_sample_drop:
        lvl_before[k] = (unsigned)level;
        has_lvl_before[k] = true;
        continue;
      case nvtop_plot_sample_point:
        // Nothing to connect the sample to: draw it as a point, be it the
        // first sample of a line, one coming back from a hole, or the first
        // one the chart can connect from.
        lvl_before[k] = (unsigned)level;
        has_lvl_before[k] = true;
        wcolor_set(win, plot_line_colors[k], NULL);
        // Interleaved lines share the same left edge even though their later
        // sample columns are staggered by line. Fill the complete first sample
        // group so no line leaves a one-column hole before its next sample.
        if (sample_column == 0u)
          mvwhline(win, lvl_before[k], i, 0, num_lines);
        else
          mvwhline(win, lvl_before[k], i + k, 0, 1);
        continue;
      case nvtop_plot_sample_connect:
        break;
      }
      if (!has_lvl_before[k]) {
        // The line has a hole right before this sample: nothing to connect it
        // to, draw it as a point.
        lvl_before[k] = (unsigned)level;
        has_lvl_before[k] = true;
        wcolor_set(win, plot_line_colors[k], NULL);
        mvwhline(win, lvl_before[k], i + k, 0, 1);
        continue;
      }
      unsigned lvl_now_k = (unsigned)level;
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

  // Carry every visible line through the shared edge column to the frame. The
  // column is display-only and does not change the amount of history shown.
  if ((size_t)cols > num_data) {
    const int edge = cols - 1;
    for (unsigned k = 0; k < num_lines; ++k) {
      if (!has_lvl_before[k])
        continue;
      wcolor_set(win, plot_line_colors[k], NULL);
      mvwaddch(win, (int)lvl_before[k], edge, ACS_HLINE);
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

void nvtop_plot_readouts(WINDOW *win, size_t num_data, const double *data, unsigned num_lines, bool legend_left) {
  if (!win || num_lines == 0 || num_data < num_lines)
    return;
  int rows, cols;
  getmaxyx(win, rows, cols);
  if (cols <= (int)(PLOT_READOUT_LEFT_PAD + PLOT_READOUT_VALUE_WIDTH))
    return;

  // The value of a line is the sample its visible end lies on: the newest one
  // of the history, at the edge the time flows out of, skipping the holes the
  // line is broken at. A line with no drawn value shows nothing at all.
  const size_t num_columns = num_data / num_lines;
  const bool reversed_time_axis = !legend_left;
  double values[MAX_LINES_PER_PLOT];
  for (unsigned k = 0; k < num_lines; ++k) {
    double value = NAN;
    if (nvtop_plot_newest_drawn_value(num_columns, reversed_time_axis, data, num_lines, k, &value))
      values[k] = value;
    else
      values[k] = NAN;
  }

  struct plot_readout readouts[MAX_LINES_PER_PLOT];
  const unsigned num_readouts = nvtop_place_plot_readouts((unsigned)rows, values, num_lines, readouts, num_lines);
  char text[PLOT_READOUT_VALUE_SIZE];
  for (unsigned i = 0; i < num_readouts; ++i) {
    if (!nvtop_format_plot_readout(text, sizeof(text), readouts[i].value))
      continue;
    wcolor_set(win, plot_line_colors[readouts[i].line], NULL);
    mvwaddnstr(win, readouts[i].row, (int)PLOT_READOUT_LEFT_PAD, text, (int)PLOT_READOUT_VALUE_WIDTH);
  }
  wstandend(win);
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
