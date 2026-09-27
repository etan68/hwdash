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

// The legend keys of the combined host chart, at the width of the chart legend
// slots. A legend says which curve is which metric and nothing else, exactly
// like the GPU chart legends: the current values belong to the detail block.
#define LEGEND_BUFFER_SIZE 35u

static void test_legend_keys(void) {
  CHECK(strcmp(host_metric_name(host_metric_cpu), "CPU") == 0);
  CHECK(strcmp(host_metric_name(host_metric_memory), "RAM") == 0);

  // The key of the CPU line and the key of the memory line: a unit, no value.
  CHECK(strcmp(host_metric_legend_name(host_metric_cpu), "CPU %") == 0);
  CHECK(strcmp(host_metric_legend_name(host_metric_memory), "RAM %") == 0);
  CHECK(strlen(host_metric_legend_name(host_metric_cpu)) < LEGEND_BUFFER_SIZE);
  CHECK(strlen(host_metric_legend_name(host_metric_memory)) < LEGEND_BUFFER_SIZE);

  // The keys do not depend on the state: a metric that is not available yet, or
  // not available anymore, keeps the same key. Its value, or the lack of it, is
  // what the detail block says.
  struct host_metrics_state state;
  CHECK(host_metrics_init(&state, 32u));
  CHECK(strcmp(host_metric_legend_name(host_metric_cpu), "CPU %") == 0);
  state.cpu_valid = true;
  state.cpu_percent = 42.5;
  state.memory_valid = true;
  state.memory_used_gib = 12.;
  state.memory_total_gib = 16.;
  state.memory_percent = 75.;
  CHECK(strcmp(host_metric_legend_name(host_metric_cpu), "CPU %") == 0);
  CHECK(strcmp(host_metric_legend_name(host_metric_memory), "RAM %") == 0);
  CHECK(strchr(host_metric_legend_name(host_metric_memory), '0') == NULL);
  host_metrics_release(&state);

  // An unknown metric still yields a key rather than an empty string.
  CHECK(strcmp(host_metric_legend_name(host_metric_count), "Host %") == 0);
}

static void test_cpu_identity(void) {
  struct host_cpu_identity identity;

  // x86: model name, one logical thread per line, siblings and cpu cores give
  // the physical core count (2 threads per core, 8 cores here).
  CHECK(host_cpu_parse_cpuinfo_text(
      "processor\t: 0\nmodel name\t: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz\nsiblings\t: 12\ncpu cores\t: 6\n"
      "processor\t: 1\nmodel name\t: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz\nsiblings\t: 12\ncpu cores\t: 6\n",
      &identity));
  CHECK(strcmp(identity.model, "Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz") == 0);
  CHECK(identity.model_valid);
  CHECK(identity.logical_threads == 2u);

  // A full 12 thread listing of that machine: the physical core count follows.
  CHECK(host_cpu_parse_cpuinfo_text(
      "processor\t: 0\nmodel name\t: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz\nsiblings\t: 12\ncpu cores\t: 6\n"
      "processor\t: 1\nmodel name\t: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz\nsiblings\t: 12\ncpu cores\t: 6\n"
      "processor\t: 2\nmodel name\t: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz\nsiblings\t: 12\ncpu cores\t: 6\n"
      "processor\t: 3\nmodel name\t: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz\nsiblings\t: 12\ncpu cores\t: 6\n"
      "processor\t: 4\nmodel name\t: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz\nsiblings\t: 12\ncpu cores\t: 6\n"
      "processor\t: 5\nmodel name\t: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz\nsiblings\t: 12\ncpu cores\t: 6\n"
      "processor\t: 6\nmodel name\t: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz\nsiblings\t: 12\ncpu cores\t: 6\n"
      "processor\t: 7\nmodel name\t: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz\nsiblings\t: 12\ncpu cores\t: 6\n"
      "processor\t: 8\nmodel name\t: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz\nsiblings\t: 12\ncpu cores\t: 6\n"
      "processor\t: 9\nmodel name\t: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz\nsiblings\t: 12\ncpu cores\t: 6\n"
      "processor\t: 10\nmodel name\t: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz\nsiblings\t: 12\ncpu cores\t: 6\n"
      "processor\t: 11\nmodel name\t: Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz\nsiblings\t: 12\ncpu cores\t: 6\n",
      &identity));
  CHECK(identity.logical_threads == 12u);
  CHECK(identity.physical_cores == 6u);

  // ARM: no "model name", the model is the "Model" field, and there is no
  // siblings/cpu cores information at all.
  CHECK(host_cpu_parse_cpuinfo_text("processor\t: 0\nBogoMIPS\t: 100.00\nModel\t\t: RK3588 Rockchip (0x48a)\n"
                                    "processor\t: 1\nModel\t\t: RK3588 Rockchip (0x48a)\n",
                                    &identity));
  CHECK(strcmp(identity.model, "RK3588 Rockchip (0x48a)") == 0);
  CHECK(identity.logical_threads == 2u);
  CHECK(identity.physical_cores == 0u); // Not guessed when the fields are missing

  // The architectures that only have "Hardware" or "Processor".
  CHECK(host_cpu_parse_cpuinfo_text("processor\t: 0\nHardware\t: BCM2835\n", &identity));
  CHECK(strcmp(identity.model, "BCM2835") == 0);
  CHECK(identity.logical_threads == 1u);
  CHECK(host_cpu_parse_cpuinfo_text("Processor\t: ARMv7 Processor rev 4 (v7l)\n", &identity));
  CHECK(strncmp(identity.model, "ARMv7 Processor", 15) == 0);
  CHECK(identity.logical_threads == 0u); // It is a model, not a thread entry

  // An unreadably long model is truncated, not dropped.
  char long_cpuinfo[512];
  int written = snprintf(long_cpuinfo, sizeof(long_cpuinfo), "model name\t: ");
  for (unsigned i = 0; i < 200u; ++i)
    written += snprintf(long_cpuinfo + written, sizeof(long_cpuinfo) - (size_t)written, "x");
  CHECK(host_cpu_parse_cpuinfo_text(long_cpuinfo, &identity));
  CHECK(identity.model_valid);
  CHECK(strlen(identity.model) == HOST_CPU_MODEL_MAX_LENGTH);

  // Missing or malformed data stays unavailable instead of being invented.
  CHECK(!host_cpu_parse_cpuinfo_text("", &identity));
  CHECK(!identity.model_valid && identity.logical_threads == 0u && identity.physical_cores == 0u);
  CHECK(!host_cpu_parse_cpuinfo_text(NULL, &identity));
  CHECK(!host_cpu_parse_cpuinfo_text("vendor_id\t: GenuineIntel\ncpu MHz\t: 2600.000\n", &identity));
  // An empty model field is not a model, and a blank thread count is not a count.
  CHECK(!host_cpu_parse_cpuinfo_text("model name\t:\nprocessor\t: \n", &identity));

  // The sysfs processor lists, the format of /sys/devices/system/cpu/online.
  unsigned threads = 0;
  CHECK(host_cpu_parse_processor_list("0-47\n", &threads));
  CHECK(threads == 48u);
  CHECK(host_cpu_parse_processor_list("0,1,2,3\n", &threads));
  CHECK(threads == 4u);
  CHECK(host_cpu_parse_processor_list("0-3,8-11", &threads));
  CHECK(threads == 8u);
  CHECK(host_cpu_parse_processor_list("0", &threads));
  CHECK(threads == 1u);
  CHECK(host_cpu_parse_processor_list("0,2-4,10\n", &threads));
  CHECK(threads == 5u);

  CHECK(!host_cpu_parse_processor_list("-", &threads)); // Its size cannot be known
  CHECK(!host_cpu_parse_processor_list("", &threads));
  CHECK(!host_cpu_parse_processor_list("\n", &threads));
  CHECK(!host_cpu_parse_processor_list(NULL, &threads));
  CHECK(!host_cpu_parse_processor_list("3-1\n", &threads));    // Reversed range
  CHECK(!host_cpu_parse_processor_list("0-4096\n", &threads)); // More cpus than a machine can have
  CHECK(!host_cpu_parse_processor_list("0,1,junk\n", &threads));
  CHECK(!host_cpu_parse_processor_list("0,,1\n", &threads));
  CHECK(!host_cpu_parse_processor_list("0-3junk\n", &threads));
  CHECK(!host_cpu_parse_processor_list("0-\n", &threads));
}

