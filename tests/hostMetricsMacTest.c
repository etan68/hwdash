/*
 *
 * Copyright (C) 2026 etan68
 *
 * This file is part of HWDash.
 *
 * HWDash is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HWDash is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HWDash.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// A live smoke test of the macOS host collector: it samples this machine
// through the collector the interface uses and checks what comes out of the Mach
// host ports and the sysctl MIB against the same kernel questions asked directly.
// Small on purpose, and runnable on its own without a GPU, ncurses or gtest:
//
//   ./build-macos/tests/hostMetricsMacTest
//
// A rate needs two samples, so the first one of a state only builds a reference.

#include "nvtop/host_metrics.h"
#include "nvtop/host_metrics_mac.h"

#include <mach/mach.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <time.h>

static unsigned checks = 0, failures = 0;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    ++checks;                                                                                                          \
    if (!(cond)) {                                                                                                     \
      ++failures;                                                                                                      \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                          \
    }                                                                                                                  \
  } while (0)

static bool read_u32(const char *name, uint32_t *value) {
  size_t size = sizeof(*value);
  return sysctlbyname(name, value, &size, NULL, 0) == 0 && size == sizeof(*value);
}

static void sleep_milliseconds(unsigned milliseconds) {
  struct timespec wait = {(time_t)(milliseconds / 1000u), (long)(milliseconds % 1000u) * 1000000L};
  nanosleep(&wait, NULL);
}

// The number of Mach ports this process holds, send rights included. The
// collector takes the host port and the reply allocations of the kernel at
// every refresh; this is how one tells them being given back from them
// accumulating in a process that redraws for a living.
static unsigned task_port_count(void) {
  mach_port_name_array_t names = NULL;
  mach_msg_type_number_t name_count = 0;
  mach_port_type_array_t types = NULL;
  mach_msg_type_number_t type_count = 0;
  if (mach_port_names(mach_task_self(), &names, &name_count, &types, &type_count) != KERN_SUCCESS)
    return 0u;
  const unsigned count = (unsigned)name_count;
  vm_deallocate(mach_task_self(), (vm_address_t)names, (vm_size_t)(name_count * sizeof(mach_port_name_t)));
  vm_deallocate(mach_task_self(), (vm_address_t)types, (vm_size_t)(type_count * sizeof(mach_port_type_t)));
  return count;
}

// The CPU reference belongs to the state that sampled it: a state that never
// sampled has no rate whatever the other states of this process already have, a
// released state takes nobody else's reference with it, and a re-initialized one
// starts over. Loops below stop at the first interval that moved a tick, so an
// interval too short to be a rate costs a retry rather than a failure.
static unsigned sample_until_cpu_valid(struct host_metrics_state *state, unsigned attempts) {
  for (unsigned i = 0; i < attempts; ++i) {
    sleep_milliseconds(150u);
    host_metrics_update(state);
    if (state->cpu_valid)
      return i + 1u;
  }
  return 0u;
}

static void test_independent_states(void) {
  struct host_metrics_state first, second;
  if (!host_metrics_init(&first, 16u) || !host_metrics_init(&second, 16u)) {
    CHECK(false);
    return;
  }

  // Neither of them has a reference of its own yet, and a rate is the interval
  // between two of its own samples: nothing else can supply one.
  CHECK(host_metrics_update(&first));
  CHECK(!first.cpu_valid);
  CHECK(first.history_count[host_metric_cpu] == 0u);
  CHECK(host_metrics_update(&second));
  CHECK(!second.cpu_valid);

  // Each of them measures its own interval, and each of them gets one sample of
  // its own into its own history.
  CHECK(sample_until_cpu_valid(&first, 6u) > 0u);
  CHECK(first.cpu_valid);
  CHECK(first.mac_cpu_reference != NULL);
  CHECK(first.history_count[host_metric_cpu] == 1u);
  CHECK(sample_until_cpu_valid(&second, 6u) > 0u);
  CHECK(second.history_count[host_metric_cpu] == 1u);

  // Releasing the first one gives its reference back and leaves the second one
  // measuring over its own interval, while the first one has no rate to report
  // until it has sampled twice again.
  host_metrics_release(&first);
  CHECK(first.mac_cpu_reference == NULL);
  CHECK(host_metrics_update(&first));
  CHECK(!first.cpu_valid);
  CHECK(sample_until_cpu_valid(&second, 6u) > 0u);
  CHECK(second.cpu_valid);
  CHECK(second.history_count[host_metric_cpu] == 2u);

  // A re-initialized state starts over: the state it used to be has nothing to
  // say about the interval it measures.
  CHECK(host_metrics_init(&first, 16u));
  CHECK(host_metrics_update(&first));
  CHECK(!first.cpu_valid);

  host_metrics_release(&first);
  host_metrics_release(&second);
}

int main(void) {
  printf("machine: %s\n",
#if defined(__APPLE__)
         "macOS"
#else
         "not macOS: the collector has no host to sample here"
#endif
  );
  if (!host_metrics_platform_supported()) {
    printf("host metrics are not supported on this platform: nothing to sample\n");
    return EXIT_SUCCESS;
  }

  struct host_metrics_state state;
  if (!host_metrics_init(&state, 64u)) {
    printf("FAIL could not allocate the host metrics\n");
    return EXIT_FAILURE;
  }

  // The first sample: the reference of a rate cannot be a rate of one sample.
  CHECK(host_metrics_update(&state));

  // The memory, from host_statistics64 and hw.memsize.
  CHECK(state.memory_valid);
  CHECK(isfinite(state.memory_percent));
  CHECK(state.memory_total_gib >= 0.5 && state.memory_total_gib <= 4096.);
  CHECK(state.memory_used_gib >= 0. && state.memory_used_gib <= state.memory_total_gib);
  CHECK(state.memory_percent > 0. && state.memory_percent <= 100.);
  CHECK(state.memory.available_kib <= state.memory.total_kib);
  // The total is the memory of the machine, and nothing else: the very same
  // question, asked to the same kernel directly, has to give the same answer.
  uint64_t memsize = 0;
  size_t size = sizeof(memsize);
  if (sysctlbyname("hw.memsize", &memsize, &size, NULL, 0) == 0 && memsize > 0)
    CHECK(state.memory.total_kib == memsize / 1024u);

  // The whole host CPU: a finite utilization between 0 and 100 out of the
  // second sample, and the same sample in the history the chart draws.
  sleep_milliseconds(300u);
  CHECK(host_metrics_update(&state));
  CHECK(state.cpu_valid);
  CHECK(isfinite(state.cpu_percent));
  CHECK(state.cpu_percent >= 0. && state.cpu_percent <= 100.);
  double history_sample = -1.;
  CHECK(host_metrics_history_get(&state, host_metric_cpu, 0u, &history_sample));
  CHECK(isfinite(history_sample));
  CHECK(history_sample >= 0. && history_sample <= 100.);

  // A second interval gives another rate rather than the same number copied
  // over, and a rate of the interval it was measured on: the reference is the
  // previous sample, not the first one.
  sleep_milliseconds(300u);
  CHECK(host_metrics_update(&state));
  CHECK(state.cpu_valid);
  CHECK(state.cpu_percent >= 0. && state.cpu_percent <= 100.);

  // The identity, and the counts the same sysctl answers to anybody asking.
  CHECK(state.identity_probed);
  CHECK(state.identity_valid);
  CHECK(state.identity.model_valid);
  CHECK(state.identity.model[0] != '\0');
  CHECK(state.identity.logical_threads > 0);
  CHECK(state.identity.physical_cores > 0);
  CHECK(state.identity.physical_cores <= state.identity.logical_threads);
  // The threads are the processors the kernel answers for hw.activecpu and the
  // cores the ones it answers for hw.physicalcpu. The collector never prints the
  // threads for the cores: a kernel that does not answer the second one leaves
  // the cores unknown, which the block shows as N/A.
  uint32_t active = 0, logical = 0, physical = 0;
  const bool answered_active = read_u32("hw.activecpu", &active) && active > 0;
  const bool answered_logical = read_u32("hw.logicalcpu", &logical) && logical > 0;
  if (answered_active)
    CHECK(state.identity.logical_threads == active);
  else if (answered_logical)
    CHECK(state.identity.logical_threads == logical);
  if (read_u32("hw.physicalcpu", &physical) && physical > 0 && (!answered_logical || physical <= logical))
    CHECK(state.identity.physical_cores == physical);
  else
    CHECK(state.identity.physical_cores == 0u);

  // The load averages of the kernel.
  CHECK(state.load_valid);
  double measured[3] = {0., 0., 0.};
  for (unsigned i = 0; i < 3; ++i) {
    CHECK(isfinite(state.load_avg[i]));
    CHECK(state.load_avg[i] >= 0.);
    CHECK(state.load_avg[i] < 10000.);
  }
  if (getloadavg(measured, 3) == 3) {
    for (unsigned i = 0; i < 3; ++i)
      CHECK(fabs(measured[i] - state.load_avg[i]) < 20.);
  }

  // What this platform does not measure stays unavailable: no live frequency and
  // no package power, and no swap at all when the kernel keeps its answer to
  // itself. None of them is ever replaced by a nameplate or a fabricated zero.
  CHECK(!state.freq_valid);
  CHECK(!state.power_valid);
  // The swap of this kernel, asked next to the sample it is compared with:
  // vm.swapusage answers either nothing, which is an unavailable swap that reads
  // N/A, or a pair - and a pair of two zeros is a machine with none allocated
  // right now, which displays 0/0 rather than N/A.
  host_metrics_update(&state);
  struct xsw_usage usage;
  size_t usage_size = sizeof(usage);
  const bool swap_answered = sysctlbyname("vm.swapusage", &usage, &usage_size, NULL, 0) == 0 &&
                             usage_size == sizeof(usage);
  CHECK(state.swap_valid == swap_answered);
  if (swap_answered) {
    CHECK(state.swap_used_gib >= 0. && state.swap_used_gib <= state.swap_total_gib);
    if (usage.xsu_total == 0u && usage.xsu_used == 0u)
      CHECK(state.swap_total_gib == 0. && state.swap_used_gib == 0.);
    else
      CHECK(state.swap_total_gib > 0.);
  }

  // The ports and the allocations the collector takes from the kernel. Every
  // refresh asks the host port for the tick counters and the vm statistics, and
  // gives both back; a right or a reply left behind is invisible on one refresh
  // and a process out of ports after an afternoon of redraws. Count the ports of
  // this process, refresh a batch of times, count them again.
  unsigned ports_before = task_port_count();
  unsigned cpu_samples = 0, memory_samples = 0;
  const unsigned refreshes = 60u;
  for (unsigned i = 0; i < refreshes; ++i) {
    sleep_milliseconds(20u);
    if (host_metrics_update(&state)) {
      cpu_samples += state.cpu_valid ? 1u : 0u;
      memory_samples += state.memory_valid ? 1u : 0u;
    }
  }
  const unsigned ports_after = task_port_count();
  printf("ports: %u before, %u after %u refreshes\n", ports_before, ports_after, refreshes);
  if (ports_before != 0u && ports_after != 0u)
    CHECK(ports_after == ports_before);
  // The rates of the intervals that were really measured: an interval that
  // landed inside one tick reports nothing, and a whole batch of them is not a
  // machine that has no cpu to measure.
  CHECK(cpu_samples >= refreshes / 3u);
  CHECK(memory_samples >= refreshes * 9u / 10u);

  // The states of the collector do not share a CPU reference, and the process
  // wide state the interface refreshes seeds its own when it is allocated.
  test_independent_states();
  struct host_metrics_state *shared = host_metrics_get_state();
  CHECK(shared != NULL);
  CHECK(shared == NULL || shared->mac_cpu_reference != NULL);

  char line[512];
  host_metrics_format_detail_line(&state, 0u, sizeof(line) - 1u, line, sizeof(line));
  printf("  %s\n", line);
  CHECK(strstr(line, "Device CPU") != NULL);
  CHECK(strstr(line, "CORES") != NULL);
  host_metrics_format_detail_line(&state, 1u, sizeof(line) - 1u, line, sizeof(line));
  printf("  %s\n", line);
  CHECK(strstr(line, "CPU ") != NULL);
  CHECK(strstr(line, "FREQ N/A") != NULL);
  CHECK(strstr(line, "POWER N/A") != NULL);
  CHECK(strstr(line, "LOAD ") != NULL);
  host_metrics_format_detail_line(&state, 2u, sizeof(line) - 1u, line, sizeof(line));
  printf("  %s\n", line);
  CHECK(strstr(line, "RAM ") != NULL);
  CHECK(strstr(line, "AVAIL ") != NULL);

  printf("sampled: CPU %.1f%%  RAM %.2f/%.2f GiB (%.1f%%, %.2f GiB available)  load %.2f/%.2f/%.2f\n",
         state.cpu_percent, state.memory_used_gib, state.memory_total_gib, state.memory_percent,
         state.memory_total_gib - state.memory_used_gib, state.load_avg[0], state.load_avg[1], state.load_avg[2]);
  printf("identity: %s  %u cores / %u threads\n", state.identity.model, state.identity.physical_cores,
         state.identity.logical_threads);

  host_metrics_release(&state);
  host_metrics_shutdown();
  printf("%s: %u checks, %u failures\n", failures ? "FAILED" : "PASSED", checks, failures);
  return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
