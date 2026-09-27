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

// Standalone (no GPU, no gtest) check of the section layout: the monitoring
// screen from the top of the terminal to the bottom, the CPU device section
// first, the GPU device sections after it, the process list, the unused space
// and the shortcut bar anchored to the last row.

#include "nvtop/interface_layout_selection.h"
#include "nvtop/plot_geometry.h"

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

#define MAX_DEVICES 8u

struct SectionLayout {
  unsigned rows, cols; // Terminal size, the shortcut line included
  unsigned host_lines; // Percentage lines of the CPU chart
  unsigned num_devices;
  struct layout_section sections[MAX_LAYOUT_SECTIONS];
  struct window_position device_positions[MAX_DEVICES];
  struct window_position plot_positions[MAX_CHARTS];
  unsigned map_device_to_plot[MAX_DEVICES];
  struct layout_result result;
};

// The interface lays out everything but the shortcut line, which the layout
// anchors to the row right after the space it was given.
static void compute(struct SectionLayout *layout, unsigned device_count, unsigned header_rows, unsigned header_cols,
                    unsigned terminal_rows, unsigned terminal_cols, unsigned host_lines, bool hide_processes) {
  memset(layout, 0, sizeof(*layout));
  layout->rows = terminal_rows;
  layout->cols = terminal_cols;
  layout->num_devices = device_count;
  layout->host_lines = host_lines;

  static nvtop_interface_gpu_opts gpu_opts[MAX_DEVICES];
  for (unsigned i = 0; i < device_count; ++i) {
    nvtop_interface_gpu_opts options = {.to_draw = plot_default_draw_info()};
    gpu_opts[i] = options;
  }

  const struct layout_request request = {
      .devices_count = device_count,
      .device_header_rows = header_rows,
      .device_header_cols = header_cols,
      .rows = terminal_rows > 0 ? terminal_rows - 1 : 0,
      .cols = terminal_cols,
      .gpu_opts = gpu_opts,
      .process_displayed = process_default_displayed_field(),
      .hide_processes = hide_processes,
      .host_chart_lines = host_lines,
      .host_detail_rows = host_lines > 0 ? HOST_DETAIL_LINE_COUNT : 0,
  };
  struct layout_result result;
  memset(&result, 0, sizeof(result));
  result.sections = layout->sections;
  result.device_positions = layout->device_positions;
  result.plot_positions = layout->plot_positions;
  result.map_device_to_plot = layout->map_device_to_plot;
  compute_monitoring_layout(&request, &result);
  layout->result = result;
}

static const struct layout_section *section_at(const struct SectionLayout *layout, unsigned index) {
  if (index >= layout->result.num_sections)
    return NULL;
  return &layout->sections[index];
}

static bool is_kind(const struct SectionLayout *layout, unsigned index, enum layout_section_kind kind) {
  const struct layout_section *section = section_at(layout, index);
  return section && section->kind == kind;
}

static bool window_overlap(struct window_position a, struct window_position b) {
  if (a.sizeX == 0 || a.sizeY == 0 || b.sizeX == 0 || b.sizeY == 0)
    return false;
  return a.posX < b.posX + b.sizeX && b.posX < a.posX + a.sizeX && a.posY < b.posY + b.sizeY &&
         b.posY < a.posY + a.sizeY;
}

static bool window_inside(struct window_position win, struct window_position container) {
  if (win.sizeX == 0 || win.sizeY == 0)
    return true;
  return win.posX >= container.posX && win.posX + win.sizeX <= container.posX + container.sizeX &&
         win.posY >= container.posY && win.posY + win.sizeY <= container.posY + container.sizeY;
}

// The narrowest chart that can draw that many lines and read out their current
// value: the box drawing, the readout gutter, and ten columns per line.
static unsigned min_chart_cols(unsigned lines) { return PLOT_COLUMNS_NOT_DATA + 10u * lines; }