static void test_load_average(void) {
  double load[3] = {0., 0., 0.};

  // The real /proc/loadavg line: the trailing fields are not load averages.
  CHECK(host_loadavg_parse_text("0.42 0.38 0.35 1/512 12345\n", load));
  CHECK(nearly_equal(load[0], 0.42) && nearly_equal(load[1], 0.38) && nearly_equal(load[2], 0.35));

  CHECK(host_loadavg_parse_text("0.00 0.01 0.00 2 2000\n", load));
  CHECK(nearly_equal(load[0], 0.) && nearly_equal(load[2], 0.));

  // Tabs and no trailing newline.
  CHECK(host_loadavg_parse_text("12.50\t11.25\t10.00", load));
  CHECK(nearly_equal(load[0], 12.5) && nearly_equal(load[1], 11.25) && nearly_equal(load[2], 10.));

  CHECK(!host_loadavg_parse_text("", load));
  CHECK(!host_loadavg_parse_text(NULL, load));
  CHECK(!host_loadavg_parse_text("0.42 0.38\n", load));       // One average missing
  CHECK(!host_loadavg_parse_text("a b c\n", load));           // Not numbers
  CHECK(!host_loadavg_parse_text("-0.10 0.20 0.30\n", load)); // Negative load is not a load
  CHECK(!host_loadavg_parse_text("0.42,0.38,0.35\n", load));  // Wrong separator
  CHECK(!host_loadavg_parse_text("1e400 0.1 0.2\n", load));   // Out of range
}

