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

#ifndef HOST_METRICS_H__
#define HOST_METRICS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Whole host metrics (as opposed to the per GPU metrics provided by the
// vendor backends). The parsing helpers are pure functions of the text they
// are given so that they can be tested without a live /proc filesystem.

// Number of history samples kept for each host metric.
#define HOST_METRICS_HISTORY_SIZE 512u

enum host_metric {
  host_metric_cpu = 0,
  host_metric_memory = 1,
  host_metric_count = 2,
};

// Aggregate tick counters read from the "cpu " line of /proc/stat.
// guest and guest_nice are deliberately left out: the kernel already accounts
// them inside user and nice, adding them again would double count guest time.
struct host_cpu_ticks {
  uint64_t total; // Busy and idle ticks (guest time excluded)
  uint64_t idle;  // idle + iowait ticks
};

// Aggregate memory read from /proc/meminfo. Values are expressed in KiB.
// Used memory is defined by the kernel as MemTotal - MemAvailable.
struct host_memory_info {
  uint64_t total_kib;
  uint64_t available_kib;
};

// Swap read from /proc/meminfo. Used swap is SwapTotal - SwapFree.
struct host_swap_info {
  uint64_t total_kib;
  uint64_t free_kib;
};

// Static identity of the whole host CPU. One package and one CPU is the
// targeted case: the machine has a single model, a single core count and a
// single thread count. A field that cannot be read stays invalid: it is
// reported as N/A and never guessed.
#define HOST_CPU_MODEL_MAX_LENGTH 64u

struct host_cpu_identity {
  char model[HOST_CPU_MODEL_MAX_LENGTH + 1]; // Truncated model name
  unsigned physical_cores;                   // 0 when unknown
  unsigned logical_threads;                  // 0 when unknown
  bool model_valid;
};

struct host_metrics_state {
  struct host_cpu_ticks last_cpu;
  bool has_last_cpu;
  double cpu_percent; // Valid in [0, 100] when cpu_valid
  bool cpu_valid;
  struct host_memory_info memory;
  double memory_used_gib;
  double memory_total_gib;
  double memory_percent;
  bool memory_valid;
  bool supported;
  // Static identity of the CPU, read once and kept: it does not change while
  // the process runs.
  struct host_cpu_identity identity;
  bool identity_valid;
  bool identity_probed;
  // Average current frequency of the running cores, in megahertz.
  double cpu_freq_mhz;
  bool freq_valid;
  unsigned freq_probe_failures; // Stop polling sources that are not there
  // 1, 5 and 15 minute load averages.
  double load_avg[3];
  bool load_valid;
  struct host_swap_info swap;
  double swap_used_gib;
  double swap_total_gib;
  bool swap_valid;
  unsigned sample_count;
  double *history[host_metric_count];
  unsigned history_capacity;
  unsigned history_count[host_metric_count];
  unsigned history_next[host_metric_count];
};

// true if the host metrics can be collected on the current platform.
bool host_metrics_platform_supported(void);

// Parse the aggregate "cpu " line out of /proc/stat content.
bool host_cpu_parse_stat_text(const char *text, struct host_cpu_ticks *ticks);

// Utilization in percent between two samples. Returns false (and leaves
// *percent untouched) when no meaningful rate can be computed: missing input,
// no elapsed tick or decreasing counters (counter reset, cpu hotplug, ...).
bool host_cpu_utilization(const struct host_cpu_ticks *previous, const struct host_cpu_ticks *current,
                          double *percent);

// Extract MemTotal and MemAvailable (KiB) out of /proc/meminfo content.
bool host_memory_parse_meminfo_text(const char *text, struct host_memory_info *info);

// Convert memory information into display values. Returns false, and therefore
// reports the memory as unavailable, when the information cannot be trusted:
// no total at all, or MemAvailable larger than MemTotal.
bool host_memory_usage(const struct host_memory_info *info, double *used_gib, double *total_gib, double *percent);

// Parse the model out of a /proc/cpuinfo content and count the logical threads
// it declares. The model is the first of the "model name", "Model", "Hardware"
// and "Processor" fields, the way the architectures expose it. Returns false
// when neither a model nor a thread count could be read.
bool host_cpu_parse_cpuinfo_text(const char *text, struct host_cpu_identity *identity);

