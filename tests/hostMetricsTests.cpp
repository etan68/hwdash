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

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ostream>
#include <cstdlib>
#include <gtest/gtest.h>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "nvtop/extract_gpuinfo_common.h"
#include "nvtop/host_metrics.h"
#include "nvtop/interface_layout_selection.h"
#include "nvtop/interface_options.h"
}

static std::ostream &operator<<(std::ostream &os, const window_position &win) {
  return os << "(" << win.posX << "," << win.posY << ")+" << win.sizeX << "x" << win.sizeY;
}

namespace {

bool nearly(double a, double b) { return std::fabs(a - b) < 1e-9; }

bool overlap(const window_position &a, const window_position &b) {
  if (a.sizeX == 0 || a.sizeY == 0 || b.sizeX == 0 || b.sizeY == 0)
    return false;
  bool overlap_x = a.posX + a.sizeX > b.posX && b.posX + b.sizeX > a.posX;
  bool overlap_y = a.posY + a.sizeY > b.posY && b.posY + b.sizeY > a.posY;
  return overlap_x && overlap_y;
}

bool inside(const window_position &win, const window_position &container) {
  if (win.sizeX == 0 || win.sizeY == 0)
    return true;
  return win.posX >= container.posX && win.posX + win.sizeX <= container.posX + container.sizeX &&
         win.posY >= container.posY && win.posY + win.sizeY <= container.posY + container.sizeY;
}

bool same_position(const window_position &a, const window_position &b) {
  return a.posX == b.posX && a.posY == b.posY && a.sizeX == b.sizeX && a.sizeY == b.sizeY;
}

// The layout of one terminal, as the interface computes it: the sections from
// the top of the terminal to the bottom, with the CPU device first.
struct ComputedLayout {
  unsigned rows = 0, cols = 0; // Terminal size, shortcut line included
  unsigned host_lines = 0;     // Lines requested for the host chart
  unsigned header_rows = 0;    // Rows of a GPU detail block
  std::vector<window_position> headers;
  std::vector<window_position> charts; // The GPU charts, in order
  std::vector<unsigned> device_to_chart;
  std::vector<layout_section> sections;
  unsigned num_plots = 0;
  window_position host_detail = {};
  window_position host = {};
  window_position process = {};
  window_position setup = {};
  window_position shortcut = {};
};

// host_lines is the number of percentage lines of the combined host chart: 0
// keeps the chart out of the layout, 1 and 2 are the one metric and the two
// metrics cases. with_host_argument = false leaves the host chart arguments out
// of the call, the way a build without any host chart calls the layout.
ComputedLayout compute_layout(unsigned device_count, unsigned header_rows, unsigned header_cols, unsigned term_rows,
                              unsigned term_cols, unsigned host_lines, bool hide_processes = false,
                              bool with_host_argument = true) {
  ComputedLayout layout;
  layout.rows = term_rows;
  layout.cols = term_cols;
  layout.host_lines = host_lines;
  layout.header_rows = header_rows;

  std::vector<nvtop_interface_gpu_opts> gpu_opts(device_count,
                                                 nvtop_interface_gpu_opts{.to_draw = plot_default_draw_info()});
  layout.headers.resize(device_count);
  layout.device_to_chart.resize(device_count);
  std::vector<window_position> chart_positions(MAX_CHARTS);
  std::vector<layout_section> sections(MAX_LAYOUT_SECTIONS);
  unsigned num_plots = 0;

  // The interface lays out everything but the shortcut line.
  host_chart_input host = {.show = with_host_argument && host_lines > 0, .num_lines = host_lines};
  if (!with_host_argument) {
    // The legacy entry point, the way a caller without any host chart uses it.
    compute_sizes_from_layout(device_count, header_rows, header_cols, term_rows > 0 ? term_rows - 1 : 0, term_cols,
                              gpu_opts.data(), process_default_displayed_field(), layout.headers.data(), &num_plots,
                              chart_positions.data(), layout.device_to_chart.data(), &layout.process, &layout.setup,
                              hide_processes, nullptr, nullptr);
    layout.num_plots = num_plots;
  } else {
    layout_request request = {};
    request.devices_count = device_count;
    request.device_header_rows = header_rows;
    request.device_header_cols = header_cols;
    request.rows = term_rows > 0 ? term_rows - 1 : 0;
    request.cols = term_cols;
    request.gpu_opts = gpu_opts.data();
    request.process_displayed = process_default_displayed_field();
    request.hide_processes = hide_processes;
    request.host_chart_lines = host.num_lines;
    request.host_detail_rows = host.show ? LAYOUT_HOST_DETAIL_ROWS : 0;
    layout_result result = {};
    result.sections = sections.data();
    result.device_positions = layout.headers.data();
    result.plot_positions = chart_positions.data();
    result.map_device_to_plot = layout.device_to_chart.data();
    compute_monitoring_layout(&request, &result);
    layout.num_plots = result.num_plots;
    layout.host = result.host_chart;
    layout.host_detail = result.host_detail;
    layout.process = result.process;
    layout.setup = result.setup;
    layout.shortcut = result.shortcut;
    sections.resize(result.num_sections);
    layout.sections = std::move(sections);
  }
  chart_positions.resize(layout.num_plots);
  layout.charts = std::move(chart_positions);
  return layout;
}

// The chart rows, in the order the layout algorithm created them. The host
// chart is not part of layout.charts, so this describes the GPU charts only.
std::vector<unsigned> chart_rows(const ComputedLayout &layout) {
  std::vector<unsigned> rows;
  for (const auto &chart : layout.charts)
    if (std::find(rows.begin(), rows.end(), chart.posY) == rows.end())
      rows.push_back(chart.posY);
  return rows;
}

// The top of the first GPU chart row, below the host device section.
unsigned first_gpu_row_pos_y(const ComputedLayout &layout) {
  unsigned top = UINT32_MAX;
  for (const auto &chart : layout.charts)
    top = std::min(top, chart.posY);
  return top;
}

// A device the terminal cannot give a whole section has no detail block at all:
// it is not part of the screen, and no position of it means anything.
bool header_is_allocated(const ComputedLayout &layout, size_t dev) {
  return dev < layout.headers.size() && layout.headers[dev].sizeX != 0 && layout.headers[dev].sizeY != 0;
}

// The top of the first GPU detail block, the section that follows the CPU one.
// Only the devices that do have a section of their own count here.
unsigned first_gpu_detail_pos_y(const ComputedLayout &layout) {
  unsigned top = UINT32_MAX;
  for (size_t dev = 0; dev < layout.headers.size(); ++dev)
    if (header_is_allocated(layout, dev))
      top = std::min(top, layout.headers[dev].posY);
  return top;
}

// The rows the CPU device section takes: its detail block, its chart, and the
// blank row that separates it from the first GPU section.
unsigned cpu_section_rows(const ComputedLayout &layout) {
  return layout.host_detail.sizeY + layout.host.sizeY + LAYOUT_SECTION_GAP_ROWS;
}

// The layout drops the whole CPU device section when the terminal cannot give
// one more chart row to it, exactly like it drops a GPU section that does not
// fit. The GPU sections that survive are the same ones, at the same places.
bool host_chart_is_allocated(const ComputedLayout &layout) { return layout.host.sizeX != 0 && layout.host.sizeY != 0; }

// Rank of a chart among the chart rows. Every visible GPU has a chart row of
// its own, and the host chart is one more row above them all: the ranks stay
// the same even though the positions do not.
unsigned row_rank(const ComputedLayout &layout, size_t chart_id) {
  std::vector<unsigned> rows = chart_rows(layout);
  return static_cast<unsigned>(std::find(rows.begin(), rows.end(), layout.charts[chart_id].posY) - rows.begin());
}

// The host chart is a whole row of its own above the GPU charts. It must never
// change the number of visible GPU sections, the device each chart draws, their
// order or the columns of any of them. The GPU charts only share the chart
// space with one more row: they keep their full width, stay at least as high as
// the minimum chart height and the first of them moves down by exactly the
// height of the CPU device section.
void expect_gpu_charts_keep_their_arrangement(const ComputedLayout &without, const ComputedLayout &with) {
  ASSERT_EQ(with.charts.size(), without.charts.size());
  EXPECT_EQ(with.device_to_chart, without.device_to_chart);
  EXPECT_EQ(chart_rows(with).size(), chart_rows(without).size()) << "The number of chart rows changed";
  for (size_t i = 0; i < without.charts.size(); ++i) {
    EXPECT_EQ(row_rank(with, i), row_rank(without, i)) << "GPU chart " << i << " changed row";
    EXPECT_EQ(with.charts[i].posX, without.charts[i].posX) << "GPU chart " << i << " moved horizontally";
    EXPECT_EQ(with.charts[i].sizeX, without.charts[i].sizeX) << "GPU chart " << i << " changed width";
    EXPECT_EQ(with.charts[i].sizeY, with.charts[0].sizeY) << "GPU chart " << i << " does not have the chart height";
    EXPECT_LE(with.charts[i].sizeY, without.charts[i].sizeY) << "GPU chart " << i << " grew out of nowhere";
    EXPECT_GE(with.charts[i].sizeY, 7u) << "GPU chart " << i << " is below the minimum chart height";
  }
  if (host_chart_is_allocated(with) && !without.charts.empty())
    EXPECT_EQ(first_gpu_row_pos_y(with), first_gpu_row_pos_y(without) + cpu_section_rows(with))
        << "The GPU charts are not shifted down by the CPU device section";
}

// The invariants every chart of the chart area obeys, checked on the host
// chart to prove it is laid out like a GPU one.
void expect_host_chart_follows_the_chart_rules(const ComputedLayout &layout) {
  const window_position screen = {0, 0, layout.cols, layout.rows - 1};
  const window_position &host = layout.host;
  ASSERT_GT(host.sizeX, 0u) << "Empty host chart";
  ASSERT_GT(host.sizeY, 0u) << "Empty host chart";

  // Same outer frame as a GPU chart: 4 columns for the vertical scale and 2
  // rows for the frame, and the line count divides the drawing width.
  EXPECT_GE(host.sizeX, 5u + 10u * layout.host_lines) << "Host chart " << host;
  EXPECT_EQ((host.sizeX - 5u) % layout.host_lines, 0u) << "Host chart " << host << " is not a multiple of "
                                                      << layout.host_lines << " drawing columns";

  // It is the chart of the CPU device: right below the CPU detail block, with
  // no GPU detail block above it. The CPU section is the first section of the
  // screen, so every GPU chart starts below it and none shares its row.
  EXPECT_EQ(host.posX, 0u) << "Host chart " << host << " does not start on the left edge";
  EXPECT_GE(host.sizeX + layout.host_lines - 1, layout.cols)
      << "Host chart " << host << " does not take the full width of its row";
  EXPECT_LE(host.sizeX, layout.cols) << "Host chart " << host << " is wider than the terminal";
  EXPECT_EQ(layout.host_detail.sizeY, LAYOUT_HOST_DETAIL_ROWS) << "The CPU detail block is not a fixed height block";
  EXPECT_EQ(layout.host_detail.posY, 0u) << "The CPU device is not the first section of the screen";
  EXPECT_EQ(host.posY, layout.host_detail.posY + layout.host_detail.sizeY)
      << "Host chart " << host << " is not right below its own detail block";
  for (const auto &chart : layout.charts) {
    EXPECT_LT(host.posY, chart.posY) << "Host chart " << host << " is not above the chart " << chart;
    EXPECT_NE(chart.posY, host.posY) << "Chart " << chart << " shares the row of the host chart " << host;
  }

  // Same row height as every GPU chart row: the chart rows all share the chart
  // space equally, the host row included.
  for (const auto &chart : layout.charts)
    EXPECT_EQ(host.sizeY, chart.sizeY) << "Host chart " << host << " does not have the height of " << chart;

  // A GPU detail block never sits above the CPU chart: it comes after the
  // whole CPU section, the blank separating row included. Inside the terminal
  // and away from the other windows.
  for (size_t dev = 0; dev < layout.headers.size(); ++dev) {
    if (!header_is_allocated(layout, dev))
      continue; // No section for that device: no header to place anywhere.
    EXPECT_LE(host.posY + host.sizeY + LAYOUT_SECTION_GAP_ROWS, layout.headers[dev].posY)
        << "Host chart " << host << " overlaps the header at " << layout.headers[dev];
  }
  EXPECT_TRUE(inside(host, screen)) << "Host chart " << host << " is outside of the terminal";
  EXPECT_TRUE(inside(layout.process, screen)) << "The process list is outside of the terminal";
  std::vector<window_position> others = layout.charts;
  others.push_back(layout.process);
  for (const auto &other : others)
    EXPECT_FALSE(overlap(host, other)) << "Host chart " << host << " overlaps " << other;
  for (size_t first = 0; first < layout.charts.size(); ++first)
    for (size_t second = first + 1; second < layout.charts.size(); ++second)
      EXPECT_FALSE(overlap(layout.charts[first], layout.charts[second]))
          << "Charts " << layout.charts[first] << " and " << layout.charts[second] << " overlap";
}

} // namespace