static void test_frequencies(void) {
  double mhz = 0.;

  // The cpufreq interface reports kilohertz.
  CHECK(host_freq_parse_scaling_freq_text("2400000\n", &mhz));
  CHECK(nearly_equal(mhz, 2400.));
  CHECK(host_freq_parse_scaling_freq_text(" 1234567 ", &mhz));
  CHECK(nearly_equal(mhz, 1234.567));
  CHECK(host_freq_parse_scaling_freq_text("400000", &mhz));
  CHECK(nearly_equal(mhz, 400.));

  // A core that is down reports nothing, and garbage is not a frequency.
  CHECK(!host_freq_parse_scaling_freq_text("0\n", &mhz));
  CHECK(!host_freq_parse_scaling_freq_text("", &mhz));
  CHECK(!host_freq_parse_scaling_freq_text(NULL, &mhz));
  CHECK(!host_freq_parse_scaling_freq_text("bogus\n", &mhz));
  CHECK(!host_freq_parse_scaling_freq_text("2400000 kHz\n", &mhz));
  CHECK(!host_freq_parse_scaling_freq_text("-1000\n", &mhz));
  CHECK(!host_freq_parse_scaling_freq_text("999\n", &mhz)); // Below a kilohertz

  // /proc/cpuinfo averages, one value per logical thread.
  CHECK(host_cpuinfo_parse_freq_text("processor\t: 0\ncpu MHz\t\t: 1000.000\n"
                                     "processor\t: 1\ncpu MHz\t\t: 3000.000\n",
                                     &mhz));
  CHECK(nearly_equal(mhz, 2000.));
  CHECK(host_cpuinfo_parse_freq_text("cpu MHz\t: 2600.123\n", &mhz));
  CHECK(nearly_equal(mhz, 2600.123));
  // The architectures that do not report a frequency there.
  CHECK(!host_cpuinfo_parse_freq_text("processor\t: 0\nBogoMIPS\t: 100.00\n", &mhz));
  CHECK(!host_cpuinfo_parse_freq_text("", &mhz));
  CHECK(!host_cpuinfo_parse_freq_text(NULL, &mhz));
  CHECK(!host_cpuinfo_parse_freq_text("cpu MHz\t: 0.000\n", &mhz));
  CHECK(!host_cpuinfo_parse_freq_text("cpu MHz\t: abc\n", &mhz));
  CHECK(!host_cpuinfo_parse_freq_text("cpu MHz\t:\n", &mhz));
}

static void test_swap(void) {
  struct host_swap_info swap = {1, 1};
  CHECK(host_swap_parse_meminfo_text("MemTotal: 1 kB\nSwapTotal:  2097152 kB\nSwapFree:   1048576 kB\n", &swap));
  CHECK(swap.total_kib == 2u * 1024u * 1024u);
  CHECK(swap.free_kib == 1024u * 1024u);

  // Tabs and no trailing unit spacing.
  CHECK(host_swap_parse_meminfo_text("SwapTotal:\t1024\tkB\nSwapFree:0kB\n", &swap));
  CHECK(swap.total_kib == 1024u && swap.free_kib == 0u);

  // Both fields are needed, and a machine without swap has nothing to report.
  CHECK(!host_swap_parse_meminfo_text("SwapTotal: 1024 kB\n", &swap));
  CHECK(!host_swap_parse_meminfo_text("SwapFree: 1024 kB\n", &swap));
  CHECK(!host_swap_parse_meminfo_text("SwapTotal: 0 kB\nSwapFree: 0 kB\n", &swap));
  CHECK(!host_swap_parse_meminfo_text("MemTotal: 1 kB\n", &swap));
  CHECK(!host_swap_parse_meminfo_text("", &swap));
  CHECK(!host_swap_parse_meminfo_text(NULL, &swap));
  CHECK(!host_swap_parse_meminfo_text("SwapTotal: 1024 MB\nSwapFree: 1 kB\n", &swap));
  CHECK(!host_swap_parse_meminfo_text("SwapTotal: junk kB\nSwapFree: 1 kB\n", &swap));
  // The swap totals are not confused with the memory ones.
  CHECK(!host_swap_parse_meminfo_text("MemTotal: 1024 kB\nMemAvailable: 1 kB\n", &swap));

  double used = 0., total = 0.;
  struct host_swap_info half = {2u * 1024u * 1024u, 1024u * 1024u};
  CHECK(host_swap_usage(&half, &used, &total));
  CHECK(nearly_equal(used, 1.));
  CHECK(nearly_equal(total, 2.));

  struct host_swap_info full = {1024u * 1024u, 0u};
  CHECK(host_swap_usage(&full, &used, &total));
  CHECK(nearly_equal(used, 1.));

  // More free swap than the machine has is inconsistent: unavailable, not a
  // plausible negative usage.
  struct host_swap_info inconsistent = {1024u, 2048u};
  CHECK(!host_swap_usage(&inconsistent, &used, &total));
  struct host_swap_info empty = {0u, 0u};
  CHECK(!host_swap_usage(&empty, &used, &total));
  CHECK(!host_swap_usage(NULL, &used, &total));
  CHECK(!host_swap_usage(&half, NULL, &total));
}

// The CPU detail block, at the width of the reference screenshot terminal.
#define DETAIL_WIDTH 162u