// Count the processors of a Linux processor list such as "0-3", "0,1,2" or
// "0-3,8-11", the format of the sysfs cpu lists. A list that would cover more
// processors than a machine can have is refused as malformed, and so is the
// "-" "every processor" shorthand, whose count cannot be known from the text.
// Returns false on malformed input.
bool host_cpu_parse_processor_list(const char *text, unsigned *count);

// Parse the three load averages of a /proc/loadavg content. The remaining
// fields of the line (running/total processes, last pid) are ignored.
bool host_loadavg_parse_text(const char *text, double load_avg[3]);

// Parse the kilohertz value of a cpufreq scaling_cur_freq or cpuinfo_cur_freq
// file into megahertz. A missing or zero frequency is not a frequency.
bool host_freq_parse_scaling_freq_text(const char *text, double *mhz);

// Average every "cpu MHz" field of a /proc/cpuinfo content. Returns false when
// the architecture does not report a frequency there.
bool host_cpuinfo_parse_freq_text(const char *text, double *average_mhz);

// Extract SwapTotal and SwapFree (KiB) out of a /proc/meminfo content. Both
// fields are required: without them the used swap cannot be known.
bool host_swap_parse_meminfo_text(const char *text, struct host_swap_info *info);

// Convert swap information into display values in GiB. Returns false, and
// therefore reports the swap as unavailable, when the information cannot be
// trusted: no swap at all, or more free swap than the machine has.
bool host_swap_usage(const struct host_swap_info *info, double *used_gib, double *total_gib);

// The CPU detail block displayed above the combined CPU/RAM chart, with the
// same density as a GPU detail block:
//   line 0: Device CPU [<model>]  CORES <physical>C/<logical>T
//   line 1: CPU  <util>%   FREQ <average>   LOAD <1m> / <5m> / <15m>
//   line 2: RAM  <used>/<total> GiB  <percent>%   AVAIL <avail>   SWAP <used>/<total> GiB
#define HOST_DETAIL_LINE_COUNT 3u

// Format one line of the CPU detail block for a block `width` columns wide.
// The fields that do not fit are dropped in the responsive order: the swap
// first, then the 5 and 15 minute load averages, then the available memory,
// then the static CPU information. The CPU utilization and the memory usage
// are never dropped and an overlong model name is truncated. Like snprintf, the
// return value is the length the line would have needed at its fullest.
unsigned host_metrics_format_detail_line(const struct host_metrics_state *state, unsigned line_index, unsigned width,
                                         char *buffer, size_t size);

// Allocate the history buffers. capacity is clamped to sane bounds.
bool host_metrics_init(struct host_metrics_state *state, unsigned capacity);

// Short name of a metric, used as the prefix of the chart line legend.
const char *host_metric_name(enum host_metric metric);

// Format the legend of the chart line of a metric: CPU utilization, or memory
// used and total in GiB with the utilization. The short form only gives the
// utilization, for the narrow charts where the detailed one does not fit. An
// unavailable metric says N/A: it is never shown as an idle zero. Like
// snprintf, the return value is the length the legend would have needed, so
// that the caller can tell whether the detailed form fitted the chart.
unsigned host_metrics_format_legend(const struct host_metrics_state *state, enum host_metric metric, bool short_form,
                                    char *buffer, size_t size);

void host_metrics_release(struct host_metrics_state *state);

// Read the platform files, update the current values and push the valid
// samples into the histories. Must be called at most once per interface
// refresh. Returns true if at least one metric is usable.
bool host_metrics_update(struct host_metrics_state *state);

// Read a single sample of the history of a metric, the way the interface reads
// the GPU samples out of its ring buffer. index_from_newest is 0 for the most
// recent sample. Returns false when there is no such sample: the metric was
// never available, or the history is shorter than the index. The caller must
// then draw nothing for that sample, and not a zero.
bool host_metrics_history_get(const struct host_metrics_state *state, enum host_metric metric,
                              unsigned index_from_newest, double *value);

// Process wide state used by the interface and the refresh loop. Lazily
// allocated, released by host_metrics_shutdown().
struct host_metrics_state *host_metrics_get_state(void);

void host_metrics_shutdown(void);

// Refresh the process wide state. Safe to call from the refresh loop.
bool host_metrics_refresh(void);

#endif // HOST_METRICS_H__