// The visible devices are an ordered prefix of the requested devices, and the
// only devices the layout says anything about: every device with a section, a
// header and a chart comes first, in index order, and every device after it is
// entirely absent, with no position, no header and no chart to be drawn in.
static void check_visible_prefix_is_the_only_output(const struct SectionLayout *layout, const char *trace) {
  const unsigned visible = layout->result.num_plots;
  CHECK(visible <= layout->num_devices);
  for (unsigned dev = 0; dev < layout->num_devices; ++dev) {
    const struct window_position *header = &layout->result.device_positions[dev];
    const unsigned plot = layout->result.map_device_to_plot[dev];
    if (dev < visible) {
      CHECK(plot == dev);
      CHECK(header->sizeX > 0u && header->sizeY > 0u);
      CHECK(layout->result.plot_positions[dev].sizeX > 0u && layout->result.plot_positions[dev].sizeY > 0u);
      continue;
    }
    if (plot != MAX_CHARTS)
      printf("note: %s leaves device %u with a chart\n", trace, dev);
    CHECK(plot == MAX_CHARTS);
    CHECK(header->posX == 0u && header->posY == 0u && header->sizeX == 0u && header->sizeY == 0u);
  }
  // No two visible GPUs share a chart, and no chart is shared with a device
  // that has no section of its own.
  for (unsigned first = 0; first < visible; ++first)
    for (unsigned second = first + 1; second < visible; ++second) {
      CHECK(layout->result.map_device_to_plot[first] != layout->result.map_device_to_plot[second]);
      CHECK(layout->result.plot_positions[first].posY != layout->result.plot_positions[second].posY);
    }
}

// 162 columns by 62 rows, one GPU and the two host metrics: the shape of the
// reference screenshot terminal.
static void test_sections_of_a_cpu_and_a_gpu(void) {
  struct SectionLayout layout;
  compute(&layout, 1u, 3u, 55u, 62u, 162u, 2u, false);

  CHECK(layout.result.num_plots == 1u);
  CHECK(layout.result.num_sections == 4u);
  CHECK(is_kind(&layout, 0u, layout_section_host_device));
  CHECK(is_kind(&layout, 1u, layout_section_gpu_device));
  CHECK(is_kind(&layout, 2u, layout_section_processes));
  CHECK(is_kind(&layout, 3u, layout_section_shortcut));

  const struct layout_section *cpu = section_at(&layout, 0u);
  // The detail subsection is a fixed height block, right above its own chart.
  CHECK(cpu->detail.sizeY == HOST_DETAIL_LINE_COUNT);
  CHECK(cpu->detail.posY == 0u);
  CHECK(cpu->chart.posY == cpu->detail.posY + cpu->detail.sizeY);
  CHECK(cpu->chart.sizeY >= LAYOUT_MIN_CHART_ROWS);
  CHECK(layout.result.host_detail.posY == cpu->detail.posY);
  CHECK(layout.result.host_chart.posY == cpu->chart.posY);

  const struct layout_section *gpu = section_at(&layout, 1u);
  // One blank row between the two device sections, and none between the GPU
  // detail and its own chart.
  CHECK(gpu->area.posY == cpu->area.posY + cpu->area.sizeY + LAYOUT_SECTION_GAP_ROWS);
  CHECK(gpu->chart.posY == gpu->detail.posY + gpu->detail.sizeY);
  CHECK(layout.result.device_positions[0].posY >= gpu->detail.posY);
  CHECK(layout.result.device_positions[0].posY + layout.result.device_positions[0].sizeY <= gpu->chart.posY);
  CHECK(layout.result.plot_positions[0].posY == gpu->chart.posY);
  CHECK(layout.result.plot_positions[0].sizeX == gpu->chart.sizeX);

  // Charts of the same terminal have the same height.
  CHECK(cpu->chart.sizeY == gpu->chart.sizeY);

  // The process list follows the last section with a blank row between them,
  // and the shortcut bar is the last row of the terminal.
  const struct layout_section *process = section_at(&layout, 2u);
  CHECK(process->area.posY == gpu->area.posY + gpu->area.sizeY + LAYOUT_SECTION_GAP_ROWS);
  CHECK(process->area.posY + process->area.sizeY <= layout.rows - 1u);
  CHECK(layout.result.shortcut.posY == layout.rows - 1u);
  CHECK(layout.result.shortcut.sizeY == 1u);
  CHECK(layout.result.shortcut.sizeX == layout.cols);

  // No device header above the CPU chart: the CPU section is only the CPU.
  CHECK(gpu->device_first == 0u && gpu->device_count == 1u);
  CHECK(cpu->device_count == 0u);
}