static void fill_detail_state(struct host_metrics_state *state) {
  memset(state, 0, sizeof(*state));
  state->supported = true;
  state->cpu_valid = true;
  state->cpu_percent = 12.5;
  state->memory_valid = true;
  state->memory_total_gib = 31.26;
  state->memory_used_gib = 3.21;
  state->memory_percent = 10.3;
  state->memory.total_kib = (uint64_t)(31.26 * 1024. * 1024.);
  state->memory.available_kib = (uint64_t)(28.05 * 1024. * 1024.);
  state->freq_valid = true;
  state->cpu_freq_mhz = 3400.;
  state->load_valid = true;
  state->load_avg[0] = 0.42;
  state->load_avg[1] = 0.38;
  state->load_avg[2] = 0.35;
  state->swap_valid = true;
  state->swap_used_gib = 0.50;
  state->swap_total_gib = 8.;
  state->power_valid = true;
  state->package_power_watts = 15.5;
  state->lenovo_fan_supported = true;
  state->lenovo_fan_valid = true;
  state->lenovo_fan_rpm = 1834u;
  state->identity_valid = true;
  state->identity.model_valid = true;
  snprintf(state->identity.model, sizeof(state->identity.model), "%s", "Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz");
  state->identity.physical_cores = 6u;
  state->identity.logical_threads = 12u;
}

// Width at which a field disappears from a line of the detail block, looking
// for the first time as the terminal narrows. 0 when it never disappears.
static unsigned first_width_without_at = DETAIL_WIDTH;
static char first_width_buffer[512];
static unsigned first_width_without(const struct host_metrics_state *state, unsigned line_index, const char *needle) {
  for (unsigned width = first_width_without_at; width >= 10u; --width) {
    if (host_metrics_format_detail_line(state, line_index, width, first_width_buffer, sizeof(first_width_buffer)) !=
        strlen(first_width_buffer))
      continue; // truncated rather than dropped: the line is not complete yet
    if (strstr(first_width_buffer, needle) == NULL)
      return width;
  }
  return 0u;
}

static void test_detail_block(void) {
  struct host_metrics_state state;
  char line[512];

  fill_detail_state(&state);

  // Everything known, on a wide enough terminal.
  CHECK(host_metrics_format_detail_line(&state, 0u, DETAIL_WIDTH, line, sizeof(line)) ==
        strlen("Device CPU [Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz]  CORES 6C/12T"));
  CHECK(strcmp(line, "Device CPU [Intel(R) Core(TM) i7-9750H CPU @ 2.60GHz]  CORES 6C/12T") == 0);
  CHECK(host_metrics_format_detail_line(&state, 1u, DETAIL_WIDTH, line, sizeof(line)) ==
        strlen("CPU  12.5%   FREQ 3.40GHz   LOAD 0.42 / 0.38 / 0.35   POWER 15.5W   Lenovo CPU Fan 1834 RPM"));
  CHECK(strcmp(line, "CPU  12.5%   FREQ 3.40GHz   LOAD 0.42 / 0.38 / 0.35   POWER 15.5W   Lenovo CPU Fan 1834 RPM") == 0);
  CHECK(strstr(line, "LOAD 0.42 / 0.38 / 0.35") != NULL);
  CHECK(strstr(line, "POWER 15.5W") != NULL);
  CHECK(strstr(line, "Lenovo CPU Fan 1834 RPM") != NULL);
  CHECK(host_metrics_format_detail_line(&state, 2u, DETAIL_WIDTH, line, sizeof(line)) ==
        strlen("RAM  3.21/31.26 GiB 10.3%   AVAIL 28.05 GiB   SWAP 0.50/8.00 GiB"));
  CHECK(strstr(line, "RAM  3.21/31.26 GiB 10.3%") == line);
  CHECK(strstr(line, "AVAIL 28.05 GiB") != NULL);
  CHECK(strstr(line, "SWAP 0.50/8.00 GiB") != NULL);

  // Nothing but the utilization lines is kept when the terminal gets narrow,
  // and, on each line, the fields go in the responsive order: the swap before
  // the available memory, the 15 minute load average before the 5 minute one,
  // and the package power before the static CPU information.
  const unsigned width_no_swap = first_width_without(&state, 2u, "SWAP");
  const unsigned width_no_load15 = first_width_without(&state, 1u, "/ 0.35");
  const unsigned width_no_load5 = first_width_without(&state, 1u, "/ 0.38");
  const unsigned width_no_avail = first_width_without(&state, 2u, "AVAIL");
  const unsigned width_no_fan = first_width_without(&state, 1u, "Lenovo CPU Fan");
  const unsigned width_no_power = first_width_without(&state, 1u, "POWER");
  const unsigned width_no_freq = first_width_without(&state, 1u, "FREQ");
  CHECK(width_no_swap > 0u && width_no_load15 > 0u && width_no_load5 > 0u && width_no_avail > 0u);
  CHECK(width_no_swap > width_no_avail);   // On the memory line, the swap goes
                                           // before the available memory,
  CHECK(width_no_load15 > width_no_load5); // and on the CPU line the load
                                           // averages go newest last,
  CHECK(width_no_load5 > width_no_fan);    // then the fan speed,
  CHECK(width_no_fan > width_no_power);    // then the package power,
  CHECK(width_no_power > width_no_freq);   // then the static information.
  // Where the power had to go, the CPU utilization is still there: the fields
  // that carry it are never dropped.
  first_width_without(&state, 1u, "POWER");
  CHECK(strstr(first_width_buffer, "CPU  12.5%") != NULL);
  CHECK(strstr(first_width_buffer, "POWER") == NULL);

  // The static information is what survives the longest after the utilization:
  // a line narrower than the model still shows the device line, and the model
  // itself is truncated rather than dropped whole.
  host_metrics_format_detail_line(&state, 0u, 10u, line, sizeof(line));
  CHECK(strcmp(line, "Device CPU") == 0);

  // Whatever is left, the CPU utilization and the memory usage stay, and the
  // line never overflows the width it was given.
  host_metrics_format_detail_line(&state, 1u, 8u, line, sizeof(line));
  CHECK(strlen(line) == 8u);
  CHECK(line[7] == '+' && strncmp(line, "CPU  1", 6) == 0);
  host_metrics_format_detail_line(&state, 2u, 22u, line, sizeof(line));
  CHECK(strlen(line) <= 22u);
  CHECK(strncmp(line, "RAM  ", 5) == 0);

  // An overlong model name is truncated the same way, at the width asked for.
  struct host_metrics_state long_state = state;
  long_state.memory_valid = false;
  host_metrics_format_detail_line(&long_state, 0u, 40u, line, sizeof(line));
  CHECK(strlen(line) == 40u); // exactly the width it was given
  CHECK(strncmp(line, "Device CPU [Intel", 17) == 0);
  CHECK(strchr(line, '+') != NULL); // with a marker where it was cut

  // Unknown data reads N/A: never a fake zero or a fake model name.
  struct host_metrics_state unknown;
  memset(&unknown, 0, sizeof(unknown));
  host_metrics_format_detail_line(&unknown, 0u, DETAIL_WIDTH, line, sizeof(line));
  CHECK(strcmp(line, "Device CPU [N/A]  CORES N/A/N/A") == 0);
  host_metrics_format_detail_line(&unknown, 1u, DETAIL_WIDTH, line, sizeof(line));
  CHECK(strcmp(line, "CPU  N/A   FREQ N/A   LOAD N/A   POWER N/A") == 0);
  host_metrics_format_detail_line(&unknown, 2u, DETAIL_WIDTH, line, sizeof(line));
  CHECK(strcmp(line, "RAM  N/A   SWAP N/A") == 0);

  // A partially known identity shows what is known.
  struct host_metrics_state half_identity = unknown;
  half_identity.identity_valid = true;
  half_identity.identity.model_valid = true;
  snprintf(half_identity.identity.model, sizeof(half_identity.identity.model), "%s", "Apple M2");
  half_identity.identity.logical_threads = 8u;
  host_metrics_format_detail_line(&half_identity, 0u, DETAIL_WIDTH, line, sizeof(line));
  CHECK(strcmp(line, "Device CPU [Apple M2]  CORES N/A/8T") == 0);

  // Like snprintf: the return value is the length the line would have needed,
  // and the buffer always stays a valid string.
  char small[8];
  CHECK(host_metrics_format_detail_line(&state, 0u, DETAIL_WIDTH, small, sizeof(small)) > sizeof(small));
  CHECK(strlen(small) == sizeof(small) - 1u);

  // Unusable input.
  CHECK(host_metrics_format_detail_line(&state, HOST_DETAIL_LINE_COUNT, DETAIL_WIDTH, line, sizeof(line)) == 0u);
  CHECK(host_metrics_format_detail_line(&state, 0u, 0u, line, sizeof(line)) == 0u);
  CHECK(host_metrics_format_detail_line(&state, 0u, DETAIL_WIDTH, NULL, 0u) == 0u);
  CHECK(host_metrics_format_detail_line(NULL, 0u, DETAIL_WIDTH, line, sizeof(line)) > 0u);
  CHECK(strcmp(line, "Device CPU [N/A]  CORES N/A/N/A") == 0);
}

