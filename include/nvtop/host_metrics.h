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

#ifndef HOST_METRICS_H__
#define HOST_METRICS_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Fields of the aggregate "cpu" line of /proc/stat
typedef struct host_cpu_stat {
  unsigned long long user;
  unsigned long long nice;
  unsigned long long system;
  unsigned long long idle;
  unsigned long long iowait;
  unsigned long long irq;
  unsigned long long softirq;
  unsigned long long steal;
  unsigned long long guest;
  unsigned long long guest_nice;
} host_cpu_stat;

// Stateful sampler producing whole-host CPU utilization from /proc/stat samples
typedef struct host_cpu_sampler {
  host_cpu_stat prev;
  bool has_prev;
} host_cpu_sampler;

// Parse an aggregate cpu line of /proc/stat ("cpu  user nice system idle ...").
// Per-core lines ("cpu0", ...) and malformed lines are rejected: the four
// mandatory fields must be valid, optional trailing fields may be absent
// (treated as zero) but if present must be valid numbers, and no other
// trailing input is allowed.
bool host_metrics_parse_cpu_stat_line(const char *line, host_cpu_stat *out);

// Compute whole-host CPU utilization (percent, 0-100) between two samples.
// Busy time is the sum of the deltas of the first 8 counters (user, nice,
// system, irq, softirq, steal count as busy; idle and iowait count as
// idle). guest and guest_nice are virtual CPU time already accounted in
// user and nice, so they are not added again: guest execution is busy CPU
// time.
// Returns false when the data is unusable: a counter decreased between
// samples, the sums overflow 64 bits, zero ticks elapsed, or the inputs
// are invalid.
bool host_metrics_cpu_utilization_percent(const host_cpu_stat *prev, const host_cpu_stat *curr,
                                          double *out_percent);

// Feed a new sample into the sampler. The first call only primes the sampler
// and reports no data; subsequent calls report the utilization delta.
// The baseline is always advanced to the newest sample: when a delta cannot
// be computed (e.g. counters were reset or decreased) the sampler re-primes
// on the bad sample, so the following sample recovers a valid delta.
bool host_metrics_cpu_sampler_update(host_cpu_sampler *sampler, const host_cpu_stat *curr, double *out_percent);

// Read the aggregate cpu sample from a /proc/stat-like file
bool host_metrics_read_cpu_stat(const char *path, host_cpu_stat *out);

// Fields parsed from /proc/meminfo
typedef struct host_meminfo {
  bool has_total;
  bool has_available;
  unsigned long long total_kb;
  unsigned long long available_kb;
} host_meminfo;

// Parse a single "Name: <value> kB" line of /proc/meminfo.
// Returns false when the line does not start with <name>":" or the value
// is missing, malformed, overflowing, or not expressed in kB.
bool host_metrics_parse_meminfo_value(const char *line, const char *name, unsigned long long *out_kb);

// Parse the NUL-terminated content of /proc/meminfo.
// Fields that are absent remain flagged as such (has_total/has_available).
bool host_metrics_parse_meminfo(const char *content, host_meminfo *out);

// Read and parse a /proc/meminfo-like file
bool host_metrics_read_meminfo(const char *path, host_meminfo *out);

// Compute RAM usage from parsed /proc/meminfo data: used = MemTotal - MemAvailable.
// Returns false (data unavailable) when MemTotal or MemAvailable is missing,
// MemTotal is zero, or MemAvailable exceeds MemTotal (underflow).
bool host_metrics_ram_usage(const host_meminfo *info, double *out_used_gib, double *out_total_gib,
                            double *out_percent);

// True when the platform exposes /proc-based host metrics (currently Linux).
// On other platforms the host panels are unsupported and must not be sampled.
bool host_metrics_platform_supported(void);

#endif // HOST_METRICS_H__