// Every GPU header sits directly above the chart that draws its devices, and
// the GPU order is the device order. Every visible GPU is a section of its own
// with a chart of its own: a device the terminal cannot show has neither a
// header nor a chart, so a header never claims a chart it does not draw.
static void test_every_gpu_header_is_above_its_own_chart(void) {
  const unsigned device_counts[] = {1u, 2u, 3u, 4u, 8u};
  const unsigned terminals[][2] = {{30u, 162u}, {40u, 162u}, {62u, 162u}, {62u, 90u}, {100u, 600u}};
  for (unsigned count_id = 0; count_id < sizeof(device_counts) / sizeof(*device_counts); ++count_id) {
    for (unsigned term_id = 0; term_id < sizeof(terminals) / sizeof(*terminals); ++term_id) {
      struct SectionLayout layout;
      compute(&layout, device_counts[count_id], 3u, 55u, terminals[term_id][0], terminals[term_id][1], 2u, false);
      char trace[64];
      snprintf(trace, sizeof(trace), "%u devices, %ux%u", device_counts[count_id], terminals[term_id][0],
               terminals[term_id][1]);
      unsigned gpu_sections = 0;
      unsigned last_header_bottom = 0;
      for (unsigned i = 0; i < layout.result.num_sections; ++i) {
        const struct layout_section *section = &layout.sections[i];
        if (section->kind != layout_section_gpu_device)
          continue;
        ++gpu_sections;
        // One device, one chart, the header of the device right above it.
        CHECK(section->device_count == 1u);
        CHECK(section->plot_count == 1u);
        CHECK(section->device_first < layout.num_devices);
        CHECK(section->plot_first == section->device_first);
        CHECK(section->detail.sizeY == 3u);
        CHECK(section->chart.sizeY >= LAYOUT_MIN_CHART_ROWS);
        CHECK(section->chart.posY == section->detail.posY + section->detail.sizeY);
        CHECK(section->area.sizeY == section->detail.sizeY + section->chart.sizeY);
        if (gpu_sections > 1u) {
          // The sections follow each other, device index after device index,
          // with one blank row between two of them.
          CHECK(section->device_first == gpu_sections - 1u);
          CHECK(section->area.posY == last_header_bottom + LAYOUT_SECTION_GAP_ROWS);
        }
        last_header_bottom = section->area.posY + section->area.sizeY;
        const unsigned dev = section->device_first;
        const struct window_position *header = &layout.result.device_positions[dev];
        CHECK(header->posY == section->detail.posY);
        CHECK(header->posY + header->sizeY == section->chart.posY);
        CHECK(layout.result.map_device_to_plot[dev] == section->plot_first);
        CHECK(layout.result.plot_positions[section->plot_first].posY == section->chart.posY);
        CHECK(layout.result.plot_positions[section->plot_first].sizeX == section->chart.sizeX);
      }
      // The visible devices are the whole story: one section per visible GPU,
      // and the devices the terminal cannot show are not shown at all.
      CHECK(gpu_sections == layout.result.num_plots);
      check_visible_prefix_is_the_only_output(&layout, trace);
    }
  }
}