TEST(HostMetricsCpu, ParseAggregateLine) {
  host_cpu_ticks ticks = {};
  EXPECT_TRUE(host_cpu_parse_stat_text("cpu  100 0 0 100 0 0 0 0 0 0\n", &ticks));
  EXPECT_EQ(ticks.total, 200u);
  EXPECT_EQ(ticks.idle, 100u);

  // idle and iowait both count as idle time.
  EXPECT_TRUE(host_cpu_parse_stat_text("cpu  10 10 10 40 40 0 0 0 0 0\n", &ticks));
  EXPECT_EQ(ticks.total, 110u);
  EXPECT_EQ(ticks.idle, 80u);

  // Shorter historical lines are tolerated.
  EXPECT_TRUE(host_cpu_parse_stat_text("cpu  10 10 10 10\n", &ticks));
  EXPECT_EQ(ticks.total, 40u);
  EXPECT_TRUE(host_cpu_parse_stat_text("cpu  10 0 0 10 0 0 0 0 0 0 7\n", &ticks));
  EXPECT_EQ(ticks.total, 20u);

  // guest time is already included in user: adding it again would double count.
  EXPECT_TRUE(host_cpu_parse_stat_text("cpu  10 0 0 10 0 0 0 0 500 500\n", &ticks));
  EXPECT_EQ(ticks.total, 20u);
}

