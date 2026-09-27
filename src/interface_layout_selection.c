#include "nvtop/interface_layout_selection.h"
#include "nvtop/interface.h"
#include "nvtop/interface_options.h"
#include "nvtop/plot_geometry.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define max(a, b) ((a) > (b) ? (a) : (b))
#define min(a, b) ((a) < (b) ? (a) : (b))

static unsigned min_rows_taken_by_process(unsigned rows, unsigned num_devices) {
  return 1 + max(5, min(rows / 4, num_devices * 3));
}

// The narrowest chart that can draw that many percentage lines and read out
// their current value: the box drawing, the readout gutter, and ten columns of
// data per line.
static unsigned min_plot_cols(unsigned num_data_info_to_plot) {
  return PLOT_COLUMNS_NOT_DATA + 10u * num_data_info_to_plot;
}

// The monitoring screen, section by section //////////////////////////////////
//
// Everything the interface draws is a section, and the sections follow each
// other from the top of the terminal to the bottom, the keyboard shortcut bar
// excepted, which stays anchored to the last row of the terminal:
//
//   +-----------------------------------------------------+
//   | Device CPU [model name]            CORES 8C/16T     | <- CPU detail block
//   | CPU  12.5%   FREQ 3.40GHz   LOAD 0.42 / 0.38 / 0.35 |
//   | RAM  3.21/31.26 GiB 10.3%  AVAIL 28.05 GiB  SWAP .. |
//   | [ combined CPU/RAM chart ]                          | <- CPU chart
//   +-----------------------------------------------------+
//                                                         <- one blank gap row
//   | Device 0 [GPU name] ...                             | <- GPU0 detail block
//   | [ GPU0 chart ]                                      | <- GPU0 chart
//   +-----------------------------------------------------+
//                                                         <- one blank gap row
//   | Device 1 [GPU name] ...                             |
//   | [ GPU1 chart ]                                      |
//   ...
//   | process list                                        |
//   | flexible unused space                              |
//   | keyboard shortcuts (last terminal row)              |
//   +-----------------------------------------------------+
//
// Every device is a section of its own, the whole host CPU like the GPUs: one
// fixed height detail block, right above one chart of its own that draws that
// device only, and one blank row after the whole section. Two devices never
// share a chart and never share a section.
//
// The terminal is not always tall enough to give a chart of at least the
// minimum chart height to every device. Then whole sections are left out, in a
// fixed order: the CPU device first, then the GPU devices from the highest
// index to the lowest, always recomputing the shared chart height in between. A
// device that is left out has no header, no chart and no chart to be mapped to,
// so that a header never introduces somebody else's chart.

// The vertical Y axis of a chart, in the coordinates of the terminal.
static unsigned chart_y_axis_column(const struct window_position *chart) { return chart->posX + PLOT_Y_AXIS_COL; }

// The first row of the detail block of a device is its title row, and it stays
// where the section puts it. Every row below it is indented so that its first
// field starts at the column the chart of that same device draws its vertical Y
// axis at, which lines the values up with the curves they belong to. A detail
// block, or the chart that goes with it, that the terminal cannot show has no
// rows to indent at all.
unsigned layout_detail_indent(const struct window_position *detail, const struct window_position *chart) {
  if (!detail || !chart)
    return 0u;
  if (detail->sizeX == 0u || detail->sizeY == 0u || chart->sizeX == 0u || chart->sizeY == 0u)
    return 0u;
  // A chart that narrow has no data column at all and is not drawn: there is
  // no axis for the detail rows to line up with.
  if (chart->sizeX <= PLOT_COLUMNS_NOT_DATA)
    return 0u;
  const unsigned axis = chart_y_axis_column(chart);
  return axis > detail->posX ? axis - detail->posX : 0u;
}

const char *layout_section_kind_name(enum layout_section_kind kind) {
  switch (kind) {
  case layout_section_host_device:
    return "cpu-device";
  case layout_section_gpu_device:
    return "gpu-device";
  case layout_section_processes:
    return "processes";
  case layout_section_unused:
    return "unused";
  case layout_section_shortcut:
    return "shortcut";
  case layout_section_kind_count:
    break;
  }
  return "unknown";
}

static unsigned saturating_subtraction(unsigned value, unsigned subtraction) {
  return value > subtraction ? value - subtraction : 0;
}