// Three GPUs in a terminal tall enough for all of them: three sections, three
// charts, device 0 in chart 0, device 1 in chart 1, device 2 in chart 2, each
// header immediately above its own chart and nothing else.
static void test_three_gpus_are_three_independent_sections(void) {
  struct SectionLayout layout;
  compute(&layout, 3u, 3u, 55u, 100u, 162u, 2u, false);

  CHECK(layout.result.num_plots == 3u);
  unsigned gpu_sections = 0;
  const struct layout_section *sections[3] = {NULL, NULL, NULL};
  for (unsigned i = 0; i < layout.result.num_sections; ++i) {
    if (layout.sections[i].kind != layout_section_gpu_device)
      continue;
    CHECK(gpu_sections < 3u);
    if (gpu_sections < 3u)
      sections[gpu_sections] = &layout.sections[i];
    ++gpu_sections;
  }
  CHECK(gpu_sections == 3u);
  CHECK(layout.result.num_sections == 6u); // cpu, gpu0, gpu1, gpu2, processes, shortcut
  CHECK(is_kind(&layout, 0u, layout_section_host_device));
  CHECK(is_kind(&layout, 4u, layout_section_processes));
  CHECK(is_kind(&layout, 5u, layout_section_shortcut));

  for (unsigned dev = 0; dev < 3u && dev < gpu_sections; ++dev) {
    const struct layout_section *section = sections[dev];
    CHECK(section->device_count == 1u);
    CHECK(section->device_first == dev);
    CHECK(section->plot_count == 1u);
    CHECK(section->plot_first == dev);
    CHECK(layout.result.map_device_to_plot[dev] == dev);
    // The header of the device, directly above the chart of the same device.
    CHECK(layout.result.device_positions[dev].posY == section->detail.posY);
    CHECK(section->chart.posY == section->detail.posY + section->detail.sizeY);
    CHECK(layout.result.plot_positions[dev].posY == section->chart.posY);
    CHECK(layout.result.plot_positions[dev].sizeY == section->chart.sizeY);
    CHECK(section->detail.posY > 0u);
    // Every visible chart has the same height, the CPU chart included.
    CHECK(section->chart.sizeY == layout.result.host_chart.sizeY);
    CHECK(section->chart.sizeY >= LAYOUT_MIN_CHART_ROWS);
    // A full row to itself: as wide as the CPU chart of the same terminal.
    CHECK(section->chart.sizeX == layout.result.host_chart.sizeX);
    // One blank row between two device sections.
    if (dev > 0u) {
      const struct layout_section *previous = sections[dev - 1u];
      CHECK(section->area.posY == previous->area.posY + previous->area.sizeY + LAYOUT_SECTION_GAP_ROWS);
    }
  }
  check_visible_prefix_is_the_only_output(&layout, "three gpus");
}

// A terminal too short for every GPU section: the output is a complete ordered
// prefix of the GPU sections, and the omitted GPUs have no header, no chart and
// no mapping. Nothing is drawn halfway.
static void test_short_terminal_keeps_a_complete_ordered_prefix(void) {
  const unsigned terminals[][2] = {{24u, 162u}, {30u, 162u}, {36u, 162u}, {40u, 90u}, {12u, 162u}};
  for (unsigned term_id = 0; term_id < sizeof(terminals) / sizeof(*terminals); ++term_id) {
    struct SectionLayout layout;
    compute(&layout, 3u, 3u, 55u, terminals[term_id][0], terminals[term_id][1], 2u, false);
    char trace[64];
    snprintf(trace, sizeof(trace), "%ux%u", terminals[term_id][0], terminals[term_id][1]);

    // The GPU sections come in index order and stop at the first device that
    // does not fit: never a hole, never a device that comes back after one.
    unsigned gpu_sections = 0;
    for (unsigned i = 0; i < layout.result.num_sections; ++i) {
      const struct layout_section *section = &layout.sections[i];
      if (section->kind != layout_section_gpu_device)
        continue;
      CHECK(section->device_first == gpu_sections);
      CHECK(section->device_count == 1u);
      CHECK(section->plot_count == 1u);
      ++gpu_sections;
    }
    CHECK(gpu_sections == layout.result.num_plots);
    CHECK(gpu_sections < 3u); // 24 rows and three of them do not fit together
    check_visible_prefix_is_the_only_output(&layout, trace);

    // Whatever is left is still a valid screen: the sections stack up in order,
    // the process list follows them and the shortcut bar stays last.
    const struct window_position screen = {0, 0, layout.cols, layout.rows - 1u};
    CHECK(layout.result.shortcut.posY == layout.rows - 1u);
    CHECK(window_inside(layout.result.process, screen));
    unsigned previous_bottom = 0;
    for (unsigned i = 0; i < layout.result.num_sections; ++i) {
      const struct layout_section *section = &layout.sections[i];
      if (section->kind == layout_section_shortcut)
        continue;
      if (section->kind == layout_section_unused)
        continue;
      CHECK(section->area.posY >= previous_bottom);
      CHECK(section->area.posY + section->area.sizeY <= layout.rows);
      previous_bottom = section->area.posY + section->area.sizeY + LAYOUT_SECTION_GAP_ROWS;
    }
  }
}