TEST(HostMetricsCpu, ParseRejectsUnusableInput) {
  host_cpu_ticks ticks = {1, 1};
  EXPECT_FALSE(host_cpu_parse_stat_text(nullptr, &ticks));
  EXPECT_FALSE(host_cpu_parse_stat_text("", &ticks));
  EXPECT_FALSE(host_cpu_parse_stat_text("cpu  1 2 3\n", &ticks));
  EXPECT_FALSE(host_cpu_parse_stat_text("cpu  a b c d\n", &ticks));
  EXPECT_FALSE(host_cpu_parse_stat_text("cpu  1 2 3 4 oops\n", &ticks));
  EXPECT_FALSE(host_cpu_parse_stat_text("cpu  1 2 -3 4\n", &ticks));
  EXPECT_FALSE(host_cpu_parse_stat_text("cpu  340282366920938463463374607431768211456 2 3 4\n", &ticks));
  // Only per-cpu lines: no aggregate line.
  EXPECT_FALSE(host_cpu_parse_stat_text("cpu0 1 1 1 1\ncpu1 1 1 1 1\n", &ticks));
  EXPECT_EQ(ticks.total, 0u);
}

TEST(HostMetricsCpu, DeltaUtilization) {
  host_cpu_ticks previous = {1000, 750};
  host_cpu_ticks current = {1100, 825};
  double percent = -1.;
  EXPECT_TRUE(host_cpu_utilization(&previous, &current, &percent));
  EXPECT_TRUE(nearly(percent, 25.));

  host_cpu_ticks idle_now = {2000, 1750};
  EXPECT_TRUE(host_cpu_utilization(&previous, &idle_now, &percent));
  EXPECT_TRUE(nearly(percent, 0.));
  host_cpu_ticks busy_now = {2000, 750};
  EXPECT_TRUE(host_cpu_utilization(&previous, &busy_now, &percent));
  EXPECT_TRUE(nearly(percent, 100.));

  // No elapsed tick, and decreasing counters, must be reported as unavailable.
  percent = -1.;
  host_cpu_ticks same = previous;
  EXPECT_FALSE(host_cpu_utilization(&previous, &same, &percent));
  EXPECT_TRUE(nearly(percent, -1.));
  host_cpu_ticks backward = {900, 700};
  EXPECT_FALSE(host_cpu_utilization(&previous, &backward, &percent));
  host_cpu_ticks idle_backward = {1100, 700};
  EXPECT_FALSE(host_cpu_utilization(&previous, &idle_backward, &percent));
  host_cpu_ticks inconsistent = {10, 20};
  EXPECT_FALSE(host_cpu_utilization(&previous, &inconsistent, &percent));
  EXPECT_FALSE(host_cpu_utilization(nullptr, &current, &percent));
}

TEST(HostMetricsCpu, RealisticSamples) {
  // Two samples taken from a real multi core /proc/stat aggregate line.
  host_cpu_ticks first = {}, second = {};
  EXPECT_TRUE(host_cpu_parse_stat_text("cpu  50046421 154764 15947860 8482668305 3448388 0 3631613 0 0 0\n"
                                       "intr 219917474 0 0 0\n",
                                       &first));
  EXPECT_TRUE(host_cpu_parse_stat_text("cpu  50046945 154764 15948029 8482681982 3448388 0 3631712 0 0 0\n"
                                       "intr 219917500 0 0 0\n",
                                       &second));
  double percent = -1.;
  EXPECT_TRUE(host_cpu_utilization(&first, &second, &percent));
  // busy: 524 user + 169 system + 99 softirq over 14469 elapsed ticks.
  EXPECT_TRUE(nearly(percent, 100. * 792. / 14469.));
}

TEST(HostMetricsMemory, ParseMemInfo) {
  host_memory_info info = {};
  EXPECT_TRUE(host_memory_parse_meminfo_text("MemTotal:       16384 kB\nMemFree: 1 kB\nMemAvailable:   4096 kB\n",
                                             &info));
  EXPECT_EQ(info.total_kib, 16384u);
  EXPECT_EQ(info.available_kib, 4096u);

  EXPECT_TRUE(host_memory_parse_meminfo_text("MemTotal:\t1024   kB \nMemAvailable:0kB\n", &info));
  EXPECT_EQ(info.total_kib, 1024u);
  EXPECT_EQ(info.available_kib, 0u);

  // SwapTotal must not be mistaken for MemTotal.
  EXPECT_TRUE(host_memory_parse_meminfo_text("MemTotal: 1024 kB\nSwapTotal: 9 kB\nMemAvailable: 512 kB\n", &info));
  EXPECT_EQ(info.available_kib, 512u);
}

TEST(HostMetricsMemory, ParseGuards) {
  host_memory_info info = {1, 1};
  // Missing MemAvailable must not silently fall back to MemFree.
  EXPECT_FALSE(host_memory_parse_meminfo_text("MemTotal: 1024 kB\nMemFree: 1024 kB\n", &info));
  EXPECT_EQ(info.total_kib, 0u);
  EXPECT_FALSE(host_memory_parse_meminfo_text("MemAvailable: 512 kB\n", &info));
  EXPECT_FALSE(host_memory_parse_meminfo_text("", &info));
  EXPECT_FALSE(host_memory_parse_meminfo_text("MemTotal: nope kB\nMemAvailable: 1 kB\n", &info));
  EXPECT_FALSE(host_memory_parse_meminfo_text("MemTotal: 1024 MB\nMemAvailable: 1 kB\n", &info));
  EXPECT_FALSE(host_memory_parse_meminfo_text("MemTotal: 1024\nMemAvailable: 1 kB\n", &info));
  EXPECT_FALSE(host_memory_parse_meminfo_text("MemTotal: 0 kB\nMemAvailable: 0 kB\n", &info));
}

TEST(HostMetricsMemory, UsageBoundaries) {
  host_memory_info info = {16u * 1024u * 1024u, 4u * 1024u * 1024u};
  double used = 0., total = 0., percent = 0.;
  EXPECT_TRUE(host_memory_usage(&info, &used, &total, &percent));
  EXPECT_TRUE(nearly(used, 12.));
  EXPECT_TRUE(nearly(total, 16.));
  EXPECT_TRUE(nearly(percent, 75.));

  // MemAvailable above MemTotal is inconsistent: the memory is unavailable, it
  // is not a suspicious zero usage.
  host_memory_info inconsistent = {1024u, 4096u};
  EXPECT_FALSE(host_memory_usage(&inconsistent, &used, &total, &percent));

  host_memory_info exactly_full = {1024u, 0u};
  EXPECT_TRUE(host_memory_usage(&exactly_full, &used, &total, &percent));
  EXPECT_TRUE(nearly(percent, 100.));

  host_memory_info no_room_left = {1024u, 1u};
  EXPECT_TRUE(host_memory_usage(&no_room_left, &used, &total, &percent));
  EXPECT_TRUE(nearly(used, 1023. / (1024. * 1024.)));

  host_memory_info empty = {0u, 0u};
  EXPECT_FALSE(host_memory_usage(&empty, &used, &total, &percent));
}