// The text a reported title points at, so that what the interface is going to
// color can be checked against the line it was computed with.
static void reported_title_text(const char *line, const struct host_detail_field *field, char *out, size_t size) {
  if (size == 0)
    return;
  out[0] = '\0';
  if (!field || field->length == 0u)
    return;
  if ((size_t)field->offset + (size_t)field->length > strlen(line))
    return; // A title past the line the caller got: it is not in it.
  size_t length = field->length < size - 1u ? field->length : size - 1u;
  memcpy(out, line + field->offset, length);
  out[length] = '\0';
}

static unsigned reported_title_count(const struct host_detail_field *fields, const char *line) {
  unsigned count = 0u;
  char text[32];
  for (unsigned i = 0; i < HOST_DETAIL_FIELD_MAX; ++i) {
    reported_title_text(line, &fields[i], text, sizeof(text));
    if (text[0] != '\0')
      ++count;
  }
  return count;
}

static bool reported_title_is(const struct host_detail_field *fields, const char *line, const char *title) {
  char text[32];
  for (unsigned i = 0; i < HOST_DETAIL_FIELD_MAX; ++i) {
    reported_title_text(line, &fields[i], text, sizeof(text));
    if (text[0] != '\0' && strcmp(text, title) == 0)
      return true;
  }
  return false;
}