// Both host metrics off: the CPU device is gone, its gap row with it, and the
// first GPU section starts at the top of the terminal.
static void test_host_disabled_removes_the_cpu_section(void) {
  struct SectionLayout with_host, without_host;
  compute(&with_host, 2u, 3u, 55u, 62u, 162u, 2u, false);
  compute(&without_host, 2u, 3u, 55u, 62u, 162u, 0u, false);

  CHECK(is_kind(&with_host, 0u, layout_section_host_device));
  CHECK(!is_kind(&without_host, 0u, layout_section_host_device));
  CHECK(without_host.result.num_sections == with_host.result.num_sections - 1u);
  CHECK(without_host.result.host_detail.sizeY == 0u);
  CHECK(without_host.result.host_chart.sizeY == 0u);

  // The GPU sections move down by the whole CPU section, gap included.
  const unsigned host_section_rows = with_host.sections[0].area.sizeY + LAYOUT_SECTION_GAP_ROWS;
  CHECK(with_host.sections[1].detail.posY == without_host.sections[0].detail.posY + host_section_rows);
  CHECK(without_host.sections[0].detail.posY == 0u);
  // The GPU charts keep their order and their width. The first one is pushed
  // down by the whole CPU section; the ones after it never come before the
  // place they had without it, even though every chart is shorter with it.
  CHECK(with_host.result.num_plots == without_host.result.num_plots);
  for (unsigned plot = 0; plot < without_host.result.num_plots; ++plot) {
    CHECK(with_host.result.plot_positions[plot].posX == without_host.result.plot_positions[plot].posX);
    CHECK(with_host.result.plot_positions[plot].sizeX == without_host.result.plot_positions[plot].sizeX);
    CHECK(with_host.result.plot_positions[plot].posY >= without_host.result.plot_positions[plot].posY);
    // The order of the charts never changes.
    if (plot > 0u)
      CHECK(with_host.result.plot_positions[plot].posY > with_host.result.plot_positions[plot - 1u].posY);
    if (plot == 0u)
      CHECK(with_host.result.plot_positions[plot].posY ==
            without_host.result.plot_positions[plot].posY + host_section_rows);
    // Neither layout lets a chart reach the process list.
    CHECK(with_host.result.plot_positions[plot].posY + with_host.result.plot_positions[plot].sizeY <=
          with_host.result.process.posY);
    CHECK(without_host.result.plot_positions[plot].posY + without_host.result.plot_positions[plot].sizeY <=
          without_host.result.process.posY);
  }

  // The layout stays valid: charts in bounds, no overlap, process below.
  const struct window_position screen = {0, 0, without_host.cols, without_host.rows - 1u};
  CHECK(window_inside(without_host.result.process, screen));
  for (unsigned plot = 0; plot < without_host.result.num_plots; ++plot) {
    CHECK(window_inside(without_host.result.plot_positions[plot], screen));
    CHECK(without_host.result.plot_positions[plot].sizeY >= LAYOUT_MIN_CHART_ROWS);
    // Every chart ends above the process list, the blank row between them.
    CHECK(without_host.result.plot_positions[plot].posY + without_host.result.plot_positions[plot].sizeY <=
          without_host.result.process.posY);
  }
}

