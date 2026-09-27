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

#include "hwdash/vendor/lenovo_cpu_fan.h"

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

// The current power drawn by the CPU package, either from a powercap (RAPL)
// energy counter, whose delta over the elapsed time is the power, or from a
// hwmon package power input, which is already a power. An energy counter needs
// two trustworthy samples to mean anything, so the power is reported only once
// that is the case: it is never guessed and never shown as a fabricated zero.
struct host_power_sample {
  uint64_t energy_uj;    // Cumulative package energy, in microjoules
  uint64_t max_range_uj; // Value the energy counter wraps at, in microjoules
};

// Largest power a CPU package can draw. Anything above it is not a package
// that worked that hard but a counter that restarted, a zone that changed
// under the sampling, or a malformed file.
#define HOST_POWER_MAX_PLAUSIBLE_WATTS 1000.

// Longest identity the probe gives a power source: a sysfs path.
#define HOST_POWER_SOURCE_MAX_LENGTH 256u

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
  // Power of the CPU package, in watts, valid only when power_valid: an
  // unknown power reads N/A in the detail block, never 0W.
  double package_power_watts;
  bool power_valid;
  // Vendor-specific host sensors are isolated from the generic host collector.
  struct hwdash_lenovo_cpu_fan lenovo_cpu_fan;
  // Previous sample of the package energy counter and the monotonic time it was
  // taken at: the pair the watts are computed from.
  struct host_power_sample last_power;
  // Monotonic clock, in nanoseconds, at which last_power was taken. A plain
  // count of nanoseconds rather than a clock type of the platform: this header
  // is included by the C++ test files too.
  uint64_t last_power_time_nsec;
  bool has_last_power;
  // Identity of the counter the previous sample came from. Another counter (a
  // module reload, a zone that disappeared and came back, a probe that settles
  // on another package) restarts the sampling: two samples of two different
  // counters make no power.
  char power_source[HOST_POWER_SOURCE_MAX_LENGTH];
  unsigned power_probe_failures; // Stop polling sources that are not there
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

// The package power helpers are pure, like the other parsers of this file, so
// that the counter semantics can be checked on a machine that has no such
// counter at all.

// Parse the content of a powercap energy_uj or max_energy_range_uj file: one
// number, optionally trailed by blanks. Anything else - a sign, a trailing
// text, an out of range value - is refused.
bool host_power_parse_energy_counter_text(const char *text, uint64_t *microjoules);

// Same as above for the energy counter itself, where a counter that reads zero
// reports no energy at all and is therefore not a sample to build a rate on.
bool host_power_parse_energy_uj_text(const char *text, uint64_t *energy_uj);

// Parse the content of a powercap zone `name` file, "package-0\n", into the
// name it declares, without the blanks around it.
bool host_power_parse_zone_name_text(const char *text, char *name, size_t size);

// true if a powercap zone name declares a whole CPU package: "package",
// "package-<n>", "pkg", "pkg-<n>", "soc", "soc-<n>" or the AMD package domain
// "amd". The sub zones of a package - core, dram, uncore, platform - are not a
// package and are refused, and so is a name whose package index is not a plain
// number. The case of the name does not matter.
bool host_power_zone_name_is_package(const char *name);

// Index a package zone name declares ("package-1" is the second package), 0
// when the name declares a package without saying which one.
bool host_power_zone_name_index(const char *name, unsigned *index);

// true if the label of an hwmon power input declares the package: a label that
// names the package, be it "Package", "Pkg" or "VDDPKG".
bool host_power_hwmon_label_is_package(const char *label);

// Watts drawn by the package between two samples of its energy counter: the
// energy delta over the elapsed monotonic time. The counter wrapping at its
// max energy range is handled; everything else that makes a delta untrustworthy
// is refused, and *watts left untouched: a first sample, an elapsed time of
// zero, a counter that did not move, a counter that decreased without a wrap to
// explain it (a reset, a zone that came back with another value), two samples
// of two different ranges, a counter outside its own range, and a power above
// HOST_POWER_MAX_PLAUSIBLE_WATTS. An unavailable power is reported as such by
// the caller, never as 0W.
bool host_power_watts_between(const struct host_power_sample *previous, const struct host_power_sample *current,
                              double elapsed_seconds, double *watts);

// Parse the content of an hwmon power*_input file, microwatts, into watts. A
// sensor that reads zero, or a negative or absurd value, reports nothing.
bool host_power_parse_hwmon_input_text(const char *text, double *watts);

// The CPU detail block displayed above the combined CPU/RAM chart, with the
// same density as a GPU detail block:
//   line 0: Device CPU [<model>]  CORES <physical>C/<logical>T
//   line 1: CPU  <util>%   FREQ <average>   LOAD <1m> / <5m> / <15m>   POWER <watts>W   Lenovo CPU Fan <rpm> RPM
//   line 2: RAM  <used>/<total> GiB  <percent>%   AVAIL <avail>   SWAP <used>/<total> GiB
#define HOST_DETAIL_LINE_COUNT 3u

// Format one line of the CPU detail block for a block `width` columns wide.
// The fields that do not fit are dropped in the responsive order: the swap
// first, then the 5 and 15 minute load averages, then the available memory,
// then the Lenovo fan speed, package power and static CPU information. The CPU utilization
// and the memory usage are never dropped and an overlong model name is
// truncated. Like snprintf, the return value is the length the line would have
// needed at its fullest.
unsigned host_metrics_format_detail_line(const struct host_metrics_state *state, unsigned line_index, unsigned width,
                                         char *buffer, size_t size);

// The title of a field of the detail block, and where it lies in the formatted
// line, so that the interface can color the titles the way the GPU detail
// blocks color theirs, and color nothing else. A title that the narrow
// formatter dropped, cut, or never wrote has no field for it.
struct host_detail_field {
  unsigned offset; // Column of the title in the line
  unsigned length; // Columns of the title
};

// Enough for the most crowded line of the block, with room to spare.
#define HOST_DETAIL_FIELD_MAX 6u

// Same as host_metrics_format_detail_line, and the placement of the field
// titles of that line on top, for up to max_fields of them, in line order. The
// fields are index aligned with the fields of the line as it was written: an
// overlong line is truncated and the titles past the truncation are not
// reported.
unsigned host_metrics_format_detail_line_fields(const struct host_metrics_state *state, unsigned line_index,
                                                unsigned width, char *buffer, size_t size,
                                                struct host_detail_field *fields, unsigned max_fields);

// Allocate the history buffers. capacity is clamped to sane bounds.
bool host_metrics_init(struct host_metrics_state *state, unsigned capacity);

// Short name of a metric, used as the prefix of the chart line legend.
const char *host_metric_name(enum host_metric metric);

// Legend key of the chart line of a metric: "CPU %" for the CPU utilization
// line, "RAM %" for the memory utilization line. A legend is a key that says
// which curve is which metric, not a readout: the current values, available or
// not, belong to the detail block above the chart, exactly like the GPU charts
// do.
const char *host_metric_legend_name(enum host_metric metric);

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