// The legends of the two lines of the combined host chart. Same buffer size as
// the chart legends, see PLOT_MAX_LEGEND_SIZE.
TEST(HostMetricsLegend, CurrentValuesAndUnavailable) {
  host_metrics_state state = {};
  ASSERT_TRUE(host_metrics_init(&state, 32u));
  char legend[35] = {};

  // Nothing sampled: N/A, never the idle zero the zeroed state would suggest.
  EXPECT_STREQ(host_metric_name(host_metric_cpu), "CPU");
  EXPECT_STREQ(host_metric_name(host_metric_memory), "RAM");
  EXPECT_EQ(host_metrics_format_legend(&state, host_metric_cpu, false, legend, sizeof(legend)), 7u);
  EXPECT_STREQ(legend, "CPU N/A");
  EXPECT_EQ(host_metrics_format_legend(&state, host_metric_memory, false, legend, sizeof(legend)), 7u);
  EXPECT_STREQ(legend, "RAM N/A");
  EXPECT_EQ(host_metrics_format_legend(&state, host_metric_memory, true, legend, sizeof(legend)), 7u);
  EXPECT_STREQ(legend, "RAM N/A");
  EXPECT_EQ(host_metrics_format_legend(nullptr, host_metric_cpu, false, legend, sizeof(legend)), 7u);
  EXPECT_STREQ(legend, "CPU N/A");

  state.cpu_valid = true;
  state.cpu_percent = 42.5;
  EXPECT_EQ(host_metrics_format_legend(&state, host_metric_cpu, false, legend, sizeof(legend)), 9u);
  EXPECT_STREQ(legend, "CPU 42.5%");

  state.memory_valid = true;
  state.memory_used_gib = 12.;
  state.memory_total_gib = 16.;
  state.memory_percent = 75.;
  unsigned detailed = host_metrics_format_legend(&state, host_metric_memory, false, legend, sizeof(legend));
  EXPECT_STREQ(legend, "RAM 12.00/16.00 GiB 75.0%");
  unsigned short_form = host_metrics_format_legend(&state, host_metric_memory, true, legend, sizeof(legend));
  EXPECT_STREQ(legend, "RAM 75.0%");
  // The short form is the one the narrow charts fall back to.
  EXPECT_LT(short_form, detailed);
  // Like snprintf, the returned length tells the caller that the detailed form
  // does not fit the chart, without writing outside of the buffer.
  char narrow[12] = {};
  EXPECT_EQ(host_metrics_format_legend(&state, host_metric_memory, false, narrow, sizeof(narrow)), detailed);
  EXPECT_EQ(std::strlen(narrow), sizeof(narrow) - 1);
  EXPECT_EQ(host_metrics_format_legend(&state, host_metric_memory, true, narrow, sizeof(narrow)), short_form);
  EXPECT_STREQ(narrow, "RAM 75.0%");

  // A metric that goes back to unavailable must not keep a stale percentage.
  state.memory_valid = false;
  EXPECT_EQ(host_metrics_format_legend(&state, host_metric_memory, true, legend, sizeof(legend)), 7u);
  EXPECT_STREQ(legend, "RAM N/A");

  EXPECT_EQ(host_metrics_format_legend(&state, host_metric_count, false, legend, sizeof(legend)), 8u);
  EXPECT_STREQ(legend, "Host N/A");
  EXPECT_EQ(host_metrics_format_legend(&state, host_metric_cpu, false, nullptr, sizeof(legend)), 0u);
  EXPECT_EQ(host_metrics_format_legend(&state, host_metric_cpu, false, legend, 0u), 0u);
  host_metrics_release(&state);
}

TEST(HostMetricsState, InvalidStatesAreNotZeroLoad) {
  host_metrics_state state = {};
  ASSERT_TRUE(host_metrics_init(&state, 32u));
  // Before any sample, nothing is valid: the UI must not show a zero load.
  EXPECT_FALSE(state.cpu_valid);
  EXPECT_FALSE(state.memory_valid);
  double sample = 1.;
  EXPECT_FALSE(host_metrics_history_get(&state, host_metric_cpu, 0u, &sample));
  EXPECT_TRUE(nearly(sample, 1.));
  host_metrics_release(&state);
}

TEST(HostMetricsState, HistoryKeepsMostRecentSamples) {
  host_metrics_state state = {};
  ASSERT_TRUE(host_metrics_init(&state, 16u));
  for (unsigned i = 1; i <= 20u; ++i) {
    state.history[host_metric_cpu][state.history_next[host_metric_cpu]] = 10. * (double)i;
    state.history_next[host_metric_cpu] = (state.history_next[host_metric_cpu] + 1) % state.history_capacity;
    if (state.history_count[host_metric_cpu] < state.history_capacity)
      ++state.history_count[host_metric_cpu];
  }
  EXPECT_EQ(state.history_count[host_metric_cpu], 16u);
  double value = 0.;
  // The histories are read from the newest sample, like the GPU ring buffers.
  EXPECT_TRUE(host_metrics_history_get(&state, host_metric_cpu, 0u, &value));
  EXPECT_TRUE(nearly(value, 200.));
  EXPECT_TRUE(host_metrics_history_get(&state, host_metric_cpu, 15u, &value));
  EXPECT_TRUE(nearly(value, 50.));
  // The samples that the wrap around dropped are gone, and the other metric
  // was never sampled.
  EXPECT_FALSE(host_metrics_history_get(&state, host_metric_cpu, 16u, &value));
  EXPECT_FALSE(host_metrics_history_get(&state, host_metric_memory, 0u, &value));
  EXPECT_FALSE(host_metrics_history_get(&state, host_metric_count, 0u, &value));
  EXPECT_FALSE(host_metrics_history_get(nullptr, host_metric_cpu, 0u, &value));
  host_metrics_release(&state);
}

TEST(HostChartLayout, NoChartWhenBothMetricsAreDisabled) {
  for (unsigned device_count : {1u, 2u, 4u, 8u}) {
    ComputedLayout without = compute_layout(device_count, 3, 55, 62, 162, 0);
    ASSERT_EQ(without.host.sizeX, 0u);
    ASSERT_EQ(without.host.sizeY, 0u);
    for (unsigned host_lines : {1u, 2u}) {
      ComputedLayout with_host = compute_layout(device_count, 3, 55, 62, 162, host_lines);
      SCOPED_TRACE("devices=" + std::to_string(device_count) + " host lines=" + std::to_string(host_lines));
      if (!host_chart_is_allocated(with_host)) {
        // The chart area cannot give one more section to the CPU device, the
        // way it cannot give one more chart row to a GPU. The whole CPU device
        // disappears, detail block and blank row with it, and the layout is the
        // one without any host metric.
        EXPECT_EQ(with_host.host_detail.sizeY, 0u);
        EXPECT_TRUE(same_position(with_host.process, without.process));
        for (size_t i = 0; i < without.charts.size(); ++i) {
          EXPECT_TRUE(same_position(with_host.charts[i], without.charts[i])) << "Chart " << i << " moved";
          EXPECT_TRUE(same_position(with_host.headers[i], without.headers[i])) << "Header " << i << " moved";
        }
        continue;
      }
      // Enabling a host metric adds the CPU device: its detail block and the
      // combined host chart, and the GPU charts only move down.
      expect_host_chart_follows_the_chart_rules(with_host);
      expect_gpu_charts_keep_their_arrangement(without, with_host);
    }
  }
}

