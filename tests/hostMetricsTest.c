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

// Standalone (no GPU, no network, no gtest) check of the host metric parsers.
// Built by the test target and runnable on its own: ./hostMetricsTest

#include "nvtop/host_metrics.h"

#include <math.h>
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
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                          \
    }                                                                                                                  \
  } while (0)

static bool nearly_equal(double a, double b) { return fabs(a - b) < 1e-9; }

static void test_cpu_parsing(void) {
  struct host_cpu_ticks ticks = {1, 1};

  // Full modern aggregate line: user nice system idle iowait irq softirq steal guest guest_nice
  CHECK(host_cpu_parse_stat_text("cpu  100 0 0 100 0 0 0 0 0 0\n", &ticks));
  CHECK(ticks.total == 200u);
  CHECK(ticks.idle == 100u);

  // idle and iowait both count as idle time.
  CHECK(host_cpu_parse_stat_text("cpu  10 10 10 40 40 0 0 0 0 0\nintr 1\n", &ticks));
  CHECK(ticks.total == 110u);
  CHECK(ticks.idle == 80u);

  // Missing optional trailing fields are tolerated.
  CHECK(host_cpu_parse_stat_text("cpu  10 10 10 10\n", &ticks));
  CHECK(ticks.total == 40u);
  CHECK(ticks.idle == 10u);
  CHECK(host_cpu_parse_stat_text("cpu 10 10 10 10 5\n", &ticks));
  CHECK(ticks.total == 45u);
  CHECK(ticks.idle == 15u);

  // Unknown extra fields are ignored, not summed.
  CHECK(host_cpu_parse_stat_text("cpu  10 0 0 10 0 0 0 0 0 0 7\n", &ticks));
  CHECK(ticks.total == 20u);

  // guest time is already accounted in user: it must not be added again.
  CHECK(host_cpu_parse_stat_text("cpu  10 0 0 10 0 0 0 0 500 500\n", &ticks));
  CHECK(ticks.total == 20u);
  CHECK(ticks.idle == 10u);

  // Only per-cpu lines: no aggregate line.
  CHECK(!host_cpu_parse_stat_text("cpu0 1 1 1 1\ncpu1 1 1 1 1\n", &ticks));
  CHECK(ticks.total == 0u);

  // Malformed inputs.
  CHECK(!host_cpu_parse_stat_text("", &ticks));
  CHECK(!host_cpu_parse_stat_text("cpu\n", &ticks));
  CHECK(!host_cpu_parse_stat_text("cpu  1 2 3\n", &ticks));
  CHECK(!host_cpu_parse_stat_text("cpu  a b c d\n", &ticks));
  CHECK(!host_cpu_parse_stat_text("cpu  1 2 3 4 oops\n", &ticks));
  CHECK(!host_cpu_parse_stat_text("cpu  1 2 -3 4\n", &ticks));
  CHECK(!host_cpu_parse_stat_text("cpu  1 2 340282366920938463463374607431768211456 4\n", &ticks));
  CHECK(!host_cpu_parse_stat_text(NULL, &ticks));

  // The per-cpu lines must not be picked when the aggregate line is absent.
  CHECK(!host_cpu_parse_stat_text("intr 12\nbtime 9\ncpu0 1 1 1 1 1 1 1 1 0 0\n", &ticks));
}

static void test_cpu_utilization(void) {
  struct host_cpu_ticks previous = {1000, 750};
  struct host_cpu_ticks current = {1100, 825};
  double percent = -1.;

  // 50 busy ticks out of 100 elapsed ticks.
  CHECK(host_cpu_utilization(&previous, &current, &percent));
  CHECK(nearly_equal(percent, 25.));

  // Fully idle and fully busy intervals.
  struct host_cpu_ticks idle_now = {2000, 1750};
  CHECK(host_cpu_utilization(&previous, &idle_now, &percent) && nearly_equal(percent, 0.));
  struct host_cpu_ticks busy_now = {2000, 750};
  CHECK(host_cpu_utilization(&previous, &busy_now, &percent) && nearly_equal(percent, 100.));

  // No elapsed tick.
  percent = -1.;
  struct host_cpu_ticks same = previous;
  CHECK(!host_cpu_utilization(&previous, &same, &percent));
  CHECK(nearly_equal(percent, -1.));

  // Decreasing counters (reset, hotplug, ...).
  struct host_cpu_ticks backward = {900, 700};
  CHECK(!host_cpu_utilization(&previous, &backward, &percent));
  struct host_cpu_ticks idle_backward = {1100, 700};
  CHECK(!host_cpu_utilization(&previous, &idle_backward, &percent));

  // Unusable samples.
  struct host_cpu_ticks malformed = {10, 20};
  CHECK(!host_cpu_utilization(&previous, &malformed, &percent));
  CHECK(!host_cpu_utilization(NULL, &current, &percent));
  CHECK(!host_cpu_utilization(&previous, NULL, &percent));
  CHECK(!host_cpu_utilization(&previous, &current, NULL));
}

