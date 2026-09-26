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
