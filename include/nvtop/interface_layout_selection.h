/*
 *
 * Copyright (C) 2021 Maxime Schmitt <maxime.schmitt@gmail.com>
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

#ifndef INTERFACE_LAYOUT_SELECTION_H__
#define INTERFACE_LAYOUT_SELECTION_H__

#include "nvtop/host_metrics.h"
#include "nvtop/interface_common.h"
#include "nvtop/interface_options.h"

#include <stdbool.h>

struct window_position {
  unsigned posX, posY, sizeX, sizeY;
};

// Should be fine
#define MAX_CHARTS 64

// The monitoring screen, top to bottom //////////////////////////////////////
// Every section flows from the previous one, from the top of the terminal to
// the bottom, except the shortcut bar that stays anchored to the last row.
//   - the CPU device, when it is enabled: its detail block, then the combined
//     CPU/RAM chart;
//   - the GPU devices, in their index order: one section per device, each
//     detail block directly above the chart that draws that device only;
//   - the process list;
//   - the leftover space, if any;
//   - the keyboard shortcuts.
// Every device is a section of its own and owns one chart of its own: two
// devices never share a section and never share a chart. One blank row
// separates two sections. It never separates the detail subsection of a device
// from its own chart.
// A terminal that cannot give the minimum chart height to every device leaves
// whole sections out: the CPU device first, then the GPU devices from the
// highest index to the lowest. A device that is left out has no detail block,
// no chart and no chart to be mapped to.
enum layout_section_kind {
  layout_section_host_device = 0, // CPU detail block + combined CPU/RAM chart
  layout_section_gpu_device,      // One GPU detail block + its own chart
  layout_section_processes,
  layout_section_unused,
  layout_section_shortcut,
  layout_section_kind_count
};

// One blank row between two top level sections.
#define LAYOUT_SECTION_GAP_ROWS 1u

// A chart is never given fewer rows than that, and is dropped rather than
// squeezed below it.
#define LAYOUT_MIN_CHART_ROWS 7u

// A chart row is never taller than that when the process list is displayed.
#define LAYOUT_MAX_CHART_ROWS_WITH_PROCESS 23u

// The CPU detail block has as many rows as the lines it displays.
#define LAYOUT_HOST_DETAIL_ROWS HOST_DETAIL_LINE_COUNT

// Room for one section per chart, one for the CPU device, one for the process
// list, one for the leftover space and one for the shortcut bar.
#define MAX_LAYOUT_SECTIONS (MAX_CHARTS + 4u)

// A section of the monitoring screen. A device section holds the detail
// subsection above the chart subsection, both empty when the section has
// nothing of that kind. `area` spans the section from the top of its detail to
// the bottom of its chart, and never includes the gap that follows it.
// The device and the chart of a device section are given as an index and a
// count in the arrays of the caller. A CPU section holds the whole host CPU as
// a device of its own, without an entry in the device arrays, and its one
// chart; a GPU section holds exactly one GPU device and exactly one chart.
struct layout_section {
  enum layout_section_kind kind;
  unsigned device_first;         // GPU device of the section
  unsigned device_count;         // 1 for a GPU device section, 0 for a CPU one
  unsigned plot_first;           // Chart of the section
  unsigned plot_count;           // 1 for a device section, 0 when it has none
  struct window_position detail; // Detail subsection, empty when there is none
  struct window_position chart;  // Chart subsection, empty when there is none
  struct window_position area;   // The whole section, without the gap after it
};

// What the layout needs to know about the screen and about the metrics to
// display.
struct layout_request {
  unsigned devices_count;
  unsigned device_header_rows; // Rows of a GPU detail block
  unsigned device_header_cols; // Columns of a GPU detail block
  unsigned rows;               // Rows of the terminal, the shortcut row excluded
  unsigned cols;               // Columns of the terminal
  const nvtop_interface_gpu_opts *gpu_opts;
  process_field_displayed process_displayed;
  bool hide_processes;
  // Number of percentage lines of the combined CPU/RAM chart: 0 when both of
  // the host metrics are disabled, 1 or 2 otherwise. It is the whole existence
  // of the CPU device: no line means no CPU section and no gap for it.
  unsigned host_chart_lines;
  unsigned host_detail_rows; // Rows of the CPU detail block
};

// Where everything is. The caller owns the arrays and may leave out the ones it
// has no use for: sections, device positions, and the device to chart mapping.
struct layout_result {
  struct layout_section *sections; // Capacity MAX_LAYOUT_SECTIONS
  unsigned num_sections;
  struct window_position *device_positions; // One per requested device
  unsigned num_plots;                       // Visible GPU devices, one chart each
  struct window_position *plot_positions; // Capacity MAX_CHARTS
  unsigned *map_device_to_plot; // Device index for a visible device, MAX_CHARTS
                                // for a device the terminal cannot show
  unsigned chart_rows;          // Rows given to every chart, 0 when none fits
  struct window_position host_detail;     // The CPU detail block
  struct window_position host_chart;      // The combined CPU/RAM chart
  struct window_position process;         // The process list
  struct window_position setup;           // The setup screen, drawn over the charts
  struct window_position shortcut;        // Anchored to the last row of the terminal
};

// Compute the sections of the monitoring screen, top to bottom.
void compute_monitoring_layout(const struct layout_request *request, struct layout_result *result);

const char *layout_section_kind_name(enum layout_section_kind kind);

// Request for the combined whole host chart. The CPU and the memory
// utilization share a single chart, the way the GPU utilization and the GPU
// memory share a GPU chart. The host chart is an ordinary chart of its own
// device section: it gets the same outer dimensions, the same row height and
// the full width of a chart row, and its detail block sits right above it.
struct host_chart_input {
  bool show;          // At least one of the two host metrics is enabled
  unsigned num_lines; // Number of percentage lines of the chart: 1 or 2
};

// Same layout as compute_monitoring_layout(), filling the per device and per
// chart position arrays the way the interface used to ask for them.
void compute_sizes_from_layout(unsigned monitored_dev_count, unsigned device_header_rows, unsigned device_header_cols,
                               unsigned rows, unsigned cols, const nvtop_interface_gpu_opts *gpu_opts,
                               process_field_displayed process_field_displayed,
                               struct window_position *device_positions, unsigned *num_plots,
                               struct window_position plot_positions[MAX_CHARTS], unsigned *map_device_to_plot,
                               struct window_position *process_position, struct window_position *setup_position,
                               bool process_win_hide, const struct host_chart_input *host_chart,
                               struct window_position *host_chart_position);

#endif // INTERFACE_LAYOUT_SELECTION_H__