static void test_cpu_realistic_samples(void) {
  // Two samples taken from a real multi core /proc/stat aggregate line.
  struct host_cpu_ticks first = {0, 0}, second = {0, 0};
  CHECK(host_cpu_parse_stat_text("cpu  50046421 154764 15947860 8482668305 3448388 0 3631613 0 0 0\n"
                                 "intr 219917474 0 0 0\nctxt 12\n",
                                 &first));
  CHECK(host_cpu_parse_stat_text("cpu  50046945 154764 15948029 8482681982 3448388 0 3631712 0 0 0\n"
                                 "intr 219917500 0 0 0\nctxt 14\n",
                                 &second));
  CHECK(first.total > first.idle);
  CHECK(second.total > first.total);
  double percent = -1.;
  CHECK(host_cpu_utilization(&first, &second, &percent));
  // busy: 524 user + 169 system + 99 softirq over 14469 elapsed ticks.
  CHECK(nearly_equal(percent, 100. * 792. / 14469.));
}

static void test_memory_parsing(void) {
  struct host_memory_info info = {1, 1};
  CHECK(host_memory_parse_meminfo_text("MemTotal:       16384 kB\nMemFree: 1 kB\nMemAvailable:   4096 kB\n", &info));
  CHECK(info.total_kib == 16384u);
  CHECK(info.available_kib == 4096u);

  // Tabs, extra spaces and trailing whitespace are fine.
  CHECK(host_memory_parse_meminfo_text("MemTotal:\t1024   kB \nMemAvailable:0kB\n", &info));
  CHECK(info.total_kib == 1024u);
  CHECK(info.available_kib == 0u);

  // Other entries must not be mistaken for MemAvailable (MemAvailableTotal-like names).
  CHECK(host_memory_parse_meminfo_text("MemTotal: 1024 kB\nSwapTotal: 9 kB\nMemAvailable: 512 kB\n", &info));
  CHECK(info.total_kib == 1024u);
  CHECK(info.available_kib == 512u);

  // Missing MemAvailable must not be replaced by MemFree or a process sum.
  CHECK(!host_memory_parse_meminfo_text("MemTotal: 1024 kB\nMemFree: 1024 kB\n", &info));
  CHECK(info.total_kib == 0u && info.available_kib == 0u);
  CHECK(!host_memory_parse_meminfo_text("MemAvailable: 512 kB\n", &info));
  CHECK(!host_memory_parse_meminfo_text("", &info));
  CHECK(!host_memory_parse_meminfo_text("MemTotal: nonsense kB\nMemAvailable: 1 kB\n", &info));
  // An unknown unit makes the conversion unsafe.
  CHECK(!host_memory_parse_meminfo_text("MemTotal: 1024 MB\nMemAvailable: 1 kB\n", &info));
  CHECK(!host_memory_parse_meminfo_text("MemTotal: 1024\nMemAvailable: 1 kB\n", &info));
  // A zero total would produce a division by zero.
  CHECK(!host_memory_parse_meminfo_text("MemTotal: 0 kB\nMemAvailable: 0 kB\n", &info));
  CHECK(!host_memory_parse_meminfo_text(NULL, &info));

  // Duplicated entries: the first one wins.
  CHECK(host_memory_parse_meminfo_text("MemTotal: 2048 kB\nMemTotal: 4096 kB\nMemAvailable: 1024 kB\n", &info));
  CHECK(info.total_kib == 2048u);
}