// Whatever the terminal and the number of devices, the CPU device owns the
// first section of the screen: its detail block at the top, its chart right
// below it, a blank row, and only then the GPU sections.
TEST(HostChartLayout, CpuDeviceIsTheFirstSectionAboveEveryGpuChart) {
  const unsigned term_rows[] = {1, 2, 3, 8, 12, 16, 20, 24, 26, 30, 40, 62, 100, 200, 400};
  const unsigned term_cols[] = {1, 10, 29, 30, 40, 60, 80, 120, 162, 240, 600, 1200};
  const unsigned device_counts[] = {0, 1, 2, 4, 8};
  for (unsigned device_count : device_counts) {
    for (unsigned rows : term_rows) {
      for (unsigned cols : term_cols) {
        for (unsigned host_lines : {1u, 2u}) {
          ComputedLayout layout = compute_layout(device_count, 3, 55, rows, cols, host_lines);
          if (!host_chart_is_allocated(layout))
            continue;
          SCOPED_TRACE("devices=" + std::to_string(device_count) + " terminal=" + std::to_string(rows) + "x" +
                       std::to_string(cols) + " host lines=" + std::to_string(host_lines));
          expect_host_chart_follows_the_chart_rules(layout);
          // Nothing is above the CPU detail block, and the first GPU detail
          // block is one blank row below the host chart.
          if (!layout.headers.empty()) {
            EXPECT_GE(first_gpu_detail_pos_y(layout), layout.host.posY + layout.host.sizeY + LAYOUT_SECTION_GAP_ROWS);
            if (!layout.charts.empty())
              EXPECT_GE(first_gpu_row_pos_y(layout), first_gpu_detail_pos_y(layout) + layout.header_rows)
                  << "The first GPU chart does not follow its own detail block";
          }
        }
      }
    }
  }
}

TEST(HostChartLayout, OneOrTwoLinesInTheSingleChart) {
  // One metric enabled gives one line in the chart, both enabled give two
  // lines in the very same chart.
  ComputedLayout one = compute_layout(1, 3, 55, 62, 162, 1);
  ComputedLayout two = compute_layout(1, 3, 55, 62, 162, 2);
  ASSERT_EQ(one.charts.size(), two.charts.size());
  ASSERT_TRUE(host_chart_is_allocated(one));
  ASSERT_TRUE(host_chart_is_allocated(two));
  // One single chart, owning the same chart row in both cases.
  EXPECT_EQ(one.host.posY, two.host.posY);
  EXPECT_EQ(one.host.posX, two.host.posX);
  EXPECT_EQ(one.host.sizeY, two.host.sizeY);
  // The chart owns the full width of its row either way and shares that width
  // out between its lines: the second line gets its own drawing columns.
  EXPECT_EQ((one.host.sizeX - 5u) % 1u, 0u);
  EXPECT_EQ((two.host.sizeX - 5u) % 2u, 0u);
  EXPECT_EQ(one.host.sizeX, one.cols);
  EXPECT_GE(two.host.sizeX + 1u, two.cols);
  const unsigned one_line_columns = one.host.sizeX - 5u;
  const unsigned two_line_columns = (two.host.sizeX - 5u) / 2u;
  EXPECT_GE(one_line_columns, 2u * two_line_columns);
  EXPECT_LT(one_line_columns, 2u * two_line_columns + 2u);
}

TEST(HostChartLayout, GpuChartsKeepTheirOrderAndCount) {
  const unsigned term_rows[] = {16, 24, 30, 40, 62, 100, 200};
  const unsigned term_cols[] = {40, 60, 80, 120, 162, 240, 600, 1200};
  const unsigned device_counts[] = {1, 2, 4, 8, 16};
  for (unsigned device_count : device_counts) {
    for (unsigned rows : term_rows) {
      for (unsigned cols : term_cols) {
        ComputedLayout without = compute_layout(device_count, 3, 55, rows, cols, 0);
        ComputedLayout with = compute_layout(device_count, 3, 55, rows, cols, 2);
        SCOPED_TRACE("devices=" + std::to_string(device_count) + " terminal=" + std::to_string(rows) + "x" +
                     std::to_string(cols));
        // The host chart never shares a chart with a GPU: the visible GPU
        // sections stay the same ones, in the same order, and they only shift
        // down below the CPU device section.
        expect_gpu_charts_keep_their_arrangement(without, with);
        if (with.host.sizeX != 0)
          expect_host_chart_follows_the_chart_rules(with);
      }
    }
  }
}

TEST(HostChartLayout, ChartGeometryIsTheGpuChartGeometry) {
  // Two GPU devices each get a chart row of their own with two lines, exactly
  // like the host chart. Every chart row must then be a copy of the others:
  // same left edge, same width, same height, only the vertical position moves.
  const unsigned area_rows[] = {16, 24, 28, 30, 40, 62, 100};
  const unsigned term_cols[] = {60, 80, 100, 120, 162, 200, 240, 320, 640, 1200};
  unsigned compared = 0;
  for (unsigned area : area_rows) {
    for (unsigned cols : term_cols) {
      ComputedLayout layout = compute_layout(2, 1, 20, area, cols, 2, true);
      if (layout.charts.size() != 2 || !host_chart_is_allocated(layout))
        continue;
      if (layout.charts[0].posY == layout.charts[1].posY)
        continue; // The two GPU charts share a row: no solo chart row to compare.
      SCOPED_TRACE("chart area rows=" + std::to_string(area) + " cols=" + std::to_string(cols));
      ++compared;
      EXPECT_EQ(layout.host.posX, layout.charts[0].posX) << "Host chart " << layout.host;
      EXPECT_EQ(layout.host.sizeX, layout.charts[0].sizeX)
          << "Host chart " << layout.host << " is not as wide as the chart " << layout.charts[0];
      EXPECT_EQ(layout.host.sizeY, layout.charts[0].sizeY)
          << "Host chart " << layout.host << " is not as tall as the chart " << layout.charts[0];
      // Every chart row is separated from the previous section by its own
      // detail block and a blank row.
      EXPECT_GE(layout.charts[0].posY,
                layout.host.posY + layout.host.sizeY + LAYOUT_SECTION_GAP_ROWS + layout.header_rows)
          << "Host chart " << layout.host;
      EXPECT_EQ(layout.charts[0].posX, layout.charts[1].posX);
      EXPECT_EQ(layout.charts[0].sizeX, layout.charts[1].sizeX);
      EXPECT_EQ(layout.charts[0].sizeY, layout.charts[1].sizeY);
      EXPECT_GE(layout.charts[1].posY,
                layout.charts[0].posY + layout.charts[0].sizeY + LAYOUT_SECTION_GAP_ROWS + layout.header_rows);
    }
  }
  EXPECT_GT(compared, 0u) << "No comparable chart row was found";
}

TEST(HostChartLayout, UserScreenshotSizeStacksTheChartsVertically) {
  // The 162x62 terminal of the report: the host chart owns the first chart row
  // and the GPU chart of the device comes right below it, one full chart under
  // the other, never side by side.
  ComputedLayout layout = compute_layout(1, 3, 55, 62, 162, 2);
  ASSERT_EQ(layout.charts.size(), 1u);
  expect_host_chart_follows_the_chart_rules(layout);
  const window_position &gpu = layout.charts[0];
  // Both charts take the whole available chart width and start on the left.
  EXPECT_EQ(layout.host.posX, 0u);
  EXPECT_EQ(gpu.posX, 0u);
  EXPECT_GE(layout.host.sizeX + 1u, layout.cols) << "Host chart " << layout.host << " is not full width";
  EXPECT_GE(gpu.sizeX + 1u, layout.cols) << "GPU chart " << gpu << " is not full width";
  EXPECT_EQ(layout.host.sizeX, gpu.sizeX) << "The two chart rows are not as wide as each other";
  // Both rows take an equal share of the chart space: a full chart, not a
  // shallow band.
  EXPECT_EQ(layout.host.sizeY, gpu.sizeY) << "The two chart rows are not as tall as each other";
  EXPECT_GE(layout.host.sizeY, 7u) << "Host chart " << layout.host << " is below the minimum chart height";
  // The GPU chart comes under the host chart, its own detail block and the
  // blank separating row in between: the sections are stacked, never side by
  // side, and the GPU header only introduces its own chart.
  EXPECT_EQ(layout.host.posY + layout.host.sizeY + LAYOUT_SECTION_GAP_ROWS + layout.header_rows, gpu.posY)
      << "GPU chart " << gpu << " does not follow the host chart";
  EXPECT_EQ(layout.headers[0].posY, layout.host.posY + layout.host.sizeY + LAYOUT_SECTION_GAP_ROWS)
      << "The GPU header is not the blank row below the host chart";
  // Below the charts, the process list is still there.
  EXPECT_GE(layout.process.posY, gpu.posY + gpu.sizeY) << "The process list overlaps the charts";
  EXPECT_GT(layout.process.sizeY, 0u) << "The process list disappeared";
}