// One metric or two: the CPU stays a single device with a single chart.
static void test_one_or_two_host_lines_is_one_cpu_device(void) {
  struct SectionLayout one, two;
  compute(&one, 1u, 3u, 55u, 62u, 162u, 1u, false);
  compute(&two, 1u, 3u, 55u, 62u, 162u, 2u, false);

  CHECK(is_kind(&one, 0u, layout_section_host_device));
  CHECK(is_kind(&two, 0u, layout_section_host_device));
  CHECK(one.result.num_sections == two.result.num_sections);
  CHECK(one.result.host_detail.sizeY == two.result.host_detail.sizeY);
  // A single chart either way, on the same row, of the same height.
  CHECK(one.result.host_chart.sizeY == two.result.host_chart.sizeY);
  CHECK(one.result.host_chart.posY == two.result.host_chart.posY);
  // The two lines share the width of the row, one line owns all of it.
  CHECK(one.result.host_chart.sizeX >= two.result.host_chart.sizeX);
  CHECK(one.result.host_chart.sizeX >= min_chart_cols(1u));
  CHECK(two.result.host_chart.sizeX >= min_chart_cols(2u));
}

// The small terminal fallbacks: nothing overlaps and nothing leaves the
// terminal, whatever the size.
static void test_small_terminals_stay_valid(void) {
  const unsigned device_counts[] = {0u, 1u, 2u, 4u};
  const unsigned rows_values[] = {1u, 2u, 3u, 6u, 10u, 14u, 18u, 24u, 32u};
  const unsigned cols_values[] = {1u, 10u, 25u, 30u, 45u, 60u, 120u};
  for (unsigned count_id = 0; count_id < sizeof(device_counts) / sizeof(*device_counts); ++count_id) {
    for (unsigned row_id = 0; row_id < sizeof(rows_values) / sizeof(*rows_values); ++row_id) {
      for (unsigned col_id = 0; col_id < sizeof(cols_values) / sizeof(*cols_values); ++col_id) {
        for (unsigned host_id = 0; host_id < 2u; ++host_id) {
          struct SectionLayout layout;
          compute(&layout, device_counts[count_id], 3u, 55u, rows_values[row_id], cols_values[col_id], host_id + 1u,
                  false);
          char trace[80];
          snprintf(trace, sizeof(trace), "%u devices, %ux%u, %u host lines", device_counts[count_id],
                   rows_values[row_id], cols_values[col_id], host_id + 1u);
          const struct window_position screen = {0, 0, layout.cols, layout.rows - 1u};
          struct window_position windows[2u * MAX_DEVICES + MAX_CHARTS + 2u];
          unsigned num_windows = 0;
          struct window_position drawn[2u + MAX_CHARTS + 1u];
          unsigned num_drawn = 0;
          if (layout.result.host_detail.sizeY > 0) {
            windows[num_windows++] = layout.result.host_detail;
            drawn[num_drawn++] = layout.result.host_detail;
            CHECK(layout.result.host_detail.sizeY == HOST_DETAIL_LINE_COUNT);
          }
          if (layout.result.host_chart.sizeY > 0) {
            windows[num_windows++] = layout.result.host_chart;
            drawn[num_drawn++] = layout.result.host_chart;
            CHECK(layout.result.host_chart.sizeY >= LAYOUT_MIN_CHART_ROWS);
            // Room for the frame, the labels and the readout gutter of a chart
            // that is drawn at all.
            CHECK(layout.result.host_chart.sizeX > PLOT_COLUMNS_NOT_DATA);
          }
          for (unsigned dev = 0; dev < layout.num_devices; ++dev)
            windows[num_windows++] = layout.result.device_positions[dev];
          for (unsigned plot = 0; plot < layout.result.num_plots; ++plot) {
            windows[num_windows++] = layout.result.plot_positions[plot];
            drawn[num_drawn++] = layout.result.plot_positions[plot];
            CHECK(layout.result.plot_positions[plot].sizeY >= LAYOUT_MIN_CHART_ROWS);
            CHECK(layout.result.plot_positions[plot].sizeX > PLOT_COLUMNS_NOT_DATA);
          }
          if (layout.result.process.sizeY > 0) {
            windows[num_windows++] = layout.result.process;
            drawn[num_drawn++] = layout.result.process;
          }
          for (unsigned first = 0; first < num_windows; ++first)
            for (unsigned second = first + 1; second < num_windows; ++second)
              CHECK(!window_overlap(windows[first], windows[second]));
          // Charts, the CPU block and the process list never leave the
          // terminal. A device block may be wider or taller than a terminal
          // that cannot hold it: ncurses clips it, and the shortcut bar is
          // refreshed after it, the way it always was.
          for (unsigned win = 0; win < num_drawn; ++win) {
            CHECK(window_inside(drawn[win], screen));
            CHECK(layout.result.shortcut.posY >= drawn[win].posY + drawn[win].sizeY);
          }
          // The shortcut bar never moves.
          CHECK(layout.result.shortcut.posY == layout.rows - 1u);
          CHECK(layout.result.shortcut.sizeY == 1u);
          // The GPUs that are shown are an ordered prefix of the request, and
          // the others have neither a header nor a chart.
          check_visible_prefix_is_the_only_output(&layout, trace);
        }
      }
    }
  }
}