// The titles of the CPU detail block and the columns they occupy: the interface
// colors exactly those, the way the GPU detail blocks color theirs, and leaves
// the values in the normal color.
static void test_detail_field_titles(void) {
  struct host_metrics_state state;
  char line[512];
  struct host_detail_field fields[HOST_DETAIL_FIELD_MAX];

  fill_detail_state(&state);

  // Everything known: the titles of the three lines are the field names.
  CHECK(host_metrics_format_detail_line_fields(&state, 0u, DETAIL_WIDTH, line, sizeof(line), fields,
                                               HOST_DETAIL_FIELD_MAX) == strlen(line));
  CHECK(reported_title_count(fields, line) == 2u);
  CHECK(fields[0].offset == 0u); // "Device CPU" starts the line
  CHECK(fields[0].length == strlen("Device CPU"));
  CHECK(reported_title_is(fields, line, "Device CPU"));
  CHECK(reported_title_is(fields, line, "CORES"));
  // The model name of this CPU contains the "CPU" title of another line: the
  // titles are the fields, not whatever text happens to look like one, so the
  // only titles of this line are the two that were reported.
  CHECK(strstr(line, "CPU @ 2.60GHz") != NULL);

  CHECK(host_metrics_format_detail_line_fields(&state, 1u, DETAIL_WIDTH, line, sizeof(line), fields,
                                               HOST_DETAIL_FIELD_MAX) == strlen(line));
  CHECK(reported_title_count(fields, line) == 5u);
  CHECK(reported_title_is(fields, line, "CPU"));
  CHECK(reported_title_is(fields, line, "FREQ"));
  CHECK(reported_title_is(fields, line, "LOAD"));
  CHECK(reported_title_is(fields, line, "POWER"));
  CHECK(reported_title_is(fields, line, "Lenovo CPU Fan"));

  CHECK(host_metrics_format_detail_line_fields(&state, 2u, DETAIL_WIDTH, line, sizeof(line), fields,
                                               HOST_DETAIL_FIELD_MAX) == strlen(line));
  CHECK(reported_title_count(fields, line) == 3u);
  CHECK(reported_title_is(fields, line, "RAM"));
  CHECK(reported_title_is(fields, line, "AVAIL"));
  CHECK(reported_title_is(fields, line, "SWAP"));

  // A narrow line: the fields that were dropped have no title left to color, and
  // no reported title reaches past the line that was written.
  CHECK(host_metrics_format_detail_line_fields(&state, 1u, 8u, line, sizeof(line), fields,
                                               HOST_DETAIL_FIELD_MAX) > 8u);
  CHECK(strlen(line) == 8u);
  CHECK(reported_title_count(fields, line) == 1u);
  CHECK(fields[0].offset == 0u && fields[0].length == strlen("CPU"));
  CHECK(!reported_title_is(fields, line, "POWER"));

  // Unknown values keep their titles: N/A is a value of the field it belongs to.
  struct host_metrics_state unknown;
  memset(&unknown, 0, sizeof(unknown));
  CHECK(host_metrics_format_detail_line_fields(&unknown, 1u, DETAIL_WIDTH, line, sizeof(line), fields,
                                               HOST_DETAIL_FIELD_MAX) == strlen(line));
  CHECK(reported_title_count(fields, line) == 4u);
  CHECK(strcmp(line, "CPU  N/A   FREQ N/A   LOAD N/A   POWER N/A") == 0);

  // A Lenovo machine keeps the field visible while its privileged helper is
  // unavailable; another vendor has no Lenovo-only field at all.
  unknown.lenovo_fan_supported = true;
  CHECK(host_metrics_format_detail_line_fields(&unknown, 1u, DETAIL_WIDTH, line, sizeof(line), fields,
                                               HOST_DETAIL_FIELD_MAX) == strlen(line));
  CHECK(reported_title_count(fields, line) == 5u);
  CHECK(strcmp(line, "CPU  N/A   FREQ N/A   LOAD N/A   POWER N/A   Lenovo CPU Fan N/A") == 0);

  // Unusable input: the titles are optional.
  CHECK(host_metrics_format_detail_line_fields(&state, 1u, DETAIL_WIDTH, line, sizeof(line), NULL, 0u) ==
        strlen("CPU  12.5%   FREQ 3.40GHz   LOAD 0.42 / 0.38 / 0.35   POWER 15.5W   Lenovo CPU Fan 1834 RPM"));
  CHECK(host_metrics_format_detail_line_fields(&state, 1u, DETAIL_WIDTH, line, sizeof(line), fields, 0u) ==
        strlen(line));
  CHECK(host_metrics_format_detail_line_fields(&state, HOST_DETAIL_LINE_COUNT, DETAIL_WIDTH, line, sizeof(line),
                                               fields, HOST_DETAIL_FIELD_MAX) == 0u);
}