// The monitoring screen is a stack of sections, from the top of the terminal to
// the bottom: the CPU device, the GPU devices, the process list and the
// shortcut bar. One blank row separates two sections, never a device detail
// block from its own chart.
TEST(HostChartLayout, SectionsStackFromTopToBottom) {
  ComputedLayout layout = compute_layout(1, 3, 55, 62, 162, 2);
  ASSERT_FALSE(layout.sections.empty());
  const window_position screen = {0, 0, layout.cols, layout.rows - 1};

  // The CPU device comes first, with its detail block above its own chart.
  ASSERT_EQ(layout.sections[0].kind, layout_section_host_device);
  EXPECT_TRUE(same_position(layout.sections[0].detail, layout.host_detail));
  EXPECT_TRUE(same_position(layout.sections[0].chart, layout.host));
  EXPECT_EQ(layout.host_detail.posY, 0u);
  EXPECT_EQ(layout.host_detail.sizeY, LAYOUT_HOST_DETAIL_ROWS);
  EXPECT_EQ(layout.host.posY, layout.host_detail.posY + layout.host_detail.sizeY);
  EXPECT_TRUE(inside(layout.host_detail, screen)) << "The CPU detail block is outside of the terminal";

  // The GPU device follows, one blank row below the CPU section, and its own
  // detail block is directly above the chart that draws it.
  ASSERT_GT(layout.sections.size(), 1u);
  ASSERT_EQ(layout.sections[1].kind, layout_section_gpu_device);
  EXPECT_EQ(layout.sections[1].area.posY,
            layout.sections[0].area.posY + layout.sections[0].area.sizeY + LAYOUT_SECTION_GAP_ROWS);
  EXPECT_EQ(layout.sections[1].chart.posY, layout.sections[1].detail.posY + layout.sections[1].detail.sizeY);
  EXPECT_EQ(layout.sections[1].device_count, 1u);
  EXPECT_EQ(layout.device_to_chart[0], layout.sections[1].plot_first);

  // The process list follows the last section with a blank row between them,
  // and the shortcut bar is the last row of the terminal.
  const layout_section *previous = &layout.sections[1];
  bool found_processes = false;
  for (size_t i = 2; i < layout.sections.size(); ++i) {
    const layout_section &section = layout.sections[i];
    if (section.kind == layout_section_shortcut) {
      // The shortcut bar is anchored to the last row of the terminal. It has no
      // gap row of its own: the rows above it already stop right above it.
      EXPECT_GE(section.area.posY, previous->area.posY + previous->area.sizeY)
          << "The shortcut bar overlaps " << layout_section_kind_name(previous->kind);
      EXPECT_EQ(section.chart.posY + section.chart.sizeY, layout.rows) << "The shortcut bar is not bottom aligned";
      EXPECT_EQ(section.chart.sizeY, 1u);
    } else {
      EXPECT_EQ(section.area.posY, previous->area.posY + previous->area.sizeY + LAYOUT_SECTION_GAP_ROWS)
          << "Section " << i << " (" << layout_section_kind_name(section.kind) << ") is not one row after the previous";
      if (section.kind == layout_section_processes) {
        found_processes = true;
        EXPECT_TRUE(same_position(section.chart, layout.process));
        EXPECT_TRUE(inside(section.chart, screen)) << "The process list is outside of the terminal";
      }
    }
    previous = &section;
  }
  EXPECT_TRUE(found_processes) << "The process list disappeared";
}

// Three GPUs in a terminal tall enough for every section: three GPU sections,
// each holding one device and one chart of its own, device 0 drawn by chart 0,
// device 1 by chart 1 and device 2 by chart 2, every header directly above its
// own chart and nothing else.
TEST(HostChartLayout, ThreeGpusAreThreeIndependentSections) {
  ComputedLayout layout = compute_layout(3, 3, 55, 100, 162, 2);
  ASSERT_EQ(layout.num_plots, 3u);
  ASSERT_EQ(layout.charts.size(), 3u);
  // The CPU device, the three GPU devices, the process list and the shortcuts.
  ASSERT_EQ(layout.sections.size(), 6u);
  EXPECT_EQ(layout.sections[0].kind, layout_section_host_device);
  for (unsigned dev = 0; dev < 3u; ++dev) {
    const layout_section &section = layout.sections[1u + dev];
    SCOPED_TRACE("gpu section " + std::to_string(dev));
    EXPECT_EQ(section.kind, layout_section_gpu_device);
    EXPECT_EQ(section.device_count, 1u);
    EXPECT_EQ(section.device_first, dev);
    EXPECT_EQ(section.plot_count, 1u);
    EXPECT_EQ(section.plot_first, dev);
    EXPECT_EQ(layout.device_to_chart[dev], dev) << "Device " << dev << " does not own its chart";
    EXPECT_EQ(layout.headers[dev].posY, section.detail.posY);
    EXPECT_EQ(section.detail.posY + section.detail.sizeY, section.chart.posY)
        << "The header of device " << dev << " is not directly above its own chart";
    EXPECT_EQ(section.chart.sizeY, layout.host.sizeY) << "Chart " << dev << " is not as tall as the CPU chart";
    EXPECT_EQ(section.chart.sizeX, layout.host.sizeX) << "Chart " << dev << " is not as wide as the CPU chart";
    if (dev > 0u)
      EXPECT_EQ(section.area.posY,
                layout.sections[dev].area.posY + layout.sections[dev].area.sizeY + LAYOUT_SECTION_GAP_ROWS)
          << "Device " << dev << " does not follow the previous device section by one blank row";
  }
  EXPECT_EQ(layout.sections[4u].kind, layout_section_processes);
  EXPECT_EQ(layout.sections[5u].kind, layout_section_shortcut);
}