// The bounding box of two windows, ignoring the empty ones.
static struct window_position bounding_box(struct window_position box, struct window_position added) {
  if (added.sizeX == 0 || added.sizeY == 0)
    return box;
  if (box.sizeX == 0 || box.sizeY == 0)
    return added;
  unsigned left = min(box.posX, added.posX);
  unsigned right = max(box.posX + box.sizeX, added.posX + added.sizeX);
  unsigned top = min(box.posY, added.posY);
  unsigned bottom = max(box.posY + box.sizeY, added.posY + added.sizeY);
  return (struct window_position){left, top, right - left, bottom - top};
}

// The detail block of a device that owns a section of its own: it is alone on
// its row, so it starts one column in when the terminal is wider than the
// block, the way a row of device headers always did.
static struct window_position device_detail_position(unsigned cols, unsigned device_header_cols,
                                                     unsigned device_header_rows, unsigned pos_y) {
  unsigned pos_x = cols > device_header_cols ? 1u : 0u;
  return (struct window_position){pos_x, pos_y, device_header_cols, device_header_rows};
}

// The position of a chart that owns a whole chart row, as wide as the terminal
// allows, its drawing columns shared out between the lines it draws. Every
// chart of the screen is alone on its row and uses this one calculation, the
// combined CPU/RAM chart like the GPU charts.
static struct window_position full_row_chart_position(unsigned cols, unsigned num_lines, unsigned pos_y,
                                                      unsigned height) {
  if (num_lines == 0 || num_lines > MAX_LINES_PER_PLOT)
    return (struct window_position){0, 0, 0, 0};
  // What is left of the row once the labels, the borders and the readout
  // gutter are taken is shared out between the lines, column for column, so
  // that every line of the chart has exactly as many samples as the others.
  unsigned data_cols = saturating_subtraction(cols, PLOT_COLUMNS_NOT_DATA);
  data_cols -= data_cols % num_lines;
  return (struct window_position){0, pos_y, PLOT_COLUMNS_NOT_DATA + data_cols, height};
}

// How many percentage lines the chart of a device draws. A device that draws
// nothing has no chart, and therefore no header either.
static unsigned device_plot_lines(const nvtop_interface_gpu_opts *gpu_opts, unsigned dev) {
  if (!gpu_opts)
    return 0;
  return plot_count_draw_info(gpu_opts[dev].to_draw);
}

static struct layout_section make_section(enum layout_section_kind kind, unsigned device_first, unsigned device_count,
                                          unsigned plot_first, unsigned plot_count, struct window_position detail,
                                          struct window_position chart) {
  struct layout_section section;
  section.kind = kind;
  section.device_first = device_first;
  section.device_count = device_count;
  section.plot_first = plot_first;
  section.plot_count = plot_count;
  section.detail = detail;
  section.chart = chart;
  section.area = bounding_box(detail, chart);
  return section;
}