// The package power: the counters, the wrap around, and everything that makes a
// power untrustworthy. The parsers are pure, so all of this is checked whether
// or not the machine the test runs on exposes an energy counter.
static void test_package_power(void) {
  uint64_t counter = 12345;

  // The content of an energy_uj or max_energy_range_uj file.
  CHECK(host_power_parse_energy_counter_text("1099511627775\n", &counter));
  CHECK(counter == 1099511627775ull);
  CHECK(host_power_parse_energy_uj_text("  42\n", &counter));
  CHECK(counter == 42u);
  CHECK(host_power_parse_energy_uj_text("0\n", &counter) == false); // no energy reported
  CHECK(host_power_parse_energy_counter_text("0\n", &counter));     // a range may be odd, not this
  CHECK(!host_power_parse_energy_uj_text("", &counter));
  CHECK(!host_power_parse_energy_uj_text("   \n", &counter));
  CHECK(!host_power_parse_energy_uj_text("not a counter\n", &counter));
  CHECK(!host_power_parse_energy_uj_text("-12\n", &counter));
  CHECK(!host_power_parse_energy_uj_text("12345678901234567890123456789\n", &counter));
  CHECK(!host_power_parse_energy_uj_text("18446744073709551615\n", &counter));
  CHECK(!host_power_parse_energy_uj_text("42 joules\n", &counter)); // trailing text
  CHECK(!host_power_parse_energy_uj_text(NULL, &counter));
  CHECK(!host_power_parse_energy_uj_text("42\n", NULL));
  CHECK(!host_power_parse_energy_counter_text("42\n", NULL));

  // The zone names. The package is found by its name, not by a fixed path.
  char name[32];
  CHECK(host_power_parse_zone_name_text("package-0\n", name, sizeof(name)));
  CHECK(strcmp(name, "package-0") == 0);
  CHECK(host_power_parse_zone_name_text("  package-1 \r\n", name, sizeof(name)));
  CHECK(strcmp(name, "package-1") == 0);
  CHECK(!host_power_parse_zone_name_text("", name, sizeof(name)));
  CHECK(!host_power_parse_zone_name_text(" \n", name, sizeof(name)));
  CHECK(!host_power_parse_zone_name_text(NULL, name, sizeof(name)));
  CHECK(!host_power_parse_zone_name_text("package-0\n", name, 0u));

  unsigned index = 7u;
  CHECK(host_power_zone_name_index("package-0", &index) && index == 0u);
  CHECK(host_power_zone_name_index("package-2", &index) && index == 2u);
  CHECK(host_power_zone_name_index("PACKAGE-1", &index) && index == 1u);
  CHECK(host_power_zone_name_index("package", &index) && index == 0u);
  CHECK(host_power_zone_name_index("pkg-1", &index) && index == 1u);
  CHECK(host_power_zone_name_index("soc-0", &index) && index == 0u);
  CHECK(host_power_zone_name_index("amd", &index) && index == 0u);
  CHECK(!host_power_zone_name_index("core", &index));   // a sub zone of a package
  CHECK(!host_power_zone_name_index("dram", &index));   // is not the package
  CHECK(!host_power_zone_name_index("uncore", &index));
  CHECK(!host_power_zone_name_index("platform", &index));
  CHECK(!host_power_zone_name_index("package-x", &index));  // no plain index
  CHECK(!host_power_zone_name_index("package-", &index));
  CHECK(!host_power_zone_name_index("package-0:core", &index));
  CHECK(!host_power_zone_name_index("cpu", &index)); // the name of a core domain
  CHECK(!host_power_zone_name_index("", &index));              // on some drivers
  CHECK(!host_power_zone_name_index(NULL, &index));
  CHECK(!host_power_zone_name_index("package-0", NULL));
  CHECK(host_power_zone_name_is_package("package-1"));
  CHECK(!host_power_zone_name_is_package("dram"));

  // The hwmon labels: only a label that names the package may stand for it.
  CHECK(host_power_hwmon_label_is_package("Package"));
  CHECK(host_power_hwmon_label_is_package("package power"));
  CHECK(host_power_hwmon_label_is_package("Pkg"));
  CHECK(host_power_hwmon_label_is_package("VDDPKG"));
  CHECK(host_power_hwmon_label_is_package("PCH + CPU Package"));
  CHECK(!host_power_hwmon_label_is_package("Vcore"));
  CHECK(!host_power_hwmon_label_is_package("+12V"));
  CHECK(!host_power_hwmon_label_is_package(""));
  CHECK(!host_power_hwmon_label_is_package(NULL));

  // An hwmon power input is in microwatts.
  double watts = 0.;
  CHECK(host_power_parse_hwmon_input_text("12345678\n", &watts));
  CHECK(nearly_equal(watts, 12.345678));
  CHECK(host_power_parse_hwmon_input_text("1000000000\n", &watts)); // a kilowatt is a lot
  CHECK(nearly_equal(watts, 1000.));
  CHECK(!host_power_parse_hwmon_input_text("1001000000\n", &watts)); // past what a package draws
  CHECK(host_power_parse_hwmon_input_text("0\n", &watts) == false);           // reports nothing
  CHECK(!host_power_parse_hwmon_input_text("-5\n", &watts));
  CHECK(!host_power_parse_hwmon_input_text("abc\n", &watts));
  CHECK(!host_power_parse_hwmon_input_text(NULL, &watts));
  CHECK(!host_power_parse_hwmon_input_text("5\n", NULL));

  // The watts between two samples of an energy counter.
  struct host_power_sample previous = {1000000000ull, 1000000000000ull};
  struct host_power_sample current = {1100000000ull, 1000000000000ull};
  CHECK(host_power_watts_between(&previous, &current, 1., &watts));
  CHECK(nearly_equal(watts, 100.)); // 0.1 J over 0.1 s... 100 ms worth of 100W
  CHECK(host_power_watts_between(&previous, &current, 10., &watts));
  CHECK(nearly_equal(watts, 10.));
  CHECK(host_power_watts_between(&previous, &current, 0.5, &watts));
  CHECK(nearly_equal(watts, 200.));

  // The counter wrapping at its max energy range is the normal way of counting.
  previous.energy_uj = 999000000000ull;
  current.energy_uj = 500000000ull;
  // A wrap is only believable when what it adds up to is a believable power:
  // 1500W of package is not, so this is a counter that changed and it is refused.
  CHECK(!host_power_watts_between(&previous, &current, 1., &watts));
  previous.energy_uj = 999000000000ull;
  current.energy_uj = 500000000ull;
  CHECK(host_power_watts_between(&previous, &current, 60., &watts)); // 1500J over a minute
  CHECK(nearly_equal(watts, 25.));

  previous.energy_uj = 999999000000ull;
  current.energy_uj = 1000000ull;
  CHECK(host_power_watts_between(&previous, &current, 1., &watts));
  CHECK(nearly_equal(watts, 2.));

  // A first sample, no elapsed time, or a counter that did not move: nothing to
  // report, and in particular not the 0W an idle machine would never draw.
  CHECK(!host_power_watts_between(NULL, &current, 1., &watts));
  CHECK(!host_power_watts_between(&previous, NULL, 1., &watts));
  CHECK(!host_power_watts_between(&previous, &current, 0., &watts));
  CHECK(!host_power_watts_between(&previous, &current, -1., &watts));
  CHECK(!host_power_watts_between(&previous, &current, 0. / 0., &watts));
  CHECK(!host_power_watts_between(&previous, &previous, 1., &watts));
  CHECK(!host_power_watts_between(&previous, &current, 1., NULL));
  struct host_power_sample empty_counter = {0, 1000000000000ull};
  CHECK(!host_power_watts_between(&empty_counter, &current, 1., &watts));
  CHECK(!host_power_watts_between(&previous, &empty_counter, 1., &watts));
  struct host_power_sample no_range = {previous.energy_uj, 0};
  CHECK(!host_power_watts_between(&no_range, &current, 1., &watts));
  struct host_power_sample other_range = {previous.energy_uj, 2000000000000ull};
  CHECK(!host_power_watts_between(&other_range, &current, 1., &watts)); // another counter

  // A decreasing counter that no wrap explains: a reset, a module reloaded, a
  // zone that came back with another value. It would read as a huge burst.
  previous.energy_uj = 500000000000ull;
  current.energy_uj = 1000000ull;
  CHECK(!host_power_watts_between(&previous, &current, 1., &watts));

  // A counter past the range it declares is not a counter to trust.
  previous.energy_uj = 1000000ull;
  current.energy_uj = 1000000000001ull;
  CHECK(!host_power_watts_between(&previous, &current, 1., &watts));

  // A power no CPU package can draw is refused, not displayed.
  previous.energy_uj = 1000000ull;
  current.energy_uj = 1000000ull + 1000000000000ull - 1ull;
  CHECK(!host_power_watts_between(&previous, &current, 1., &watts));

  // What is left is a power: within the range of a real package.
  previous.energy_uj = 1000000ull;
  current.energy_uj = 1000000ull + 30000000ull; // 30 J
  CHECK(host_power_watts_between(&previous, &current, 1., &watts));
  CHECK(nearly_equal(watts, 30.));
}