// The rows of a detail block below its title row line up with the vertical Y
// axis of the chart of the same device, whatever the terminal, and the title
// row itself stays where the section puts it.
static void test_detail_rows_line_up_with_the_chart_axis(void) {
  struct SectionLayout layout;
  compute(&layout, 2u, 3u, 55u, 62u, 162u, 2u, false);

  // The CPU device: its title row at the section column, the CPU and the RAM
  // rows below it at the column the chart draws its axis at.
  CHECK(layout.result.host_detail.posX == 0u);
  unsigned indent = layout_detail_indent(&layout.result.host_detail, &layout.result.host_chart);
  CHECK(layout.result.host_detail.posX + indent == layout.result.host_chart.posX + PLOT_Y_AXIS_COL);
  CHECK(indent > 0u);
  // The indented rows stay inside the block, which is as wide as the terminal.
  CHECK(layout.result.host_detail.sizeX > indent);

  // Every GPU device, in a terminal wide enough for its whole header.
  for (unsigned dev = 0; dev < layout.result.num_plots; ++dev) {
    const struct window_position *header = &layout.result.device_positions[dev];
    const struct window_position *chart = &layout.result.plot_positions[dev];
    indent = layout_detail_indent(header, chart);
    CHECK(header->posX + indent == chart->posX + PLOT_Y_AXIS_COL);
    CHECK(indent < header->sizeX);
  }

  // A terminal as narrow as the device header: the header has no column of its
  // own to lose, and it still lines up with the axis of its chart.
  struct SectionLayout narrow;
  compute(&narrow, 1u, 3u, 55u, 40u, 55u, 2u, false);
  CHECK(narrow.result.device_positions[0].posX == 0u);
  indent = layout_detail_indent(&narrow.result.device_positions[0], &narrow.result.plot_positions[0]);
  CHECK(indent == PLOT_Y_AXIS_COL);
  CHECK(narrow.result.plot_positions[0].sizeX > PLOT_COLUMNS_NOT_DATA);

  // Nothing to align the rows on when there is no chart, and nothing to indent
  // when there is no detail block either.
  struct SectionLayout without_host;
  compute(&without_host, 1u, 3u, 55u, 62u, 162u, 0u, false);
  CHECK(without_host.result.host_chart.sizeY == 0u);
  CHECK(layout_detail_indent(&without_host.result.host_detail, &without_host.result.host_chart) == 0u);
  CHECK(layout_detail_indent(NULL, &layout.result.host_chart) == 0u);
  CHECK(layout_detail_indent(&layout.result.host_detail, NULL) == 0u);
  // A chart narrower than the frame and the gutter it needs draws nothing:
  // there is no axis for the detail rows to line up with.
  const struct window_position tiny = {0u, 0u, PLOT_COLUMNS_NOT_DATA, 10u};
  CHECK(layout_detail_indent(&layout.result.host_detail, &tiny) == 0u);
}