void compute_monitoring_layout(const struct layout_request *request, struct layout_result *result) {
  if (!request || !result)
    return;

  const unsigned devices_count = min(request->devices_count, MAX_CHARTS);
  const unsigned rows = request->rows;
  const unsigned cols = request->cols;
  const unsigned device_header_rows = request->device_header_rows;
  const unsigned device_header_cols = request->device_header_cols;
  const unsigned host_lines = min(request->host_chart_lines, (unsigned)MAX_LINES_PER_PLOT);
  const unsigned host_detail_rows = request->host_detail_rows;
  const bool processes_requested =
      process_field_displayed_count(request->process_displayed) > 0 && !request->hide_processes;

  // Keep the caller arrays across the reset of the result.
  struct window_position *device_positions = result->device_positions;
  struct layout_section *given_sections = result->sections;
  struct window_position *plot_positions = result->plot_positions;
  unsigned *map_device_to_plot = result->map_device_to_plot;
  memset(result, 0, sizeof(*result));
  result->device_positions = device_positions;
  result->sections = given_sections;
  result->plot_positions = plot_positions;
  result->map_device_to_plot = map_device_to_plot;
  if (plot_positions) {
    for (unsigned i = 0; i < MAX_CHARTS; ++i)
      plot_positions[i] = (struct window_position){0, 0, 0, 0};
  }
  for (unsigned i = 0; i < devices_count; ++i) {
    // No device has a chart until the layout gives it one, and a device
    // without a chart never gets a header either.
    if (map_device_to_plot)
      map_device_to_plot[i] = MAX_CHARTS;
    if (device_positions)
      device_positions[i] = (struct window_position){0, 0, 0, 0};
  }
  struct layout_section sections[MAX_LAYOUT_SECTIONS];
  unsigned section_count = 0;

  // The process list takes the room the layout has always given it, and the
  // shortcut bar the last row of the terminal, which is the row right after the
  // space the layout was given.
  unsigned process_rows = processes_requested ? min_rows_taken_by_process(rows, devices_count) : 0;
  // A terminal that cannot hold the device detail blocks and a process list at
  // once shrinks the process list down to what the detail blocks leave, and
  // shows no process list at all when there is not even room for that.
  const unsigned all_detail_rows = devices_count * device_header_rows;
  if (processes_requested && rows < all_detail_rows + process_rows)
    process_rows = rows >= all_detail_rows + 2u ? rows - all_detail_rows : 0u;

  // ---------------------------------------------------------------------------
  // Which devices make a section: the visible devices are an ordered prefix
  // ---------------------------------------------------------------------------
  // A device is left out, and with it every device after it, when its own chart
  // cannot be as wide as the lines it draws need: the chart of a device is
  // alone on its row, so no device is ever made room for by shrinking another.
  const bool host_requested = host_lines > 0 && host_detail_rows > 0;
  unsigned visible_gpus = 0;
  for (; visible_gpus < devices_count; ++visible_gpus) {
    unsigned lines = device_plot_lines(request->gpu_opts, visible_gpus);
    if (lines == 0 || lines > MAX_LINES_PER_PLOT || cols < min_plot_cols(lines))
      break;
  }

  // ---------------------------------------------------------------------------
  // How tall the charts are: every visible chart shares the same height
  // ---------------------------------------------------------------------------
  // The rows the charts are left with are the rows the terminal has once the
  // fixed height detail blocks, the blank row after each device section and the
  // process list are taken. They are shared equally between the CPU chart and
  // the charts of the visible GPU devices. When that leaves a chart below the
  // minimum chart height, the CPU device goes first, then the GPU devices from
  // the highest index to the lowest, and the height is computed again.
  bool show_host = host_requested && cols >= min_plot_cols(host_lines);
  unsigned show_gpus = visible_gpus;
  unsigned chart_rows = 0;
  unsigned rows_left_for_charts = 0;
  for (;;) {
    const unsigned num_charts = (show_host ? 1u : 0u) + show_gpus;
    // One blank row after each device section, when another top level section
    // follows it. With no process list, the last device section is the last
    // section of the screen and keeps no gap row.
    const unsigned gap_rows = num_charts > 0 ? (process_rows > 0 ? num_charts : num_charts - 1u) : 0u;
    const unsigned fixed_rows = gap_rows + (show_host ? host_detail_rows : 0u) + show_gpus * device_header_rows;
    rows_left_for_charts = saturating_subtraction(rows, fixed_rows + process_rows);
    chart_rows = num_charts > 0 ? rows_left_for_charts / num_charts : 0u;
    if (process_rows > 0 && chart_rows > LAYOUT_MAX_CHART_ROWS_WITH_PROCESS)
      chart_rows = LAYOUT_MAX_CHART_ROWS_WITH_PROCESS;
    if (num_charts == 0 || chart_rows >= LAYOUT_MIN_CHART_ROWS)
      break;
    if (show_host) {
      show_host = false;
      continue;
    }
    if (show_gpus == 0)
      break;
    --show_gpus;
  }
  const unsigned num_plots = show_gpus;
  const unsigned num_charts = (show_host ? 1u : 0u) + show_gpus;
  // The rows the charts could not use go to the process list, the way the chart
  // area always gave them away, so that no blank hole opens up above it. With
  // no process list they become the unused space above the shortcut bar.
  const unsigned chart_row_leftover = saturating_subtraction(rows_left_for_charts, chart_rows * num_charts);
  if (process_rows > 0)
    process_rows += chart_row_leftover;
  result->num_plots = num_plots;
  result->chart_rows = chart_rows;
  const bool has_process = process_rows > 0;

  // ---------------------------------------------------------------------------
  // The sections laid out, in order, from the top of the terminal to the bottom
  // ---------------------------------------------------------------------------
  const unsigned device_sections = (show_host ? 1u : 0u) + show_gpus;
  unsigned emitted_sections = 0;
  unsigned pos_y = 0;
  unsigned first_detail_bottom = 0;

  if (show_host) {
    struct window_position detail = {0, pos_y, cols, host_detail_rows};
    pos_y += host_detail_rows;
    first_detail_bottom = pos_y;
    struct window_position chart = full_row_chart_position(cols, host_lines, pos_y, chart_rows);
    pos_y += chart_rows;
    result->host_detail = detail;
    result->host_chart = chart;
    if (section_count < MAX_LAYOUT_SECTIONS)
      sections[section_count++] = make_section(layout_section_host_device, 0, 0, 0, 0, detail, chart);
    ++emitted_sections;
    if (emitted_sections < device_sections || has_process)
      pos_y += LAYOUT_SECTION_GAP_ROWS;
  }

  for (unsigned dev = 0; dev < show_gpus; ++dev) {
    // The device is alone in its section: its detail block is the whole detail
    // subsection, right above the chart that draws it and nothing else.
    struct window_position detail = device_detail_position(cols, device_header_cols, device_header_rows, pos_y);
    if (device_positions)
      device_positions[dev] = detail;
    pos_y += device_header_rows;
    if (first_detail_bottom == 0)
      first_detail_bottom = pos_y;
    struct window_position chart =
        full_row_chart_position(cols, device_plot_lines(request->gpu_opts, dev), pos_y, chart_rows);
    if (plot_positions)
      plot_positions[dev] = chart;
    if (map_device_to_plot)
      map_device_to_plot[dev] = dev;
    pos_y += chart_rows;
    if (section_count < MAX_LAYOUT_SECTIONS)
      sections[section_count++] = make_section(layout_section_gpu_device, dev, 1u, dev, 1u, detail, chart);
    ++emitted_sections;
    if (emitted_sections < device_sections || has_process)
      pos_y += LAYOUT_SECTION_GAP_ROWS;
  }

  // The process list, right below the last visible device section, the blank
  // separating row included in the sections above.
  result->process = (struct window_position){0, rows - min(process_rows, rows), cols, min(process_rows, rows)};
  if (result->process.sizeY > 0 && section_count < MAX_LAYOUT_SECTIONS)
    sections[section_count++] =
        make_section(layout_section_processes, 0, 0, 0, 0, (struct window_position){0, 0, 0, 0}, result->process);

  // The space the charts could not use, right above the shortcut bar.
  unsigned unused_top = max(pos_y, result->process.posY + result->process.sizeY);
  unsigned unused_rows = saturating_subtraction(rows, unused_top);
  if (unused_rows > 0 && section_count < MAX_LAYOUT_SECTIONS)
    sections[section_count++] = make_section(layout_section_unused, 0, 0, 0, 0, (struct window_position){0, 0, 0, 0},
                                             (struct window_position){0, unused_top, cols, unused_rows});

  // The keyboard shortcut bar, anchored to the last row of the terminal, which
  // is the row right after the space the layout was given.
  result->shortcut = (struct window_position){0, rows, cols, 1};
  if (section_count < MAX_LAYOUT_SECTIONS)
    sections[section_count++] =
        make_section(layout_section_shortcut, 0, 0, 0, 0, (struct window_position){0, 0, 0, 0}, result->shortcut);

  // The setup screen is drawn over everything but the first detail block, the
  // way it used to be drawn over everything but the device headers.
  unsigned setup_pos_y = first_detail_bottom < rows ? first_detail_bottom : 0;
  result->setup = (struct window_position){0, setup_pos_y, cols, rows - setup_pos_y};

  if (given_sections) {
    for (unsigned i = 0; i < section_count; ++i)
      given_sections[i] = sections[i];
  }
  result->num_sections = section_count;
}

