/*
 *
 * Copyright (C) 2018 Maxime Schmitt <maxime.schmitt91@gmail.com>
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

#ifndef __PLOT_H_
#define __PLOT_H_

#include "nvtop/common.h"

#include <ncurses.h>
#include <stdbool.h>
#include <stddef.h>

void nvtop_line_plot(WINDOW *win, size_t num_data, const double *data, unsigned num_plots, bool legend_left,
                     char legend[MAX_LINES_PER_PLOT][PLOT_MAX_LEGEND_SIZE]);

// Draw the current value of every line of a chart in the window the frame left
// free at its right edge, one row per line, in the color of that line. The
// window is the readout gutter of that chart and nothing else: it holds no
// curve, no border, no legend and no time axis, so a value never overwrites
// any of them. num_data and data are the samples the chart draws, legend_left
// says which end of the chart the oldest sample lies at. The window must have
// been erased by the caller, as the plot window is.
void nvtop_plot_readouts(WINDOW *win, size_t num_data, const double *data, unsigned num_lines, bool legend_left);

void draw_rectangle(WINDOW *win, unsigned startX, unsigned startY, unsigned sizeX, unsigned sizeY);

#endif // __PLOT_H_