// More devices, more GPU sections: every visible device has a section of its
// own, with its detail block attached to the chart that draws it alone, and the
// devices keep their order. The devices the terminal cannot show are not shown
// at all: no detail block, no chart, no mapping.
TEST(HostChartLayout, EveryGpuDetailIsAttachedToItsOwnChart) {
  const unsigned device_counts[] = {1, 2, 3, 4, 8};
  const unsigned terminals[][2] = {{30, 162}, {40, 162}, {62, 162}, {62, 90}, {100, 600}};
  for (unsigned device_count : device_counts) {
    for (const auto &terminal : terminals) {
      ComputedLayout layout = compute_layout(device_count, 3, 55, terminal[0], terminal[1], 2);
      SCOPED_TRACE("devices=" + std::to_string(device_count) + " terminal=" + std::to_string(terminal[0]) + "x" +
                   std::to_string(terminal[1]));
      unsigned gpu_sections = 0;
      for (const auto &section : layout.sections) {
        if (section.kind != layout_section_gpu_device)
          continue;
        // One device, one chart, drawn by nobody else, in device index order.
        EXPECT_EQ(section.device_count, 1u);
        EXPECT_EQ(section.plot_count, 1u);
        EXPECT_EQ(section.device_first, gpu_sections);
        EXPECT_EQ(section.plot_first, section.device_first);
        EXPECT_EQ(section.chart.posY, section.detail.posY + section.detail.sizeY);
        const unsigned dev = section.device_first;
        EXPECT_EQ(layout.headers[dev].posY, section.detail.posY);
        EXPECT_EQ(layout.headers[dev].posY + layout.headers[dev].sizeY, section.chart.posY)
            << "Device " << dev << " header is not directly above its own chart";
        EXPECT_EQ(layout.device_to_chart[dev], section.plot_first);
        EXPECT_EQ(layout.charts[layout.device_to_chart[dev]].posY, section.chart.posY)
            << "Device " << dev << " is not drawn by the chart below its header";
        ++gpu_sections;
      }
      // The visible devices are an ordered prefix of the request: one section
      // per visible device, and nothing at all for the others.
      EXPECT_EQ(gpu_sections, layout.num_plots) << "A visible device has no section";
      for (unsigned dev = 0; dev < device_count; ++dev) {
        if (dev < layout.num_plots) {
          EXPECT_TRUE(header_is_allocated(layout, dev)) << "Device " << dev << " has a chart and no header";
          EXPECT_EQ(layout.device_to_chart[dev], dev) << "Device " << dev << " does not own its chart";
          continue;
        }
        EXPECT_EQ(layout.device_to_chart[dev], (unsigned)MAX_CHARTS)
            << "Device " << dev << " is mapped to a chart it has no section for";
        EXPECT_FALSE(header_is_allocated(layout, dev)) << "Device " << dev << " has a header without a chart";
      }
      // No two visible GPUs share a chart.
      for (unsigned first = 0; first < layout.num_plots; ++first)
        for (unsigned second = first + 1; second < layout.num_plots; ++second) {
          EXPECT_NE(layout.device_to_chart[first], layout.device_to_chart[second]);
          EXPECT_NE(layout.charts[first].posY, layout.charts[second].posY)
              << "Charts " << first << " and " << second << " share a row";
        }
    }
  }
}

// Both host metrics off: the CPU device is not a section at all, and its blank
// row is not there either. One line or two, it stays a single CPU device with
// a single chart.
TEST(HostChartLayout, CpuDeviceSectionAppearsAndDisappearsAsOne) {
  ComputedLayout with_host = compute_layout(2, 3, 55, 62, 162, 2);
  ComputedLayout without_host = compute_layout(2, 3, 55, 62, 162, 0);
  unsigned host_sections = 0;
  for (const auto &section : without_host.sections)
    if (section.kind == layout_section_host_device)
      ++host_sections;
  EXPECT_EQ(host_sections, 0u) << "A CPU section survived without any host metric";
  EXPECT_EQ(without_host.host_detail.sizeY, 0u);
  EXPECT_EQ(without_host.host.sizeY, 0u);
  ASSERT_FALSE(without_host.sections.empty());
  EXPECT_EQ(without_host.sections.front().kind, layout_section_gpu_device);
  EXPECT_EQ(without_host.sections.front().detail.posY, 0u);

  host_sections = 0;
  for (unsigned host_lines : {1u, 2u}) {
    ComputedLayout one_or_two = compute_layout(2, 3, 55, 62, 162, host_lines);
    unsigned lines_host_sections = 0;
    for (const auto &section : one_or_two.sections)
      if (section.kind == layout_section_host_device)
        ++lines_host_sections;
    EXPECT_EQ(lines_host_sections, 1u) << host_lines << " host lines is not one CPU device";
    EXPECT_EQ(one_or_two.host_detail.sizeY, LAYOUT_HOST_DETAIL_ROWS);
    host_sections += (lines_host_sections == 1u);
  }
  EXPECT_EQ(host_sections, 2u);
}

// Resizing recomputes the positions from the terminal size alone: the same
// terminal always gives the same layout back, so the histories the charts hold
// are never thrown away by a resize.
TEST(HostChartLayout, ResizeRecomputesTheSameLayout) {
  ComputedLayout small_one = compute_layout(2, 3, 55, 24, 60, 2);
  ComputedLayout big = compute_layout(2, 3, 55, 62, 162, 2);
  ComputedLayout small_again = compute_layout(2, 3, 55, 24, 60, 2);
  EXPECT_TRUE(same_position(small_one.process, small_again.process));
  ASSERT_EQ(small_one.charts.size(), small_again.charts.size());
  for (size_t i = 0; i < small_one.charts.size(); ++i) {
    EXPECT_TRUE(same_position(small_one.charts[i], small_again.charts[i])) << "Chart " << i << " moved on resize";
    EXPECT_TRUE(same_position(small_one.headers[i], small_again.headers[i])) << "Header " << i << " moved on resize";
  }
  EXPECT_GE(big.charts.front().sizeY, small_one.charts.front().sizeY);
  EXPECT_GT(big.process.posY, small_one.process.posY);
}

// Without the host chart, the layout is the upstream one: the caller can leave
// the host chart arguments out, or ask for no host line at all.
TEST(HostChartLayout, NoHostLayoutIsTheUpstreamLayout) {
  const unsigned term_rows[] = {1, 2, 3, 8, 16, 24, 30, 40, 62, 100, 200, 400};
  const unsigned term_cols[] = {1, 10, 29, 30, 40, 60, 80, 120, 162, 240, 600, 1200};
  const unsigned device_counts[] = {0, 1, 2, 4, 8, 16};
  for (unsigned device_count : device_counts) {
    for (unsigned rows : term_rows) {
      for (unsigned cols : term_cols) {
        ComputedLayout upstream = compute_layout(device_count, 3, 55, rows, cols, 0, false, false);
        ComputedLayout disabled = compute_layout(device_count, 3, 55, rows, cols, 0, false, true);
        SCOPED_TRACE("devices=" + std::to_string(device_count) + " terminal=" + std::to_string(rows) + "x" +
                     std::to_string(cols));
        ASSERT_EQ(upstream.charts.size(), disabled.charts.size());
        EXPECT_TRUE(same_position(upstream.process, disabled.process));
        EXPECT_TRUE(same_position(upstream.setup, disabled.setup));
        for (size_t i = 0; i < upstream.headers.size(); ++i)
          EXPECT_TRUE(same_position(upstream.headers[i], disabled.headers[i]))
              << "Header " << i << " moved: " << upstream.headers[i] << " vs " << disabled.headers[i];
        for (size_t i = 0; i < upstream.charts.size(); ++i)
          EXPECT_TRUE(same_position(upstream.charts[i], disabled.charts[i]))
              << "Chart " << i << " moved: " << upstream.charts[i] << " vs " << disabled.charts[i];
        EXPECT_EQ(disabled.host.sizeX, 0u);
        EXPECT_EQ(disabled.host.sizeY, 0u);
      }
    }
  }
}