static void test_memory_usage(void) {
  struct host_memory_info info = {16u * 1024u * 1024u, 4u * 1024u * 1024u}; // 16 GiB total, 4 GiB available
  double used = 0., total = 0., percent = 0.;
  CHECK(host_memory_usage(&info, &used, &total, &percent));
  CHECK(nearly_equal(used, 12.));
  CHECK(nearly_equal(total, 16.));
  CHECK(nearly_equal(percent, 75.));

  // MemAvailable larger than MemTotal is inconsistent: the memory must be
  // reported as unavailable, not as a suspicious zero usage.
  struct host_memory_info inconsistent = {1024u, 4096u};
  CHECK(!host_memory_usage(&inconsistent, &used, &total, &percent));

  // Nothing available.
  struct host_memory_info full = {1024u, 0u};
  CHECK(host_memory_usage(&full, &used, &total, &percent));
  CHECK(nearly_equal(percent, 100.));

  // Unusable input.
  struct host_memory_info empty = {0u, 0u};
  CHECK(!host_memory_usage(&empty, &used, &total, &percent));
  CHECK(!host_memory_usage(NULL, &used, &total, &percent));
  CHECK(!host_memory_usage(&full, NULL, &total, &percent));
}

// The legends of the combined host chart. Same buffer size as the chart legends.
#define LEGEND_BUFFER_SIZE 35u

static void test_legends(void) {
  struct host_metrics_state state;
  char buffer[LEGEND_BUFFER_SIZE];

  CHECK(host_metrics_init(&state, 32u));

  CHECK(strcmp(host_metric_name(host_metric_cpu), "CPU") == 0);
  CHECK(strcmp(host_metric_name(host_metric_memory), "RAM") == 0);

  // Nothing sampled yet: both lines must say N/A, never an idle zero.
  CHECK(host_metrics_format_legend(&state, host_metric_cpu, false, buffer, sizeof(buffer)) == 7u);
  CHECK(strcmp(buffer, "CPU N/A") == 0);
  CHECK(host_metrics_format_legend(&state, host_metric_memory, false, buffer, sizeof(buffer)) == 7u);
  CHECK(strcmp(buffer, "RAM N/A") == 0);
  CHECK(host_metrics_format_legend(&state, host_metric_memory, true, buffer, sizeof(buffer)) == 7u);
  CHECK(strcmp(buffer, "RAM N/A") == 0);
  // An unavailable host state is reported the same way.
  CHECK(host_metrics_format_legend(NULL, host_metric_cpu, false, buffer, sizeof(buffer)) == 7u);
  CHECK(strcmp(buffer, "CPU N/A") == 0);

  state.cpu_valid = true;
  state.cpu_percent = 42.5;
  CHECK(host_metrics_format_legend(&state, host_metric_cpu, false, buffer, sizeof(buffer)) == 9u);
  CHECK(strcmp(buffer, "CPU 42.5%") == 0);
  // The cpu legend has no detailed form to fall back from.
  CHECK(host_metrics_format_legend(&state, host_metric_cpu, true, buffer, sizeof(buffer)) == 9u);
  CHECK(strcmp(buffer, "CPU 42.5%") == 0);

  state.memory_valid = true;
  state.memory_used_gib = 12.;
  state.memory_total_gib = 16.;
  state.memory_percent = 75.;
  unsigned detailed =
      host_metrics_format_legend(&state, host_metric_memory, false, buffer, sizeof(buffer));
  CHECK(strcmp(buffer, "RAM 12.00/16.00 GiB 75.0%") == 0);
  unsigned short_form = host_metrics_format_legend(&state, host_metric_memory, true, buffer, sizeof(buffer));
  CHECK(strcmp(buffer, "RAM 75.0%") == 0);
  // The short form is what the narrow charts fall back to.
  CHECK(short_form < detailed);
  CHECK(short_form < LEGEND_BUFFER_SIZE);

  // Like snprintf, the return value tells the caller that the detailed form
  // did not fit, and the buffer stays a valid string.
  char small[12];
  CHECK(host_metrics_format_legend(&state, host_metric_memory, false, small, sizeof(small)) == detailed);
  CHECK(strlen(small) == sizeof(small) - 1);
  CHECK(host_metrics_format_legend(&state, host_metric_memory, true, small, sizeof(small)) == short_form);
  CHECK(strcmp(small, "RAM 75.0%") == 0);

  // Memory that went back to N/A after being available must not keep a stale
  // percentage either.
  state.memory_valid = false;
  CHECK(host_metrics_format_legend(&state, host_metric_memory, true, buffer, sizeof(buffer)) == 7u);
  CHECK(strcmp(buffer, "RAM N/A") == 0);

  // Unusable input.
  CHECK(host_metrics_format_legend(&state, host_metric_count, false, buffer, sizeof(buffer)) == 8u);
  CHECK(strcmp(buffer, "Host N/A") == 0);
  CHECK(host_metrics_format_legend(&state, host_metric_cpu, false, NULL, sizeof(buffer)) == 0u);
  CHECK(host_metrics_format_legend(&state, host_metric_cpu, false, buffer, 0u) == 0u);

  host_metrics_release(&state);
}