static void test_cpu_fan_rpm(void) {
  unsigned rpm = 99u;
  CHECK(host_lenovo_fan_parse_rpm_text("1834\n", &rpm));
  CHECK(rpm == 1834u);
  CHECK(host_lenovo_fan_parse_rpm_text("  30000 \r\n", &rpm));
  CHECK(rpm == 30000u);
  CHECK(host_lenovo_fan_parse_rpm_text("0\n", &rpm));
  CHECK(rpm == 0u);
  CHECK(!host_lenovo_fan_parse_rpm_text("30001\n", &rpm));
  CHECK(!host_lenovo_fan_parse_rpm_text("-100\n", &rpm));
  CHECK(!host_lenovo_fan_parse_rpm_text("1200 RPM\n", &rpm));
  CHECK(!host_lenovo_fan_parse_rpm_text("", &rpm));
  CHECK(!host_lenovo_fan_parse_rpm_text(NULL, &rpm));
  CHECK(!host_lenovo_fan_parse_rpm_text("1200\n", NULL));
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
  char line[512];
  CHECK(host_metrics_init(&state, 32u));
  if (!host_metrics_platform_supported()) {
    // Unsupported platforms must report unavailable data, not a zero load.
    CHECK(!host_metrics_update(&state));
    CHECK(!state.cpu_valid);
    CHECK(!state.memory_valid);
    CHECK(!state.supported);
    // No host metrics at all means no package power either: the field reads N/A.
    CHECK(!state.power_valid);
    CHECK(host_metrics_format_detail_line(&state, 1u, 60u, line, sizeof(line)) > 0u);
    CHECK(strstr(line, "POWER N/A") != NULL);
  } else {
    // On Linux the sample must either be valid or explicitly unavailable.
    host_metrics_update(&state);
    // The power is either a plausible power or unavailable: never a zero that
    // the machine was asked for and did not give.
    if (state.power_valid)
      CHECK(state.package_power_watts > 0. && state.package_power_watts <= HOST_POWER_MAX_PLAUSIBLE_WATTS);
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
  test_cpu_identity();
  test_load_average();
  test_frequencies();
  test_swap();
  test_detail_block();
  test_detail_field_titles();
  test_package_power();
  test_cpu_fan_rpm();
  test_legend_keys();
  test_history();
  test_update_without_support();
  printf("%s: %u checks, %u failures\n", failures ? "FAILED" : "PASSED", checks, failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