// The chart, its shared edge extension and its readout gutter all fit in the
// section width. Every line owns exactly as many history columns as the others,
// while the display-only edge column lets the frame approach the fixed value.
static void test_chart_pays_for_its_readout_gutter(void) {
  CHECK(PLOT_READOUT_VALUE_WIDTH == 6u);           // 100.0% is the widest value
  CHECK(PLOT_READOUT_GUTTER_SIZE == 7u); // value plus an outer-edge pad
  CHECK(PLOT_COLUMNS_NOT_DATA ==
        PLOT_HORIZONTAL_OVERHEAD + PLOT_READOUT_GUTTER_SIZE + PLOT_EDGE_EXTENSION);

  const unsigned cols_values[] = {1u, 20u, 34u, 35u, 36u, 37u, 60u, 61u, 100u, 101u, 162u, 200u};
  for (unsigned col_id = 0; col_id < sizeof(cols_values) / sizeof(*cols_values); ++col_id) {
    for (unsigned lines = 1u; lines <= MAX_LINES_PER_PLOT; ++lines) {
      struct SectionLayout layout;
      compute(&layout, 1u, 3u, 55u, 40u, cols_values[col_id], lines, false);
      if (layout.result.host_chart.sizeY == 0u)
        continue; // Too narrow for that many lines: the chart is not drawn.
      const unsigned data = layout.result.host_chart.sizeX - PLOT_COLUMNS_NOT_DATA;
      // All of the frame and the gutter, and samples for every line of it.
      CHECK(layout.result.host_chart.sizeX >= min_chart_cols(lines));
      // The samples are shared out column for column between the lines.
      CHECK(data % lines == 0u);
      // What the row offers a line cannot hold another sample of every line:
      // the chart takes as much of the row as it can, gutter included.
      CHECK(data + lines + PLOT_COLUMNS_NOT_DATA > layout.cols);
      // The gutter fits between the right border of the chart and the last
      // column of the section, with the value never reaching either of them.
      CHECK(PLOT_DATA_X_OFFSET + data + PLOT_EDGE_EXTENSION + 1u + PLOT_READOUT_GUTTER_SIZE <=
            layout.result.host_chart.sizeX);
    }
  }
}

// Resizing recomputes the positions; it does not decide anything based on the
// previous layout, so the histories the charts draw are untouched.
static void test_resize_recomputes_positions(void) {
  struct SectionLayout small_layout, big_layout, back_again;
  compute(&small_layout, 2u, 3u, 55u, 24u, 60u, 2u, false);
  compute(&big_layout, 2u, 3u, 55u, 62u, 162u, 2u, false);
  compute(&back_again, 2u, 3u, 55u, 24u, 60u, 2u, false);

  CHECK(memcmp(&small_layout.result.process, &back_again.result.process, sizeof(struct window_position)) == 0);
  CHECK(small_layout.result.num_plots == back_again.result.num_plots);
  CHECK(big_layout.result.plot_positions[0].sizeY >= small_layout.result.plot_positions[0].sizeY);
  CHECK(big_layout.result.process.posY > small_layout.result.process.posY);
}

int main(void) {
  test_sections_of_a_cpu_and_a_gpu();
  test_every_gpu_header_is_above_its_own_chart();
  test_three_gpus_are_three_independent_sections();
  test_short_terminal_keeps_a_complete_ordered_prefix();
  test_host_disabled_removes_the_cpu_section();
  test_one_or_two_host_lines_is_one_cpu_device();
  test_small_terminals_stay_valid();
  test_detail_rows_line_up_with_the_chart_axis();
  test_chart_pays_for_its_readout_gutter();
  test_resize_recomputes_positions();
  printf("%s: %u checks, %u failures\n", failures ? "FAILED" : "PASSED", checks, failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