TEST(HostChartLayout, NarrowTerminalsKeepEveryChartInsideTheTerminal) {
  const unsigned term_rows[] = {10, 14, 18, 24, 32};
  const unsigned term_cols[] = {25, 30, 40, 50, 60, 70};
  for (unsigned rows : term_rows) {
    for (unsigned cols : term_cols) {
      ComputedLayout layout = compute_layout(2, 3, 55, rows, cols, 2);
      const window_position screen = {0, 0, cols, rows - 1};
      std::vector<window_position> all = layout.charts;
      if (layout.host.sizeX != 0)
        all.push_back(layout.host);
      all.push_back(layout.process);
      for (const auto &win : all)
        EXPECT_TRUE(inside(win, screen)) << "Window outside of a " << rows << "x" << cols << " terminal: " << win;
      for (size_t first = 0; first < all.size(); ++first)
        for (size_t second = first + 1; second < all.size(); ++second)
          EXPECT_FALSE(overlap(all[first], all[second]))
              << "Overlap in a " << rows << "x" << cols << " terminal between " << all[first] << " and "
              << all[second];
    }
  }
}

// A deterministic pseudo random sweep over the terminal sizes, the device
// counts, the header sizes, the number of host lines and the hidden process
// list. Every host chart the layout does allocate owns a full row of its own,
// above every GPU chart and inside of the terminal.
TEST(HostChartLayout, SweepOfTerminalsNeverSharesTheHostChartRow) {
  unsigned seed = 12345u;
  auto next = [&seed]() {
    seed = seed * 1103515245u + 12345u;
    return seed >> 16;
  };
  unsigned allocated = 0, dropped = 0;
  for (unsigned iteration = 0; iteration < 4000u; ++iteration) {
    const unsigned device_count = next() % 13u;
    const unsigned rows = 1u + next() % 250u;
    const unsigned cols = 1u + next() % 900u;
    const unsigned host_lines = 1u + next() % 2u;
    const bool hide_processes = (next() % 2u) != 0u;
    const unsigned header_rows = (iteration % 2u) ? 3u : 4u;
    const unsigned header_cols = 20u + next() % 90u;
    ComputedLayout layout = compute_layout(device_count, header_rows, header_cols, rows, cols, host_lines,
                                          hide_processes);
    SCOPED_TRACE("devices=" + std::to_string(device_count) + " terminal=" + std::to_string(rows) + "x" +
                 std::to_string(cols) + " host lines=" + std::to_string(host_lines) +
                 " hidden processes=" + std::to_string(hide_processes) +
                 " header cols=" + std::to_string(header_cols));
    if (!host_chart_is_allocated(layout)) {
      ++dropped;
      continue;
    }
    ++allocated;
    expect_host_chart_follows_the_chart_rules(layout);
    if (!layout.charts.empty() && !layout.headers.empty())
      EXPECT_EQ(first_gpu_detail_pos_y(layout), layout.host.posY + layout.host.sizeY + LAYOUT_SECTION_GAP_ROWS)
          << "The first GPU section does not follow the CPU device section";
  }
  // The sweep must actually have drawn something to say anything at all, and
  // must also have met the sizes where the host chart is dropped.
  EXPECT_GT(allocated, 100u);
  EXPECT_GT(dropped, 0u);
}

TEST(HostChartConfig, DefaultsArePlatformAware) {
  nvtop_interface_option options = {};
  interface_options_set_host_usage_defaults(&options);
#ifdef __linux__
  EXPECT_TRUE(options.show_host_cpu_usage);
  EXPECT_TRUE(options.show_host_mem_usage);
#else
  EXPECT_FALSE(options.show_host_cpu_usage);
  EXPECT_FALSE(options.show_host_mem_usage);
#endif
}

TEST(HostChartConfig, ChartOptionsSurviveSaveAndReload) {
  static const char config_path[] = "/tmp/nvtop-host-chart-test.ini";
  struct gpu_info device = {};
  std::snprintf(device.pdev, sizeof(device.pdev), "pci:0000:01:0");

  nvtop_interface_gpu_opts gpu_opts = {};
  gpu_opts.linkedGpu = &device;
  gpu_opts.to_draw = plot_default_draw_info();

  nvtop_interface_option options = {};
  options.gpu_specific_opts = &gpu_opts;
  options.config_file_location = strdup(config_path);
  ASSERT_NE(options.config_file_location, nullptr);
  options.update_interval = 1000;
  options.encode_decode_hiding_timer = 30.;
  options.sort_processes_by = process_memory;
  options.process_fields_displayed = process_default_displayed_field();
  options.use_color = true;
  interface_options_set_host_usage_defaults(&options);
  options.show_host_cpu_usage = false;
  options.show_host_mem_usage = true;

  ASSERT_TRUE(save_interface_options_to_config_file(1, &options));

  // Reload from the file the same way the startup path does.
  nvtop_interface_option reloaded = {};
  reloaded.gpu_specific_opts = &gpu_opts;
  reloaded.config_file_location = strdup(config_path);
  reloaded.process_fields_displayed = 0;
  interface_options_set_host_usage_defaults(&reloaded);
  ASSERT_TRUE(load_interface_options_from_config_file(1, &reloaded));
  EXPECT_FALSE(reloaded.show_host_cpu_usage);
  EXPECT_TRUE(reloaded.show_host_mem_usage);
  std::free(reloaded.config_file_location);

  std::remove(config_path);
  std::free(options.config_file_location);
}

TEST(HostChartConfig, ConfigWithoutTheKeysKeepsDefaults) {
  static const char config_path[] = "/tmp/nvtop-host-chart-legacy.ini";
  FILE *legacy = std::fopen(config_path, "w");
  ASSERT_NE(legacy, nullptr);
  std::fprintf(legacy,
               "[GeneralOption]\n"
               "UseColor = true\n"
               "UpdateInterval = 1500\n"
               "ShowInfoMessages = false\n"
               "\n[HeaderOption]\n"
               "UseFahrenheit = false\n"
               "EncodeHideTimer = 3.000000e+01\n"
               "GPUInfoBar = false\n"
               "\n[Device]\n"
               "Pdev = pci:0000:01:0\n"
               "Monitor = true\n"
               "ShownInfo = gpuRate\n"
               "ShownInfo = gpuMemRate\n");
  std::fclose(legacy);

  struct gpu_info device = {};
  std::snprintf(device.pdev, sizeof(device.pdev), "pci:0000:01:0");
  nvtop_interface_gpu_opts gpu_opts = {};
  gpu_opts.linkedGpu = &device;
  gpu_opts.to_draw = plot_default_draw_info();

  nvtop_interface_option options = {};
  options.gpu_specific_opts = &gpu_opts;
  options.config_file_location = strdup(config_path);
  options.update_interval = 1000;
  options.process_fields_displayed = process_default_displayed_field();
  options.sort_processes_by = process_memory;
  interface_options_set_host_usage_defaults(&options);
  ASSERT_TRUE(load_interface_options_from_config_file(1, &options));
  // Unknown keys leave the platform defaults alone.
#ifdef __linux__
  EXPECT_TRUE(options.show_host_cpu_usage);
  EXPECT_TRUE(options.show_host_mem_usage);
#else
  EXPECT_FALSE(options.show_host_cpu_usage);
  EXPECT_FALSE(options.show_host_mem_usage);
#endif
  EXPECT_EQ(options.update_interval, 1500);
  std::free(options.config_file_location);
  std::remove(config_path);
}