void compute_sizes_from_layout(unsigned devices_count, unsigned device_header_rows, unsigned device_header_cols,
                               unsigned rows, unsigned cols, const nvtop_interface_gpu_opts *gpu_opts,
                               process_field_displayed process_displayed, struct window_position *device_positions,
                               unsigned *num_plots, struct window_position plot_positions[MAX_CHARTS],
                               unsigned *map_device_to_plot, struct window_position *process_position,
                               struct window_position *setup_position, bool process_win_hide,
                               const struct host_chart_input *host_chart, struct window_position *host_chart_position) {
  struct layout_request request = {
      .devices_count = devices_count,
      .device_header_rows = device_header_rows,
      .device_header_cols = device_header_cols,
      .rows = rows,
      .cols = cols,
      .gpu_opts = gpu_opts,
      .process_displayed = process_displayed,
      .hide_processes = process_win_hide,
      .host_chart_lines = host_chart ? host_chart->num_lines : 0,
      .host_detail_rows = (host_chart && host_chart->show) ? LAYOUT_HOST_DETAIL_ROWS : 0,
  };
  struct layout_result result = {
      .sections = NULL,
      .device_positions = device_positions,
      .plot_positions = plot_positions,
      .map_device_to_plot = map_device_to_plot,
  };
  compute_monitoring_layout(&request, &result);
  if (num_plots)
    *num_plots = result.num_plots;
  if (process_position)
    *process_position = result.process;
  if (setup_position)
    *setup_position = result.setup;
  if (host_chart_position)
    *host_chart_position = result.host_chart;
}
