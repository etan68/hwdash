/*
 *
 * Copyright (C) 2026 Nvtop contributors
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

#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

extern "C" {
#include "nvtop/host_metrics.h"
#include "nvtop/interface_layout_selection.h"
#include "nvtop/interface_options.h"
}

namespace {

host_cpu_stat make_cpu_stat(unsigned long long user, unsigned long long nice, unsigned long long system,
                            unsigned long long idle, unsigned long long iowait = 0, unsigned long long irq = 0,
                            unsigned long long softirq = 0, unsigned long long steal = 0, unsigned long long guest = 0,
                            unsigned long long guest_nice = 0) {
  host_cpu_stat stat;
  memset(&stat, 0, sizeof(stat));
  stat.user = user;
  stat.nice = nice;
  stat.system = system;
  stat.idle = idle;
  stat.iowait = iowait;
  stat.irq = irq;
  stat.softirq = softirq;
  stat.steal = steal;
  stat.guest = guest;
  stat.guest_nice = guest_nice;
  return stat;
}

std::string temp_config_path() {
  const char *tmp = getenv("TMPDIR");
  std::string dir = tmp ? tmp : "/tmp";
  return dir + "/nvtop_host_metrics_test_" + std::to_string(getpid()) + ".ini";
}

} // namespace

// ---------------------------------------------------------------------------
// /proc/stat aggregate line parsing
// ---------------------------------------------------------------------------

TEST(HostMetricsCpu, ParseCpuStatLineFull) {
  host_cpu_stat stat;
  ASSERT_TRUE(host_metrics_parse_cpu_stat_line("cpu  100 50 25 2000 30 10 5 2 8 4\n", &stat));
  EXPECT_EQ(stat.user, 100u);
  EXPECT_EQ(stat.nice, 50u);
  EXPECT_EQ(stat.system, 25u);
  EXPECT_EQ(stat.idle, 2000u);
  EXPECT_EQ(stat.iowait, 30u);
  EXPECT_EQ(stat.irq, 10u);
  EXPECT_EQ(stat.softirq, 5u);
  EXPECT_EQ(stat.steal, 2u);
  EXPECT_EQ(stat.guest, 8u);
  EXPECT_EQ(stat.guest_nice, 4u);
}

TEST(HostMetricsCpu, ParseCpuStatLineLegacyKernel) {
  // Kernels without iowait/irq/softirq/steal/guest fields
  host_cpu_stat stat;
  ASSERT_TRUE(host_metrics_parse_cpu_stat_line("cpu  100 50 25 2000\n", &stat));
  EXPECT_EQ(stat.user, 100u);
  EXPECT_EQ(stat.nice, 50u);
  EXPECT_EQ(stat.system, 25u);
  EXPECT_EQ(stat.idle, 2000u);
  EXPECT_EQ(stat.iowait, 0u);
  EXPECT_EQ(stat.guest, 0u);
}

TEST(HostMetricsCpu, ParseCpuStatLineTrailingWhitespace) {
  host_cpu_stat stat;
  ASSERT_TRUE(host_metrics_parse_cpu_stat_line("cpu  100 50 25 2000  \n", &stat));
  EXPECT_EQ(stat.idle, 2000u);
  ASSERT_TRUE(host_metrics_parse_cpu_stat_line("cpu  100 50 25 2000\t5", &stat));
  EXPECT_EQ(stat.iowait, 5u);
  EXPECT_EQ(stat.guest, 0u);
}

TEST(HostMetricsCpu, ParseCpuStatLineRejectsPerCoreLines) {
  host_cpu_stat stat;
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line("cpu0 100 50 25 2000\n", &stat));
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line("cpu12 100 50 25 2000\n", &stat));
}

TEST(HostMetricsCpu, ParseCpuStatLineRejectsMalformed) {
  host_cpu_stat stat;
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line("", &stat));
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line("cpu\n", &stat));
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line("cpu 100\n", &stat));
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line("cpu 100 50 25\n", &stat));
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line("cpu 100 50 25 20ab\n", &stat));
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line("cpu -1 50 25 2000\n", &stat));
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line("cpu 100 50 x 2000\n", &stat));
  // A present optional field must be a valid number, not silently treated
  // as zero
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line("cpu 10 20 30 40 bad 60\n", &stat));
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line("cpu 10 20 30 40 5x 6 7 8 9 10\n", &stat));
  // An overflowing optional field is rejected
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line("cpu 10 20 30 40 18446744073709551616\n", &stat));
  // Nothing may follow the optional fields
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line("cpu 10 20 30 40 5 6 7 8 9 10 extra\n", &stat));
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line("cpu 10 20 30 40 5 6 7 8 9 10 11\n", &stat));
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line(NULL, &stat));
  EXPECT_FALSE(host_metrics_parse_cpu_stat_line("cpu 100 50 25 2000", NULL));
}

// ---------------------------------------------------------------------------
// CPU utilization deltas
// ---------------------------------------------------------------------------

TEST(HostMetricsCpu, DeltaBasic) {
  host_cpu_stat prev = make_cpu_stat(0, 0, 0, 0);
  host_cpu_stat curr = make_cpu_stat(100, 0, 0, 900);
  double percent = -1.;
  ASSERT_TRUE(host_metrics_cpu_utilization_percent(&prev, &curr, &percent));
  EXPECT_NEAR(percent, 10.0, 1e-9);
}

TEST(HostMetricsCpu, DeltaIdleAndIowaitCountAsIdle) {
  host_cpu_stat prev = make_cpu_stat(0, 0, 0, 0);
  host_cpu_stat curr = make_cpu_stat(0, 0, 0, 500, 500);
  double percent = -1.;
  ASSERT_TRUE(host_metrics_cpu_utilization_percent(&prev, &curr, &percent));
  EXPECT_NEAR(percent, 0.0, 1e-9);
}

TEST(HostMetricsCpu, DeltaAllBusy) {
  host_cpu_stat prev = make_cpu_stat(0, 0, 0, 0);
  host_cpu_stat curr = make_cpu_stat(10, 20, 30, 0, 0, 40, 50, 60);
  double percent = -1.;
  ASSERT_TRUE(host_metrics_cpu_utilization_percent(&prev, &curr, &percent));
  EXPECT_NEAR(percent, 100.0, 1e-9);
}

TEST(HostMetricsCpu, DeltaZeroElapsedTicks) {
  host_cpu_stat prev = make_cpu_stat(100, 50, 25, 2000);
  host_cpu_stat curr = prev;
  double percent = -1.;
  EXPECT_FALSE(host_metrics_cpu_utilization_percent(&prev, &curr, &percent));
}

TEST(HostMetricsCpu, DeltaRejectsDecreasingCounters) {
  host_cpu_stat prev = make_cpu_stat(1000, 500, 250, 20000, 30, 10, 5, 2, 8, 4);
  host_cpu_stat curr = prev;
  double percent = -1.;
  curr.user = 999;
  EXPECT_FALSE(host_metrics_cpu_utilization_percent(&prev, &curr, &percent));
  curr = prev;
  curr.idle = 19999;
  EXPECT_FALSE(host_metrics_cpu_utilization_percent(&prev, &curr, &percent));
  curr = prev;
  curr.guest = 7;
  EXPECT_FALSE(host_metrics_cpu_utilization_percent(&prev, &curr, &percent));
}

TEST(HostMetricsCpu, DeltaGuestTimeCountsAsBusy) {
  // guest time is virtual CPU time already accounted in user; it is busy
  // host CPU time and must not be subtracted (50% of 200 ticks, not 33.33%)
  host_cpu_stat prev = make_cpu_stat(0, 0, 0, 0);
  host_cpu_stat curr = make_cpu_stat(100, 0, 0, 100, 0, 0, 0, 0, 50);
  double percent = -1.;
  ASSERT_TRUE(host_metrics_cpu_utilization_percent(&prev, &curr, &percent));
  EXPECT_NEAR(percent, 50.0, 1e-9);
}

TEST(HostMetricsCpu, DeltaGuestTimeCountsAsBusyInNice) {
  // guest in user, guest_nice in nice: busy = 100 + 10, idle = 800
  host_cpu_stat prev = make_cpu_stat(0, 0, 0, 0);
  host_cpu_stat curr = make_cpu_stat(100, 10, 20, 800, 0, 0, 0, 0, 60, 10);
  double percent = -1.;
  ASSERT_TRUE(host_metrics_cpu_utilization_percent(&prev, &curr, &percent));
  EXPECT_NEAR(percent, 100.0 * 130.0 / 930.0, 1e-9);
}

TEST(HostMetricsCpu, DeltaAccumulatesWithoutWraparound) {
  // Deltas near 2^64 must not wrap around into a small (plausible) result
  host_cpu_stat prev = make_cpu_stat(0, 0, 0, 0, 0, 0, 0, 0);
  host_cpu_stat curr = prev;
  curr.user = 0xFFFFFFFFFFFFFFFFULL;
  curr.nice = 0xFFFFFFFFFFFFFFFFULL;
  curr.system = 0xFFFFFFFFFFFFFFFFULL;
  curr.idle = 0xFFFFFFFFFFFFFFFFULL;
  double percent = -1.;
  EXPECT_FALSE(host_metrics_cpu_utilization_percent(&prev, &curr, &percent));
  // A large but representable total is fine: busy = 2^63, idle = 2^63 - 1
  prev = make_cpu_stat(0, 0, 0, 0);
  curr = make_cpu_stat(0x8000000000000000ULL, 0, 0, 0x7FFFFFFFFFFFFFFFULL);
  ASSERT_TRUE(host_metrics_cpu_utilization_percent(&prev, &curr, &percent));
  EXPECT_NEAR(percent, 50.0, 1e-9);
}

TEST(HostMetricsCpu, DeltaRejectsNullPointers) {
  host_cpu_stat stat = make_cpu_stat(1, 0, 0, 1);
  double percent = -1.;
  EXPECT_FALSE(host_metrics_cpu_utilization_percent(NULL, &stat, &percent));
  EXPECT_FALSE(host_metrics_cpu_utilization_percent(&stat, NULL, &percent));
  EXPECT_FALSE(host_metrics_cpu_utilization_percent(&stat, &stat, NULL));
}

TEST(HostMetricsCpu, SamplerFirstSampleIsUnavailable) {
  host_cpu_sampler sampler;
  memset(&sampler, 0, sizeof(sampler));
  double percent = -1.;
  host_cpu_stat first = make_cpu_stat(10, 0, 5, 900);
  host_cpu_stat second = make_cpu_stat(20, 0, 5, 1000);
  EXPECT_FALSE(host_metrics_cpu_sampler_update(&sampler, &first, &percent));
  // Second sample produces a real delta: busy=10, idle=100 -> 10/110
  percent = -1.;
  ASSERT_TRUE(host_metrics_cpu_sampler_update(&sampler, &second, &percent));
  EXPECT_NEAR(percent, 100.0 * 10.0 / 110.0, 1e-9);
}

TEST(HostMetricsCpu, SamplerReprimesOnCounterReset) {
  // Baseline 1000/1000, counters reset to 10/10, then 20/20: the sampler
  // must re-prime on the reset and report 50% on the next sample
  host_cpu_sampler sampler;
  memset(&sampler, 0, sizeof(sampler));
  double percent = -1.;
  host_cpu_stat baseline = make_cpu_stat(1000, 0, 0, 1000);
  host_cpu_stat reset_sample = make_cpu_stat(10, 0, 0, 10);
  host_cpu_stat next_sample = make_cpu_stat(20, 0, 0, 20);
  EXPECT_FALSE(host_metrics_cpu_sampler_update(&sampler, &baseline, &percent));
  EXPECT_FALSE(host_metrics_cpu_sampler_update(&sampler, &reset_sample, &percent));
  percent = -1.;
  ASSERT_TRUE(host_metrics_cpu_sampler_update(&sampler, &next_sample, &percent));
  EXPECT_NEAR(percent, 50.0, 1e-9);
}

TEST(HostMetricsCpu, SamplerReprimesOnBadSample) {
  host_cpu_sampler sampler;
  memset(&sampler, 0, sizeof(sampler));
  double percent = -1.;
  host_cpu_stat priming = make_cpu_stat(100, 0, 0, 900);
  host_cpu_stat bad = make_cpu_stat(90, 0, 0, 1000);
  host_cpu_stat later = make_cpu_stat(200, 0, 0, 1900);
  EXPECT_FALSE(host_metrics_cpu_sampler_update(&sampler, &priming, &percent));
  // A sample with decreasing counters must be rejected; the baseline is
  // re-primed to it so the sampler does not stay stuck on the old one
  EXPECT_FALSE(host_metrics_cpu_sampler_update(&sampler, &bad, &percent));
  // The next sample is a delta from the bad one: busy=110, idle=900
  percent = -1.;
  ASSERT_TRUE(host_metrics_cpu_sampler_update(&sampler, &later, &percent));
  EXPECT_NEAR(percent, 100.0 * 110.0 / 1010.0, 1e-9);
}

// ---------------------------------------------------------------------------
// /proc/meminfo parsing and RAM usage
// ---------------------------------------------------------------------------

TEST(HostMetricsRam, ParseMeminfoValue) {
  unsigned long long value = 0;
  ASSERT_TRUE(host_metrics_parse_meminfo_value("MemTotal:       16384000 kB\n", "MemTotal", &value));
  EXPECT_EQ(value, 16384000u);
  ASSERT_TRUE(host_metrics_parse_meminfo_value("MemAvailable: 8192000 kB", "MemAvailable", &value));
  EXPECT_EQ(value, 8192000u);
  // Values must be expressed in kB (as in /proc/meminfo); any other unit or
  // no unit at all is rejected
  EXPECT_FALSE(host_metrics_parse_meminfo_value("MemTotal:12345", "MemTotal", &value));
  EXPECT_FALSE(host_metrics_parse_meminfo_value("MemTotal: 12345 KB", "MemTotal", &value));
  EXPECT_FALSE(host_metrics_parse_meminfo_value("MemTotal: 12345 MB", "MemTotal", &value));
  EXPECT_FALSE(host_metrics_parse_meminfo_value("MemTotal: 12345 kBx", "MemTotal", &value));
  // Wrong name, missing value or invalid values are rejected
  EXPECT_FALSE(host_metrics_parse_meminfo_value("MemFree:       4096 kB", "MemTotal", &value));
  EXPECT_FALSE(host_metrics_parse_meminfo_value("MemTotal: kB", "MemTotal", &value));
  EXPECT_FALSE(host_metrics_parse_meminfo_value("MemTotal: -5 kB", "MemTotal", &value));
  EXPECT_FALSE(host_metrics_parse_meminfo_value("MemTotal: 12abc kB", "MemTotal", &value));
  EXPECT_FALSE(host_metrics_parse_meminfo_value("MemTotal: 99999999999999999999999 kB", "MemTotal", &value));
  EXPECT_FALSE(host_metrics_parse_meminfo_value("", "MemTotal", &value));
  EXPECT_FALSE(host_metrics_parse_meminfo_value("MemTotal: 12345 kB", NULL, &value));
}

TEST(HostMetricsRam, ParseMeminfoFullContent) {
  const char *content =
      "MemTotal:       33554432 kB\n"
      "MemFree:         4194304 kB\n"
      "MemAvailable:   16777216 kB\n"
      "Buffers:          524288 kB\n"
      "Cached:          8388608 kB\n";
  host_meminfo info;
  ASSERT_TRUE(host_metrics_parse_meminfo(content, &info));
  EXPECT_TRUE(info.has_total);
  EXPECT_EQ(info.total_kb, 33554432u);
  EXPECT_TRUE(info.has_available);
  EXPECT_EQ(info.available_kb, 16777216u);
}

TEST(HostMetricsRam, ParseMeminfoMissingFields) {
  const char *content = "MemFree: 4096000 kB\n";
  host_meminfo info;
  ASSERT_TRUE(host_metrics_parse_meminfo(content, &info));
  EXPECT_FALSE(info.has_total);
  EXPECT_FALSE(info.has_available);

  host_meminfo empty;
  ASSERT_TRUE(host_metrics_parse_meminfo("", &empty));
  EXPECT_FALSE(empty.has_total);
  EXPECT_FALSE(empty.has_available);
  EXPECT_FALSE(host_metrics_parse_meminfo(NULL, &info));
  EXPECT_FALSE(host_metrics_parse_meminfo(content, NULL));
}

TEST(HostMetricsRam, RamUsageBasic) {
  host_meminfo info = {true, true, 16ULL * 1024 * 1024, 8ULL * 1024 * 1024}; // kB
  double used_gib = -1., total_gib = -1., percent = -1.;
  ASSERT_TRUE(host_metrics_ram_usage(&info, &used_gib, &total_gib, &percent));
  EXPECT_NEAR(total_gib, 16.0, 1e-9);
  EXPECT_NEAR(used_gib, 8.0, 1e-9);
  EXPECT_NEAR(percent, 50.0, 1e-9);
}

TEST(HostMetricsRam, RamUsageGuarded) {
  double used_gib, total_gib, percent;
  // Missing MemAvailable
  host_meminfo no_available = {true, false, 1024 * 1024, 0};
  EXPECT_FALSE(host_metrics_ram_usage(&no_available, &used_gib, &total_gib, &percent));
  // Missing MemTotal
  host_meminfo no_total = {false, true, 0, 1024 * 1024};
  EXPECT_FALSE(host_metrics_ram_usage(&no_total, &used_gib, &total_gib, &percent));
  // Zero MemTotal
  host_meminfo zero_total = {true, true, 0, 0};
  EXPECT_FALSE(host_metrics_ram_usage(&zero_total, &used_gib, &total_gib, &percent));
  // Underflow: available > total
  host_meminfo underflow = {true, true, 1024 * 1024, 2ULL * 1024 * 1024};
  EXPECT_FALSE(host_metrics_ram_usage(&underflow, &used_gib, &total_gib, &percent));
  // Null pointers
  EXPECT_FALSE(host_metrics_ram_usage(NULL, &used_gib, &total_gib, &percent));
  host_meminfo valid = {true, true, 1024 * 1024, 512 * 1024};
  EXPECT_FALSE(host_metrics_ram_usage(&valid, NULL, &total_gib, &percent));
}

// ---------------------------------------------------------------------------
// File readers (no GPU, no network: plain temp files)
// ---------------------------------------------------------------------------

TEST(HostMetricsCpu, ReadCpuStatFromFile) {
  std::string path = temp_config_path() + ".stat";
  FILE *file = fopen(path.c_str(), "w");
  ASSERT_NE(file, nullptr);
  fputs("cpu  100 50 25 2000 30 10 5 2 8 4\n", file);
  fputs("cpu0 1 2 3 4\n", file);
  fclose(file);
  host_cpu_stat stat;
  ASSERT_TRUE(host_metrics_read_cpu_stat(path.c_str(), &stat));
  EXPECT_EQ(stat.user, 100u);
  EXPECT_EQ(stat.guest_nice, 4u);
  EXPECT_FALSE(host_metrics_read_cpu_stat("/nonexistent/proc/stat", &stat));
  EXPECT_FALSE(host_metrics_read_cpu_stat(NULL, &stat));
  remove(path.c_str());
}

TEST(HostMetricsCpu, ReadCpuStatRejectsMalformedFile) {
  std::string path = temp_config_path() + ".stat_bad";
  FILE *file = fopen(path.c_str(), "w");
  ASSERT_NE(file, nullptr);
  fputs("cpu 10 20 30 40 bad 60\n", file);
  fputs("cpu0 1 2 3 4\n", file);
  fclose(file);
  host_cpu_stat stat;
  EXPECT_FALSE(host_metrics_read_cpu_stat(path.c_str(), &stat));
  remove(path.c_str());
}

TEST(HostMetricsRam, ReadMeminfoFromFile) {
  std::string path = temp_config_path() + ".meminfo";
  FILE *file = fopen(path.c_str(), "w");
  ASSERT_NE(file, nullptr);
  fputs("MemTotal:       33554432 kB\n", file);
  fputs("MemAvailable:   16777216 kB\n", file);
  fclose(file);
  host_meminfo info;
  ASSERT_TRUE(host_metrics_read_meminfo(path.c_str(), &info));
  EXPECT_TRUE(info.has_total);
  EXPECT_TRUE(info.has_available);
  double used_gib, total_gib, percent;
  ASSERT_TRUE(host_metrics_ram_usage(&info, &used_gib, &total_gib, &percent));
  EXPECT_NEAR(total_gib, 32.0, 1e-9);
  EXPECT_FALSE(host_metrics_read_meminfo("/nonexistent/proc/meminfo", &info));
  EXPECT_FALSE(host_metrics_read_meminfo(NULL, &info));
  remove(path.c_str());
}

// ---------------------------------------------------------------------------
// Host panel layout: bounds, overlap and underflow regression coverage
// ---------------------------------------------------------------------------

namespace {

struct host_layout_result {
  unsigned num_plots = 0;
  std::vector<struct window_position> dev_positions;
  std::vector<struct window_position> plot_positions;
  struct window_position process_position{};
  struct window_position setup_position{};
  struct window_position host_position{};
};

host_layout_result compute_host_layout(unsigned device_count, unsigned rows, unsigned cols, unsigned host_panel_rows) {
  host_layout_result result;
  nvtop_interface_gpu_opts to_draw_default = {.to_draw = plot_default_draw_info()};
  std::vector<nvtop_interface_gpu_opts> plot_display(device_count, to_draw_default);
  process_field_displayed proc_display = process_default_displayed_field();

  result.dev_positions.resize(device_count);
  result.plot_positions.resize(MAX_CHARTS);
  std::vector<unsigned> map_dev_to_plot(device_count);
  compute_sizes_from_layout(device_count, 3, 55, rows, cols, plot_display.data(), proc_display,
                            result.dev_positions.data(), &result.num_plots, result.plot_positions.data(),
                            map_dev_to_plot.data(), &result.process_position, &result.setup_position, false,
                            host_panel_rows, &result.host_position);
  result.plot_positions.resize(result.num_plots);
  return result;
}

// True when two windows share at least one cell
bool host_positions_overlap(const struct window_position &a, const struct window_position &b) {
  return a.posX < b.posX + b.sizeX && b.posX < a.posX + a.sizeX && a.posY < b.posY + b.sizeY &&
         b.posY < a.posY + a.sizeY;
}

// The host band must stay inside the terminal and must not overlap the
// device headers, the plots or the process list
void check_host_band_consistent(const host_layout_result &layout, unsigned rows, unsigned cols) {
  const struct window_position &host = layout.host_position;
  if (host.sizeX == 0 || host.sizeY == 0)
    return; // Band dropped: nothing to place
  EXPECT_EQ(host.posX, 0u);
  EXPECT_EQ(host.sizeX, cols) << "Host band should span the terminal width";
  EXPECT_LE(host.posY + host.sizeY, rows) << "Host band extends past the bottom of the terminal";
  for (const auto &dev : layout.dev_positions) {
    EXPECT_FALSE(host_positions_overlap(host, dev)) << "Host band overlaps a device header";
    EXPECT_GE(host.posY, dev.posY + dev.sizeY) << "Host band is not below the device headers";
  }
  for (const auto &plot : layout.plot_positions) {
    EXPECT_FALSE(host_positions_overlap(host, plot)) << "Host band overlaps a plot";
  }
  if (layout.process_position.sizeX > 0 && layout.process_position.sizeY > 0) {
    EXPECT_FALSE(host_positions_overlap(host, layout.process_position)) << "Host band overlaps the process list";
  }
}

} // namespace

TEST(HostMetricsLayout, FullBandOnLargeTerminal) {
  // Both host toggles on -> a 5-row full band is requested
  host_layout_result layout = compute_host_layout(2, 30, 140, 5);
  ASSERT_EQ(layout.host_position.sizeY, 5u);
  ASSERT_EQ(layout.host_position.sizeX, 140u);
  ASSERT_EQ(layout.host_position.posY, 3u); // Right below the 3-row device header
  check_host_band_consistent(layout, 30, 140);
}

TEST(HostMetricsLayout, CompactFallbackWhenFullBandDoesNotFit) {
  // With a 3-row header and 6 process rows, rows 10..13 leave 1..4 rows for
  // plots: the band shrinks to a single compact row instead of stealing rows
  for (unsigned rows : {10u, 11u, 12u, 13u}) {
    host_layout_result layout = compute_host_layout(2, rows, 140, 5);
    EXPECT_LE(layout.host_position.sizeY, 1u) << "rows=" << rows;
    check_host_band_consistent(layout, rows, 140);
  }
}

TEST(HostMetricsLayout, BandDroppedWhenNoRoom) {
  // Terminal too short for header + process + even a compact row
  host_layout_result layout = compute_host_layout(2, 8, 140, 5);
  EXPECT_EQ(layout.host_position.sizeY, 0u);
  EXPECT_EQ(layout.host_position.sizeX, 0u);
}

TEST(HostMetricsLayout, NoBandWhenHostPanelsDisabled) {
  // Zero toggles: no band is requested at all
  host_layout_result layout = compute_host_layout(2, 30, 140, 0);
  EXPECT_EQ(layout.host_position.sizeY, 0u);
  EXPECT_EQ(layout.host_position.sizeX, 0u);
}

TEST(HostMetricsLayout, ToggleCombinationsDoNotOverlapOrUnderflow) {
  // Zero, one and both toggles map to host_panel_rows of 0 or 5 (full) or 1
  // (compact, narrow terminal). Every combination must produce a valid,
  // overlap-free layout without unsigned underflow.
  const unsigned host_rows_cases[] = {0u, 5u, 1u};
  const unsigned rows_cases[] = {10u, 12u, 20u, 30u};
  const unsigned cols_cases[] = {40u, 80u, 140u};
  for (unsigned host_rows : host_rows_cases) {
    for (unsigned rows : rows_cases) {
      for (unsigned cols : cols_cases) {
        host_layout_result layout = compute_host_layout(2, rows, cols, host_rows);
        // Plots and process list still fit inside the terminal
        for (const auto &plot : layout.plot_positions) {
          EXPECT_LE(plot.posX + plot.sizeX, cols)
              << "plot exceeds terminal width (host=" << host_rows << " rows=" << rows << " cols=" << cols;
          EXPECT_LE(plot.posY + plot.sizeY, rows)
              << "plot exceeds terminal height (host=" << host_rows << " rows=" << rows << " cols=" << cols;
        }
        check_host_band_consistent(layout, rows, cols);
      }
    }
  }
}

TEST(HostMetricsLayout, CompactBandOnNarrowTerminal) {
  // Narrow terminal: compact single row, full width
  host_layout_result layout = compute_host_layout(2, 30, 30, 1);
  ASSERT_EQ(layout.host_position.sizeY, 1u);
  ASSERT_EQ(layout.host_position.sizeX, 30u);
  check_host_band_consistent(layout, 30, 30);
}

// ---------------------------------------------------------------------------
// Config persistence for the host panel options (F12 mechanism)
// ---------------------------------------------------------------------------

namespace {

nvtop_interface_option make_test_options(const char *pdev, bool cpu, bool ram) {
  nvtop_interface_option options;
  memset(&options, 0, sizeof(options));
  options.gpu_specific_opts = (nvtop_interface_gpu_opts *)calloc(1, sizeof(*options.gpu_specific_opts));
  if (options.gpu_specific_opts == nullptr)
    abort();
  static struct gpu_info device;
  memset(&device, 0, sizeof(device));
  strncpy(device.pdev, pdev, sizeof(device.pdev) - 1);
  options.gpu_specific_opts[0].linkedGpu = &device;
  options.show_host_cpu_panel = cpu;
  options.show_host_ram_panel = ram;
  options.update_interval = 1000;
  return options;
}

} // namespace

TEST(HostMetricsConfig, SaveLoadRoundTrip) {
  std::string path = temp_config_path();
  nvtop_interface_option saved = make_test_options("0000:01:00.0", true, false);
  char *location = (char *)malloc(path.size() + 1);
  if (location == nullptr)
    FAIL();
  strcpy(location, path.c_str());
  saved.config_file_location = location;

  ASSERT_TRUE(save_interface_options_to_config_file(1, &saved));

  nvtop_interface_option loaded = make_test_options("0000:01:00.0", false, true);
  loaded.config_file_location = location;
  ASSERT_TRUE(load_interface_options_from_config_file(1, &loaded));
  EXPECT_TRUE(loaded.show_host_cpu_panel);
  EXPECT_FALSE(loaded.show_host_ram_panel);
  EXPECT_EQ(loaded.update_interval, 1000u);

  free(loaded.gpu_specific_opts);
  free(saved.gpu_specific_opts);
  free(location);
  remove(path.c_str());
}

TEST(HostMetricsConfig, MissingHostKeysKeepDefaults) {
  // A config written by an older nvtop (no HostOption section) must not
  // clobber the in-memory host panel options.
  std::string path = temp_config_path();
  FILE *file = fopen(path.c_str(), "w");
  if (file == nullptr)
    FAIL();
  fputs("; legacy config without host options\n", file);
  fputs("[GeneralOption]\n", file);
  fputs("UseColor = true\n", file);
  fputs("UpdateInterval = 2000\n", file);
  fclose(file);

  nvtop_interface_option loaded = make_test_options("0000:01:00.0", true, true);
  char *location = (char *)malloc(path.size() + 1);
  if (location == nullptr)
    FAIL();
  strcpy(location, path.c_str());
  loaded.config_file_location = location;
  ASSERT_TRUE(load_interface_options_from_config_file(1, &loaded));
  EXPECT_TRUE(loaded.show_host_cpu_panel);
  EXPECT_TRUE(loaded.show_host_ram_panel);
  EXPECT_EQ(loaded.update_interval, 2000u);

  free(loaded.gpu_specific_opts);
  free(location);
  remove(path.c_str());
}