static void test_history(void) {
  struct host_metrics_state state;
  double sample = 1.;

  CHECK(host_metrics_init(&state, 0)); // clamped to the minimal capacity
  CHECK(state.history_capacity == 16u);
  // Nothing sampled yet: there is no sample to draw, and in particular not a
  // zero percent sample.
  CHECK(!host_metrics_history_get(&state, host_metric_cpu, 0u, &sample));
  CHECK(nearly_equal(sample, 1.));
  host_metrics_release(&state);

  CHECK(host_metrics_init(&state, 1u << 20)); // clamped to the maximal capacity
  CHECK(state.history_capacity == 4096u);
  host_metrics_release(&state);
  CHECK(state.history[host_metric_cpu] == NULL);

  // Feed the ring directly: the histories are part of the exposed state so the
  // ring bookkeeping can be checked without touching /proc.
  CHECK(host_metrics_init(&state, 16u));
  CHECK(state.history_count[host_metric_cpu] == 0u);
  for (unsigned i = 1; i <= 20u; ++i) {
    state.history[host_metric_cpu][state.history_next[host_metric_cpu]] = 10. * (double)i;
    state.history_next[host_metric_cpu] = (state.history_next[host_metric_cpu] + 1) % state.history_capacity;
    if (state.history_count[host_metric_cpu] < state.history_capacity)
      ++state.history_count[host_metric_cpu];
  }
  // The ring wrapped and kept the 16 most recent samples.
  CHECK(state.history_count[host_metric_cpu] == 16u);
  // The samples are read from the newest one, like the GPU ring buffer ones.
  CHECK(host_metrics_history_get(&state, host_metric_cpu, 0u, &sample));
  CHECK(nearly_equal(sample, 200.));
  CHECK(host_metrics_history_get(&state, host_metric_cpu, 7u, &sample));
  CHECK(nearly_equal(sample, 130.));
  CHECK(host_metrics_history_get(&state, host_metric_cpu, 15u, &sample));
  CHECK(nearly_equal(sample, 50.)); // oldest kept sample
  // The samples dropped by the wrap around are gone, and so is any sample of a
  // metric that was never available.
  CHECK(!host_metrics_history_get(&state, host_metric_cpu, 16u, &sample));
  CHECK(!host_metrics_history_get(&state, host_metric_memory, 0u, &sample));
  // Unknown metrics and unusable destinations are rejected.
  CHECK(!host_metrics_history_get(&state, host_metric_count, 0u, &sample));
  CHECK(!host_metrics_history_get(&state, host_metric_cpu, 0u, NULL));
  CHECK(!host_metrics_history_get(NULL, host_metric_cpu, 0u, &sample));
  host_metrics_release(&state);
}

static void test_update_without_support(void) {
  struct host_metrics_state state;
  CHECK(host_metrics_init(&state, 32u));
  if (!host_metrics_platform_supported()) {
    // Unsupported platforms must report unavailable data, not a zero load.
    CHECK(!host_metrics_update(&state));
    CHECK(!state.cpu_valid);
    CHECK(!state.memory_valid);
    CHECK(!state.supported);
  } else {
    // On Linux the sample must either be valid or explicitly unavailable.
    host_metrics_update(&state);
    if (state.cpu_valid)
      CHECK(state.cpu_percent >= 0. && state.cpu_percent <= 100.);
    if (state.memory_valid) {
      CHECK(state.memory_percent >= 0. && state.memory_percent <= 100.);
      CHECK(state.memory_total_gib > 0.);
    }
  }
  CHECK(!host_metrics_update(NULL));
  host_metrics_release(&state);
}

int main(void) {
  test_cpu_parsing();
  test_cpu_utilization();
  test_cpu_realistic_samples();
  test_memory_parsing();
  test_memory_usage();
  test_legends();
  test_history();
  test_update_without_support();
  printf("%s: %u checks, %u failures\n", failures ? "FAILED" : "PASSED", checks, failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
