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

#include "nvtop/host_metrics.h"

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nvtop/time.h" // The monotonic clock the power is a rate over

// HOST_METRICS_FORCE_PROC only exists to compile check the /proc code paths on
// a platform that has no /proc filesystem.
#if defined(__linux__) || defined(HOST_METRICS_FORCE_PROC)
#define HOST_METRICS_LINUX 1
#include <dirent.h> // The powercap and hwmon directories are scanned, not guessed
#include <sys/stat.h>
#endif

#define HOST_METRICS_ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define HOST_METRICS_HISTORY_MIN_CAPACITY 16u
#define HOST_METRICS_HISTORY_MAX_CAPACITY 4096u

#ifdef HOST_METRICS_LINUX
#define HOST_PROC_STAT_PATH "/proc/stat"
#define HOST_PROC_MEMINFO_PATH "/proc/meminfo"
#define HOST_PROC_CPUINFO_PATH "/proc/cpuinfo"
#define HOST_PROC_LOADAVG_PATH "/proc/loadavg"
#define HOST_SYSFS_SCALING_FREQ_PATH "/sys/devices/system/cpu/cpu%u/cpufreq/scaling_cur_freq"
// The aggregate cpu line and the Mem* entries are always near the top of the
// files, reading more would be wasteful for a periodic poll.
#define HOST_PROC_FILE_READ_LIMIT 4096u
// The Swap* entries sit below the Mem* entries of /proc/meminfo: reading a bit
// more than the memory block is what it takes to get both in a single read.
#define HOST_PROC_MEMINFO_READ_LIMIT 8192u
// /proc/cpuinfo grows with the number of logical threads: a whole core listing
// is close to a kilobyte, so the read limit follows.
#define HOST_PROC_CPUINFO_READ_LIMIT 262144u
#define HOST_PROC_SMALL_FILE_READ_LIMIT 256u
#endif

// The sysfs topology paths, used by the identity probe on the platforms that
// have a sysfs and left unused on the others.
#define HOST_SYSFS_CPU_ONLINE_PATH "/sys/devices/system/cpu/online"
#define HOST_SYSFS_THREAD_SIBLINGS_PATH "/sys/devices/system/cpu/cpu0/topology/thread_siblings_list"

#define HOST_MAX_CPUS 4096u
#define HOST_PROBED_CPUS_WHEN_UNKNOWN 64u
// Polling a frequency source that is not there at all is useless: give up
// after a few attempts and try again from time to time, in case a driver shows
// up later (a module load, a cpu hotplug, ...).
#define HOST_FREQ_PROBE_RETRY_PERIOD 256u
#define HOST_FREQ_PROBE_GIVE_UP_AFTER 4u

#define HOST_LENOVO_FAN_MAX_PLAUSIBLE_RPM 30000u
#define HOST_LENOVO_FAN_PROBE_GIVE_UP_AFTER 4u
#define HOST_LENOVO_FAN_PROBE_RETRY_PERIOD 64u
#define HOST_LENOVO_FAN_HELPER_PATH "/run/nvtop/lenovo-cpu-fan-rpm"
#define HOST_LENOVO_FAN_HELPER_MAX_AGE_SECONDS 5
#define HOST_DMI_SYS_VENDOR_PATH "/sys/class/dmi/id/sys_vendor"

#define HOST_KIB_PER_GIB (1024. * 1024.)

static double kib_to_gib(uint64_t kib) { return (double)kib / HOST_KIB_PER_GIB; }

static const char *skip_blanks(const char *text);
static bool parse_u64_token(const char **cursor, uint64_t *value);

bool host_metrics_platform_supported(void) {
#ifdef HOST_METRICS_LINUX
  return true;
#else
  return false;
#endif
}

bool host_lenovo_fan_parse_rpm_text(const char *text, unsigned *rpm) {
  if (!text || !rpm)
    return false;
  const char *cursor = text;
  uint64_t parsed = 0;
  if (!parse_u64_token(&cursor, &parsed))
    return false;
  if (*skip_blanks(cursor) != '\0' || parsed > HOST_LENOVO_FAN_MAX_PLAUSIBLE_RPM)
    return false;
  *rpm = (unsigned)parsed;
  return true;
}

static const char *skip_spaces(const char *text) {
  while (*text == ' ' || *text == '\t')
    ++text;
  return text;
}

static const char *skip_blanks(const char *text) {
  while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')
    ++text;
  return text;
}

// Parse one unsigned integer token. Rejects signs and out of range values so
// that a malformed file never yields a plausible but bogus counter.
static bool parse_u64_token(const char **cursor, uint64_t *value) {
  const char *text = skip_blanks(*cursor);
  if (*text < '0' || *text > '9')
    return false;
  char *end = NULL;
  errno = 0;
  unsigned long long parsed = strtoull(text, &end, 10);
  if (end == text || errno == ERANGE || parsed == 0xFFFFFFFFFFFFFFFFull)
    return false;
  *value = (uint64_t)parsed;
  *cursor = end;
  return true;
}

static bool checked_add(uint64_t *accumulator, uint64_t value) {
  if (value > 0xFFFFFFFFFFFFFFFFull - *accumulator)
    return false;
  *accumulator += value;
  return true;
}

// Move the cursor to the next line of a multi line buffer.
static const char *next_line(const char *line) {
  const char *eol = strchr(line, '\n');
  if (!eol)
    return NULL;
  return eol + 1;
}

static bool parse_cpu_fields(const char *cursor, struct host_cpu_ticks *ticks) {
  uint64_t field[10] = {0};
  unsigned parsed_fields = 0;
  while (parsed_fields < HOST_METRICS_ARRAY_SIZE(field)) {
    const char *next = cursor;
    if (!parse_u64_token(&next, &field[parsed_fields]))
      break;
    cursor = next;
    ++parsed_fields;
  }
  // user nice system idle are required, iowait appeared with 2.5.41 and the
  // remaining fields later: missing fields are accounted as zero ticks.
  if (parsed_fields < 4)
    return false;
  // Trailing garbage (an unexpected token in the middle of the counters) is a
  // malformed input, unknown extra fields are simply ignored.
  if (parsed_fields < HOST_METRICS_ARRAY_SIZE(field)) {
    const char *rest = skip_spaces(cursor);
    if (*rest != '\0' && *rest != '\n' && *rest != '\r')
      return false;
  }

  uint64_t total = 0, idle = 0;
  // guest (index 8) and guest_nice (index 9) are excluded: they are already
  // accounted for respectively in user (0) and nice (1).
  for (unsigned i = 0; i < parsed_fields && i < 8; ++i) {
    if (!checked_add(&total, field[i]))
      return false;
  }
  if (!checked_add(&idle, field[3]))
    return false;
  if (parsed_fields >= 5 && !checked_add(&idle, field[4]))
    return false;
  if (idle > total)
    return false;

  ticks->total = total;
  ticks->idle = idle;
  return true;
}

bool host_cpu_parse_stat_text(const char *text, struct host_cpu_ticks *ticks) {
  if (!text || !ticks)
    return false;
  *ticks = (struct host_cpu_ticks){0, 0};
  for (const char *line = text; line && *line; line = next_line(line)) {
    const char *name = skip_blanks(line);
    // The aggregate line is "cpu ", the per cpu lines are "cpu0 ", "cpu1 ", ...
    if (name[0] != 'c' || name[1] != 'p' || name[2] != 'u' || (name[3] != ' ' && name[3] != '\t'))
      continue;
    return parse_cpu_fields(name + 3, ticks);
  }
  return false;
}

bool host_cpu_utilization(const struct host_cpu_ticks *previous, const struct host_cpu_ticks *current,
                          double *percent) {
  if (!previous || !current || !percent)
    return false;
  if (current->idle > current->total || previous->idle > previous->total)
    return false;
  // No elapsed tick, or counters going backward (counter reset, cpu hotplug,
  // container namespace change): the rate cannot be trusted.
  if (current->total <= previous->total || current->idle < previous->idle)
    return false;

  uint64_t total_delta = current->total - previous->total;
  uint64_t idle_delta = current->idle - previous->idle;
  if (idle_delta > total_delta)
    return false;
  uint64_t busy_delta = total_delta - idle_delta;
  double utilization = 100. * (double)busy_delta / (double)total_delta;
  if (utilization < 0.)
    utilization = 0.;
  if (utilization > 100.)
    utilization = 100.;
  *percent = utilization;
  return true;
}

// Parse "<value> kB" as used by the Mem* entries of /proc/meminfo.
static bool parse_meminfo_value(const char **cursor, uint64_t *value_kib) {
  const char *text = *cursor;
  uint64_t value;
  if (!parse_u64_token(&text, &value))
    return false;
  text = skip_spaces(text);
  // Every Mem* entry of /proc/meminfo is expressed in kilobytes. An unexpected
  // unit means we cannot convert the value safely.
  if (text[0] != 'k' || text[1] != 'B')
    return false;
  text = skip_spaces(text + 2);
  if (*text != '\0' && *text != '\n' && *text != '\r')
    return false;
  *value_kib = value;
  *cursor = text;
  return true;
}

static bool line_starts_with(const char *line, const char *prefix, const char **rest) {
  const char *name = skip_spaces(line);
  size_t length = strlen(prefix);
  if (strncmp(name, prefix, length) != 0)
    return false;
  *rest = name + length;
  return true;
}

bool host_memory_parse_meminfo_text(const char *text, struct host_memory_info *info) {
  if (!text || !info)
    return false;
  *info = (struct host_memory_info){0, 0};
  uint64_t total_kib = 0, available_kib = 0;
  bool found_total = false, found_available = false;
  for (const char *line = text; line && *line; line = next_line(line)) {
    const char *rest = NULL;
    if (!found_total && line_starts_with(line, "MemTotal:", &rest)) {
      if (!parse_meminfo_value(&rest, &total_kib))
        return false;
      found_total = true;
      continue;
    }
    if (!found_available && line_starts_with(line, "MemAvailable:", &rest)) {
      if (!parse_meminfo_value(&rest, &available_kib))
        return false;
      found_available = true;
    }
  }
  // MemAvailable is required: falling back to MemFree would badly overstate
  // the used memory by ignoring the reclaimable page cache.
  if (!found_total || !found_available)
    return false;
  if (total_kib == 0)
    return false;
  info->total_kib = total_kib;
  info->available_kib = available_kib;
  return true;
}

bool host_memory_usage(const struct host_memory_info *info, double *used_gib, double *total_gib, double *percent) {
  if (!info || !used_gib || !total_gib || !percent)
    return false;
  if (info->total_kib == 0)
    return false;
  // More available memory than the machine has is inconsistent: report the
  // memory as unavailable rather than a plausible but bogus 0% usage.
  if (info->available_kib > info->total_kib)
    return false;
  uint64_t used_kib = info->total_kib - info->available_kib;
  *used_gib = (double)used_kib / (1024. * 1024.);
  *total_gib = (double)info->total_kib / (1024. * 1024.);
  double usage = 100. * (double)used_kib / (double)info->total_kib;
  if (usage < 0.)
    usage = 0.;
  if (usage > 100.)
    usage = 100.;
  *percent = usage;
  return true;
}

bool host_swap_parse_meminfo_text(const char *text, struct host_swap_info *info) {
  if (!text || !info)
    return false;
  *info = (struct host_swap_info){0, 0};
  uint64_t total_kib = 0, free_kib = 0;
  bool found_total = false, found_free = false;
  for (const char *line = text; line && *line; line = next_line(line)) {
    const char *rest = NULL;
    if (!found_total && line_starts_with(line, "SwapTotal:", &rest)) {
      if (!parse_meminfo_value(&rest, &total_kib))
        return false;
      found_total = true;
      continue;
    }
    if (!found_free && line_starts_with(line, "SwapFree:", &rest)) {
      if (!parse_meminfo_value(&rest, &free_kib))
        return false;
      found_free = true;
    }
  }
  // Both fields are required: an unknown SwapFree would pretend a full swap is
  // free, and a machine without any swap has nothing to report.
  if (!found_total || !found_free)
    return false;
  if (total_kib == 0)
    return false;
  info->total_kib = total_kib;
  info->free_kib = free_kib;
  return true;
}

bool host_swap_usage(const struct host_swap_info *info, double *used_gib, double *total_gib) {
  if (!info || !used_gib || !total_gib)
    return false;
  if (info->total_kib == 0)
    return false;
  // More free swap than the machine has is inconsistent: report the swap as
  // unavailable rather than a plausible but bogus usage.
  if (info->free_kib > info->total_kib)
    return false;
  *used_gib = kib_to_gib(info->total_kib - info->free_kib);
  *total_gib = kib_to_gib(info->total_kib);
  return true;
}


// The package power helpers /////////////////////////////////////////
// Everything below is pure: it turns the text of a sysfs file, or a pair of
// energy samples, into a power, or into the refusal to do so. That is what
// makes the counter semantics testable on a machine whose kernel exposes no
// energy counter to a normal user.

// A powercap or hwmon value file holds a single number, optionally trailed by
// blanks. Trailing text means the file is not what it claims to be.
static bool parse_single_counter_text(const char *text, uint64_t *value) {
  if (!text || !value)
    return false;
  const char *cursor = text;
  uint64_t parsed = 0;
  if (!parse_u64_token(&cursor, &parsed))
    return false;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\n' || *cursor == '\r')
    ++cursor;
  if (*cursor != '\0')
    return false;
  *value = parsed;
  return true;
}

bool host_power_parse_energy_counter_text(const char *text, uint64_t *microjoules) {
  return parse_single_counter_text(text, microjoules);
}

bool host_power_parse_energy_uj_text(const char *text, uint64_t *energy_uj) {
  if (!energy_uj)
    return false;
  uint64_t counter = 0;
  // A counter that reads zero reports no energy: it is not a sample a rate
  // could be computed from, and taking it for one would show a 0W package.
  if (!parse_single_counter_text(text, &counter) || counter == 0)
    return false;
  *energy_uj = counter;
  return true;
}

bool host_power_parse_zone_name_text(const char *text, char *name, size_t size) {
  if (!text || !name || size == 0)
    return false;
  const char *begin = skip_blanks(text);
  const char *end = begin;
  while (*end != '\0' && *end != ' ' && *end != '\t' && *end != '\n' && *end != '\r')
    ++end;
  if (end == begin)
    return false;
  size_t length = (size_t)(end - begin);
  // A name longer than the buffer is not a package name the probe could settle
  // on, but keeping its head is harmless: it will simply not match.
  if (length > size - 1)
    length = size - 1;
  memcpy(name, begin, length);
  name[length] = '\0';
  return true;
}

// A copy of a name in lower case, so that the kernel writes what it likes: the
// zone and label names are not consistently cased across drivers.
static bool copy_lower_case(char *destination, size_t size, const char *text) {
  if (!text || !destination || size == 0)
    return false;
  size_t i = 0;
  for (; text[i] != '\0'; ++i) {
    if (i + 1 >= size)
      return false;
    char c = text[i];
    if (c >= 'A' && c <= 'Z')
      c = (char)(c - 'A' + 'a');
    destination[i] = c;
  }
  destination[i] = '\0';
  return true;
}

// "name", "name-<digits>" and nothing else. The index is what tells the
// packages of a multi socket machine apart.
static bool match_indexed_name(const char *name, const char *prefix, unsigned *index) {
  size_t prefix_length = strlen(prefix);
  if (strncmp(name, prefix, prefix_length) != 0)
    return false;
  const char *rest = name + prefix_length;
  if (*rest == '\0') {
    if (index)
      *index = 0;
    return true;
  }
  if (*rest != '-')
    return false;
  ++rest;
  if (*rest == '\0')
    return false;
  unsigned value = 0;
  for (const char *c = rest; *c != '\0'; ++c) {
    if (*c < '0' || *c > '9')
      return false;
    // A machine with more packages than that has other problems: refuse.
    if (value > 65535u)
      return false;
    value = value * 10u + (unsigned)(*c - '0');
  }
  if (index)
    *index = value;
  return true;
}

bool host_power_zone_name_index(const char *name, unsigned *index) {
  if (!name || !index)
    return false;
  char lower[HOST_POWER_SOURCE_MAX_LENGTH];
  if (!copy_lower_case(lower, sizeof(lower), name))
    return false;
  // The names the powercap drivers use for a whole package. The sub zones of a
  // package (core, dram, uncore, platform) deliberately are not here: taking
  // one of them for the package would report a part of the CPU as being all of
  // it. "amd" is the AMD package domain, as the others are the Intel ones.
  static const char *const package_names[] = {"package", "pkg", "soc", "amd"};
  for (unsigned i = 0; i < HOST_METRICS_ARRAY_SIZE(package_names); ++i) {
    if (match_indexed_name(lower, package_names[i], index))
      return true;
  }
  return false;
}

bool host_power_zone_name_is_package(const char *name) {
  unsigned index = 0;
  return host_power_zone_name_index(name, &index);
}

bool host_power_hwmon_label_is_package(const char *label) {
  if (!label)
    return false;
  char lower[HOST_POWER_SOURCE_MAX_LENGTH];
  if (!copy_lower_case(lower, sizeof(lower), label))
    return false;
  // The labels naming the package power of the boards that have one: "Package",
  // "Pkg", "VDDPKG", and the compound ones built on them.
  return strstr(lower, "package") != NULL || strstr(lower, "pkg") != NULL;
}

bool host_power_watts_between(const struct host_power_sample *previous, const struct host_power_sample *current,
                              double elapsed_seconds, double *watts) {
  if (!previous || !current || !watts)
    return false;
  // No elapsed time, or a time that is not a time, is no rate at all.
  if (!isfinite(elapsed_seconds) || elapsed_seconds <= 0.)
    return false;
  if (previous->energy_uj == 0 || current->energy_uj == 0)
    return false;
  // Without the range there is no way to tell a wrap from a reset, and a reset
  // would be read as an enormous burst of energy.
  if (previous->max_range_uj == 0 || current->max_range_uj == 0)
    return false;
  if (previous->max_range_uj != current->max_range_uj)
    return false;
  const uint64_t range = current->max_range_uj;
  if (previous->energy_uj > range || current->energy_uj > range)
    return false;

  uint64_t delta;
  if (current->energy_uj >= previous->energy_uj) {
    delta = current->energy_uj - previous->energy_uj;
  } else {
    // The counter ran past the max energy range and started over: what it was
    // short of the range, plus what it counted again, is the energy used.
    uint64_t tail = range - previous->energy_uj;
    if (current->energy_uj > 0xFFFFFFFFFFFFFFFFull - tail)
      return false;
    delta = tail + current->energy_uj;
    // A decrease no wrap explains: the counter restarted, the zone is another
    // one, the module was reloaded. Nothing trustworthy to report.
    if (delta > range)
      return false;
  }
  // A counter that did not move during the interval reports nothing, and a
  // power computed from it would be a made up zero.
  if (delta == 0)
    return false;

  double value = ((double)delta / 1000000.) / elapsed_seconds;
  if (!isfinite(value) || value <= 0. || value > HOST_POWER_MAX_PLAUSIBLE_WATTS)
    return false;
  *watts = value;
  return true;
}

bool host_power_parse_hwmon_input_text(const char *text, double *watts) {
  if (!watts)
    return false;
  uint64_t microwatts = 0;
  // A sensor reading zero reports nothing: an idle package still burns power.
  if (!parse_single_counter_text(text, &microwatts) || microwatts == 0)
    return false;
  double value = (double)microwatts / 1000000.;
  if (!isfinite(value) || value > HOST_POWER_MAX_PLAUSIBLE_WATTS)
    return false;
  *watts = value;
  return true;
}

// Split a "<field> : <value>" line the way /proc/cpuinfo writes them. The
// field name is trimmed of its trailing blanks, the value of both sides.
static bool split_colon_line(const char *line, const char **name, const char **name_end, const char **value,
                             const char **value_end) {
  const char *text = skip_blanks(line);
  const char *colon = strchr(text, ':');
  if (!colon)
    return false;
  const char *field_end = colon;
  while (field_end > text && (field_end[-1] == ' ' || field_end[-1] == '\t'))
    --field_end;
  const char *begin = skip_spaces(colon + 1);
  const char *stop = begin;
  while (*stop != '\0' && *stop != '\n' && *stop != '\r')
    ++stop;
  while (stop > begin && (stop[-1] == ' ' || stop[-1] == '\t'))
    --stop;
  *name = text;
  if (name_end)
    *name_end = field_end;
  *value = begin;
  *value_end = stop;
  return true;
}

static bool field_name_is(const char *name, const char *name_end, const char *expected) {
  size_t length = strlen(expected);
  if ((size_t)(name_end - name) != length)
    return false;
  return strncmp(name, expected, length) == 0;
}

// Copy a trimmed field, truncating it to what fits: a model name is displayed
// as it is, an unreadably long one is cut rather than overflowing the terminal.
static bool copy_field(char *destination, size_t size, const char *begin, const char *end) {
  if (begin >= end)
    return false;
  size_t length = (size_t)(end - begin);
  if (length >= size)
    length = size - 1;
  memcpy(destination, begin, length);
  destination[length] = '\0';
  return length > 0;
}

// Parse a fixed point number, such as a load average or a cpu megahertz.
static bool parse_double_token(const char **cursor, double *value) {
  const char *text = skip_blanks(*cursor);
  if ((*text < '0' || *text > '9') && *text != '.')
    return false;
  char *end = NULL;
  errno = 0;
  double parsed = strtod(text, &end);
  if (end == text || errno == ERANGE || !isfinite(parsed) || parsed < 0.)
    return false;
  *value = parsed;
  *cursor = end;
  return true;
}

bool host_loadavg_parse_text(const char *text, double load_avg[3]) {
  if (!text || !load_avg)
    return false;
  const char *cursor = text;
  double parsed[3] = {0., 0., 0.};
  for (unsigned i = 0; i < 3; ++i) {
    if (!parse_double_token(&cursor, &parsed[i]))
      return false;
    // The three averages are separated by blanks; anything else is a line we
    // do not understand.
    if (i < 2 && cursor[0] != ' ' && cursor[0] != '\t')
      return false;
  }
  for (unsigned i = 0; i < 3; ++i)
    load_avg[i] = parsed[i];
  return true;
}

bool host_freq_parse_scaling_freq_text(const char *text, double *mhz) {
  if (!text || !mhz)
    return false;
  const char *cursor = text;
  uint64_t khz = 0;
  if (!parse_u64_token(&cursor, &khz) || khz == 0)
    return false;
  const char *rest = skip_blanks(cursor);
  if (*rest != '\0' && *rest != '\n' && *rest != '\r')
    return false;
  // A frequency is reported in kilohertz, and no processor runs below a
  // megahertz: a smaller value is a malformed file.
  if (khz < 1000u)
    return false;
  *mhz = (double)khz / 1000.;
  return true;
}

bool host_cpuinfo_parse_freq_text(const char *text, double *average_mhz) {
  if (!text || !average_mhz)
    return false;
  double sum = 0.;
  unsigned count = 0;
  for (const char *line = text; line && *line; line = next_line(line)) {
    const char *name = NULL, *name_end = NULL, *value = NULL, *value_end = NULL;
    if (!split_colon_line(line, &name, &name_end, &value, &value_end))
      continue;
    if (!field_name_is(name, name_end, "cpu MHz"))
      continue;
    const char *cursor = value;
    double mhz = 0.;
    if (!parse_double_token(&cursor, &mhz) || mhz <= 0.)
      return false;
    sum += mhz;
    ++count;
  }
  if (count == 0)
    return false;
  *average_mhz = sum / (double)count;
  return true;
}

bool host_cpu_parse_processor_list(const char *text, unsigned *count) {
  if (!text || !count)
    return false;
  const char *cursor = skip_blanks(text);
  if (*cursor == '\0' || *cursor == '\n' || *cursor == '\r')
    return false;
  unsigned total = 0;
  bool at_least_one = false;
  while (true) {
    cursor = skip_blanks(cursor);
    uint64_t first = 0, last = 0;
    const char *next = cursor;
    if (!parse_u64_token(&next, &first))
      return false;
    next = skip_blanks(next);
    last = first;
    if (*next == '-') {
      next = skip_blanks(next + 1);
      if (!parse_u64_token(&next, &last))
        return false;
      if (last < first || last - first >= HOST_MAX_CPUS)
        return false;
    }
    if (first >= HOST_MAX_CPUS || last >= HOST_MAX_CPUS)
      return false;
    unsigned range_count = (unsigned)(last - first + 1);
    if (total > HOST_MAX_CPUS - range_count)
      return false;
    total += range_count;
    at_least_one = true;
    cursor = skip_blanks(next);
    if (*cursor == ',') {
      ++cursor;
      continue;
    }
    break;
  }
  if (*cursor != '\0' && *cursor != '\n' && *cursor != '\r')
    return false;
  if (!at_least_one || total == 0)
    return false;
  *count = total;
  return true;
}

bool host_cpu_parse_cpuinfo_text(const char *text, struct host_cpu_identity *identity) {
  if (!text || !identity)
    return false;
  memset(identity, 0, sizeof(*identity));
  // The architectures report the model under different field names. The first
  // one that is there wins, the way the common tools do.
  static const char *const model_fields[] = {"model name", "Model", "Hardware", "Processor"};
  unsigned processor_count = 0;
  uint64_t siblings = 0, cores_per_package = 0;
  for (const char *line = text; line && *line; line = next_line(line)) {
    const char *name = NULL, *name_end = NULL, *value = NULL, *value_end = NULL;
    if (!split_colon_line(line, &name, &name_end, &value, &value_end))
      continue;
    if (!identity->model_valid) {
      for (unsigned i = 0; i < HOST_METRICS_ARRAY_SIZE(model_fields); ++i) {
        if (field_name_is(name, name_end, model_fields[i]) &&
            copy_field(identity->model, sizeof(identity->model), value, value_end)) {
          identity->model_valid = true;
          break;
        }
      }
    }
    if (field_name_is(name, name_end, "processor")) {
      // A thread entry is a processor with a number: an empty or malformed
      // field is not a thread, it is a line we do not understand.
      const char *number = value;
      uint64_t processor_id = 0;
      if (parse_u64_token(&number, &processor_id))
        ++processor_count;
    } else if (siblings == 0 && field_name_is(name, name_end, "siblings"))
      (void)parse_u64_token(&value, &siblings);
    else if (cores_per_package == 0 && field_name_is(name, name_end, "cpu cores"))
      (void)parse_u64_token(&value, &cores_per_package);
  }
  identity->logical_threads = processor_count;
  // x86 /proc/cpuinfo also lists how many threads a package holds and how many
  // cores it has: the physical core count follows from the two.
  if (siblings > 0 && cores_per_package > 0 && processor_count > 0 && processor_count % siblings == 0)
    identity->physical_cores = (unsigned)((processor_count / siblings) * cores_per_package);
  return identity->model_valid || identity->logical_threads > 0;
}

static void history_push(struct host_metrics_state *state, enum host_metric metric, double value) {
  if (metric >= host_metric_count || !state->history[metric] || state->history_capacity == 0)
    return;
  state->history[metric][state->history_next[metric]] = value;
  state->history_next[metric] = (state->history_next[metric] + 1) % state->history_capacity;
  if (state->history_count[metric] < state->history_capacity)
    ++state->history_count[metric];
}

bool host_metrics_init(struct host_metrics_state *state, unsigned capacity) {
  if (!state)
    return false;
  memset(state, 0, sizeof(*state));
  if (capacity < HOST_METRICS_HISTORY_MIN_CAPACITY)
    capacity = HOST_METRICS_HISTORY_MIN_CAPACITY;
  if (capacity > HOST_METRICS_HISTORY_MAX_CAPACITY)
    capacity = HOST_METRICS_HISTORY_MAX_CAPACITY;
  for (unsigned i = 0; i < host_metric_count; ++i) {
    state->history[i] = calloc(capacity, sizeof(**state->history));
    if (!state->history[i]) {
      host_metrics_release(state);
      return false;
    }
  }
  state->history_capacity = capacity;
  state->supported = host_metrics_platform_supported();
  return true;
}

void host_metrics_release(struct host_metrics_state *state) {
  if (!state)
    return;
  for (unsigned i = 0; i < host_metric_count; ++i) {
    free(state->history[i]);
    state->history[i] = NULL;
  }
  state->history_capacity = 0;
  state->has_last_cpu = false;
  state->cpu_valid = false;
  state->memory_valid = false;
  state->freq_valid = false;
  state->load_valid = false;
  state->swap_valid = false;
  state->identity_valid = false;
  state->identity_probed = false;
  state->power_valid = false;
  state->lenovo_fan_valid = false;
  state->lenovo_fan_supported = false;
  state->lenovo_fan_support_probed = false;
  state->lenovo_fan_probe_failures = 0u;
  state->has_last_power = false;
  state->power_source[0] = '\0';
  state->power_probe_failures = 0u;
}

bool host_metrics_history_get(const struct host_metrics_state *state, enum host_metric metric,
                              unsigned index_from_newest, double *value) {
  if (!state || !value || metric >= host_metric_count || !state->history[metric])
    return false;
  if (index_from_newest >= state->history_count[metric])
    return false;
  unsigned newest = (state->history_next[metric] + state->history_capacity - 1) % state->history_capacity;
  unsigned index = (newest + state->history_capacity - index_from_newest) % state->history_capacity;
  *value = state->history[metric][index];
  return true;
}

const char *host_metric_name(enum host_metric metric) {
  switch (metric) {
  case host_metric_cpu:
    return "CPU";
  case host_metric_memory:
    return "RAM";
  case host_metric_count:
    break;
  }
  return "Host";
}

const char *host_metric_legend_name(enum host_metric metric) {
  // A legend says which curve is which metric and nothing else: the current
  // value, available or not, belongs to the detail block above the chart.
  switch (metric) {
  case host_metric_cpu:
    return "CPU %";
  case host_metric_memory:
    return "RAM %";
  case host_metric_count:
    break;
  }
  return "Host %";
}

// The CPU detail block ///////////////////////////////////////////////
// It shows the same sampled values as the combined chart below it, plus the
// information that does not have a chart line of its own: what this CPU is,
// how fast it currently runs, how loaded the machine is and how much swap is
// used, and how much power the package burns. Everything that is not known
// reads N/A.

// The narrower the terminal, the sooner a field is dropped. The fields that
// carry the utilization are never dropped: a detail block that would only hold
// them is still a useful detail block.
enum host_detail_priority {
  host_detail_priority_always = 100,
  host_detail_priority_static = 60, // Model, core count, frequency, load 1 minute
  host_detail_priority_power = 45,
  host_detail_priority_lenovo_fan = 44,
  host_detail_priority_available = 40,
  host_detail_priority_load_5m = 30,
  host_detail_priority_load_15m = 25,
  host_detail_priority_swap = 20,
};

#define HOST_DETAIL_SEGMENT_SIZE 96u
#define HOST_DETAIL_LINE_SIZE 512u
// A cut off field keeps that many characters, marker included: below that it
// identifies nothing and it is dropped instead.
#define HOST_DETAIL_SHRINK_MIN_LENGTH 8u

struct host_detail_segment {
  char text[HOST_DETAIL_SEGMENT_SIZE];
  const char *title; // Field title inside text, drawn in the title color
  unsigned priority;
  bool droppable;
  bool kept;
  bool shrinkable; // An overlong field that is cut rather than dropped
};

static void detail_segment(struct host_detail_segment *segment, unsigned priority, bool droppable, const char *title,
                           const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  vsnprintf(segment->text, sizeof(segment->text), format, arguments);
  va_end(arguments);
  segment->title = title;
  segment->priority = priority;
  segment->droppable = droppable;
  segment->shrinkable = false;
  segment->kept = true;
}

// Where the title of a segment lies in its text. A title is always part of the
// head of its segment, after the blanks that separate it from the previous
// field, so a value that happens to contain the title text cannot be mistaken
// for the title itself.
static bool segment_title_span(const char *text, const char *title, unsigned *offset, unsigned *length) {
  if (!title)
    return false;
  const char *title_at = strstr(text, title);
  if (!title_at)
    return false;
  *offset = (unsigned)(title_at - text);
  *length = (unsigned)strlen(title);
  return true;
}

// Concatenate the kept segments, tell how wide the line would be, and say where
// each field title ended up in the line that was written.
static unsigned detail_render_line(char *line, size_t line_size, const struct host_detail_segment *segments,
                                   unsigned count, struct host_detail_field *fields, unsigned max_fields) {
  if (fields) {
    for (unsigned i = 0; i < max_fields; ++i) {
      fields[i].offset = 0;
      fields[i].length = 0;
    }
  }
  size_t written = 0;
  unsigned needed = 0;
  unsigned field_count = 0;
  for (unsigned i = 0; i < count; ++i) {
    if (!segments[i].kept)
      continue;
    size_t length = strlen(segments[i].text);
    unsigned title_offset = 0, title_length = 0;
    const bool has_title = segment_title_span(segments[i].text, segments[i].title, &title_offset, &title_length);
    if (line && written + 1 < line_size) {
      size_t room = line_size - 1 - written;
      size_t copied = length < room ? length : room;
      memcpy(line + written, segments[i].text, copied);
      // A title is reported when the whole of it made it into the line: a title
      // that the narrow formatter cut, or that never fitted at all, has nothing
      // to color.
      if (has_title && fields && field_count < max_fields &&
          (size_t)title_offset + (size_t)title_length <= copied) {
        fields[field_count].offset = (unsigned)written + title_offset;
        fields[field_count].length = title_length;
        ++field_count;
      }
      written += copied;
    } else {
      written = line_size - 1;
    }
    needed += (unsigned)length;
  }
  if (line && line_size > 0)
    line[written > line_size - 1 ? line_size - 1 : written] = '\0';
  return needed;
}

static unsigned format_detail_line(const struct host_metrics_state *state, unsigned line_index, unsigned width,
                                   char *buffer, size_t size, struct host_detail_field *fields,
                                   unsigned max_fields) {
  if (!buffer || size == 0)
    return 0;
  buffer[0] = '\0';
  if (width == 0 || line_index >= HOST_DETAIL_LINE_COUNT)
    return 0;
  if (fields) {
    for (unsigned i = 0; i < max_fields; ++i) {
      fields[i].offset = 0;
      fields[i].length = 0;
    }
  }

  const bool cpu_valid = state && state->cpu_valid;
  const bool memory_valid = state && state->memory_valid;
  const bool freq_valid = state && state->freq_valid;
  const bool load_valid = state && state->load_valid;
  const bool swap_valid = state && state->swap_valid;
  const bool power_valid = state && state->power_valid;
  const bool lenovo_fan_valid = state && state->lenovo_fan_valid;
  const struct host_cpu_identity *identity = state ? &state->identity : NULL;

  struct host_detail_segment segments[8];
  unsigned count = 0;
  switch (line_index) {
  case 0: {
    detail_segment(&segments[count++], host_detail_priority_always, false, "Device CPU", "Device CPU");
    if (identity && identity->model_valid) {
      // The model is a value, not a field: it has no title to color.
      detail_segment(&segments[count++], host_detail_priority_static, true, NULL, " [%s]", identity->model);
      // A model name is worth keeping: it is cut to what fits, with a marker,
      // before anything else of the line is dropped.
      segments[count - 1].shrinkable = true;
    } else {
      detail_segment(&segments[count++], host_detail_priority_static, true, NULL, " [N/A]");
    }
    char physical[16], logical[16];
    physical[0] = logical[0] = '\0';
    if (identity && identity->physical_cores)
      snprintf(physical, sizeof(physical), "%uC", identity->physical_cores);
    if (identity && identity->logical_threads)
      snprintf(logical, sizeof(logical), "%uT", identity->logical_threads);
    detail_segment(&segments[count++], host_detail_priority_static, true, "CORES", "  CORES %s/%s",
                   physical[0] ? physical : "N/A", logical[0] ? logical : "N/A");
    break;
  }
  case 1: {
    if (cpu_valid)
      detail_segment(&segments[count++], host_detail_priority_always, false, "CPU", "CPU  %4.1f%%",
                     state->cpu_percent);
    else
      detail_segment(&segments[count++], host_detail_priority_always, false, "CPU", "CPU  N/A");
    if (freq_valid)
      detail_segment(&segments[count++], host_detail_priority_static, true, "FREQ", "   FREQ %.2fGHz",
                     state->cpu_freq_mhz / 1000.);
    else
      detail_segment(&segments[count++], host_detail_priority_static, true, "FREQ", "   FREQ N/A");
    if (load_valid) {
      detail_segment(&segments[count++], host_detail_priority_static, true, "LOAD", "   LOAD %.2f",
                     state->load_avg[0]);
      // The two trailing averages are the continuation of the LOAD field: they
      // carry no title of their own.
      detail_segment(&segments[count++], host_detail_priority_load_5m, true, NULL, " / %.2f", state->load_avg[1]);
      detail_segment(&segments[count++], host_detail_priority_load_15m, true, NULL, " / %.2f", state->load_avg[2]);
    } else {
      detail_segment(&segments[count++], host_detail_priority_static, true, "LOAD", "   LOAD N/A");
    }
    // One package, one power, for now: the whole CPU package, not a per core
    // or per plane breakdown, and not a chart curve either.
    if (power_valid)
      detail_segment(&segments[count++], host_detail_priority_power, true, "POWER", "   POWER %4.1fW",
                     state->package_power_watts);
    else
      detail_segment(&segments[count++], host_detail_priority_power, true, "POWER", "   POWER N/A");
    if (state && state->lenovo_fan_supported) {
      if (lenovo_fan_valid)
        detail_segment(&segments[count++], host_detail_priority_lenovo_fan, true, "Lenovo CPU Fan",
                       "   Lenovo CPU Fan %u RPM", state->lenovo_fan_rpm);
      else
        detail_segment(&segments[count++], host_detail_priority_lenovo_fan, true, "Lenovo CPU Fan",
                       "   Lenovo CPU Fan N/A");
    }
    break;
  }
  case 2: {
    if (memory_valid) {
      detail_segment(&segments[count++], host_detail_priority_always, false, "RAM", "RAM  %.2f/%.2f GiB %4.1f%%",
                     state->memory_used_gib, state->memory_total_gib, state->memory_percent);
      detail_segment(&segments[count++], host_detail_priority_available, true, "AVAIL", "   AVAIL %.2f GiB",
                     kib_to_gib(state->memory.available_kib));
    } else {
      detail_segment(&segments[count++], host_detail_priority_always, false, "RAM", "RAM  N/A");
    }
    if (swap_valid)
      detail_segment(&segments[count++], host_detail_priority_swap, true, "SWAP", "   SWAP %.2f/%.2f GiB",
                     state->swap_used_gib, state->swap_total_gib);
    else
      detail_segment(&segments[count++], host_detail_priority_swap, true, "SWAP", "   SWAP N/A");
    break;
  }
  default:
    return 0;
  }

  char line[HOST_DETAIL_LINE_SIZE];
  unsigned needed = detail_render_line(line, sizeof(line), segments, count, fields, max_fields);
  while (needed > width) {
    // A field that may be cut, such as a model name, is cut one character at a
    // time before anything is dropped: the CPU stays identified as long as the
    // line has room for a few letters of it.
    bool shrunk = false;
    for (unsigned i = 0; i < count; ++i) {
      if (!segments[i].kept || !segments[i].shrinkable)
        continue;
      unsigned length = (unsigned)strlen(segments[i].text);
      if (length <= HOST_DETAIL_SHRINK_MIN_LENGTH) {
        // A name cut below a few letters identifies nothing: from there it is
        // dropped like any other field that does not fit.
        segments[i].shrinkable = false;
        break;
      }
      segments[i].text[length - 2] = '+';
      segments[i].text[length - 1] = '\0';
      needed = detail_render_line(line, sizeof(line), segments, count, fields, max_fields);
      shrunk = true;
      break;
    }
    if (needed <= width || !shrunk)
      break;
  }
  while (needed > width) {
    int lowest = -1;
    for (unsigned i = 0; i < count; ++i) {
      if (!segments[i].kept || !segments[i].droppable)
        continue;
      if (lowest < 0 || segments[i].priority < segments[lowest].priority)
        lowest = (int)i;
    }
    if (lowest < 0)
      break;
    segments[lowest].kept = false;
    needed = detail_render_line(line, sizeof(line), segments, count, fields, max_fields);
  }

  // What is left still may not fit: an overlong model name is truncated, with
  // a marker, rather than overflowing the terminal or wrapping on the next row.
  unsigned visible = needed < width ? needed : width;
  if ((size_t)visible > size - 1)
    visible = (unsigned)(size - 1);
  if (visible == 0) {
    buffer[0] = '\0';
    return needed;
  }
  if (visible < needed) {
    memcpy(buffer, line, visible - 1);
    buffer[visible - 1] = '+';
    buffer[visible] = '\0';
  } else {
    memcpy(buffer, line, visible);
    buffer[visible] = '\0';
  }
  // A title that the last cut removed, or that the marker replaced, is not in
  // the line the caller got: it must not be reported, and so not be colored.
  if (fields) {
    const unsigned kept_columns = visible < needed ? visible - 1 : visible;
    for (unsigned i = 0; i < max_fields; ++i) {
      if (fields[i].length == 0)
        continue;
      if (fields[i].offset + fields[i].length > kept_columns) {
        fields[i].offset = 0;
        fields[i].length = 0;
      }
    }
  }
  return needed;
}

unsigned host_metrics_format_detail_line(const struct host_metrics_state *state, unsigned line_index, unsigned width,
                                         char *buffer, size_t size) {
  return format_detail_line(state, line_index, width, buffer, size, NULL, 0u);
}

unsigned host_metrics_format_detail_line_fields(const struct host_metrics_state *state, unsigned line_index,
                                                unsigned width, char *buffer, size_t size,
                                                struct host_detail_field *fields, unsigned max_fields) {
  return format_detail_line(state, line_index, width, buffer, size, fields, max_fields);
}

#ifdef HOST_METRICS_LINUX
static char *read_text_file(const char *path, size_t limit) {
  FILE *file = fopen(path, "r");
  if (!file)
    return NULL;
  char *buffer = malloc(limit + 1);
  if (!buffer) {
    fclose(file);
    return NULL;
  }
  size_t total = 0;
  while (total < limit) {
    size_t read_count = fread(buffer + total, 1, limit - total, file);
    if (read_count == 0)
      break;
    total += read_count;
  }
  fclose(file);
  if (total == 0) {
    free(buffer);
    return NULL;
  }
  buffer[total] = '\0';
  return buffer;
}

static bool read_cpu_ticks(struct host_cpu_ticks *ticks) {
  char *content = read_text_file(HOST_PROC_STAT_PATH, HOST_PROC_FILE_READ_LIMIT);
  if (!content)
    return false;
  bool parsed = host_cpu_parse_stat_text(content, ticks);
  free(content);
  return parsed;
}

// One read of /proc/meminfo feeds both the memory and the swap parsing.
static char *read_meminfo_text(void) { return read_text_file(HOST_PROC_MEMINFO_PATH, HOST_PROC_MEMINFO_READ_LIMIT); }

static bool read_cpu_identity(struct host_cpu_identity *identity) {
  char *content = read_text_file(HOST_PROC_CPUINFO_PATH, HOST_PROC_CPUINFO_READ_LIMIT);
  if (!content)
    return false;
  bool parsed = host_cpu_parse_cpuinfo_text(content, identity);
  free(content);
  return parsed;
}

// The average frequency of every online core, from the cpufreq interface. It
// is the authoritative source, and the only one on the architectures that do
// not report a frequency in /proc/cpuinfo.
static bool read_scaling_frequency(double *mhz, unsigned logical_threads) {
  unsigned probed = logical_threads ? logical_threads : HOST_PROBED_CPUS_WHEN_UNKNOWN;
  if (probed > HOST_MAX_CPUS)
    probed = HOST_MAX_CPUS;
  double sum = 0.;
  unsigned count = 0;
  for (unsigned cpu = 0; cpu < probed; ++cpu) {
    char path[128];
    if (snprintf(path, sizeof(path), HOST_SYSFS_SCALING_FREQ_PATH, cpu) >= (int)sizeof(path))
      break;
    char *content = read_text_file(path, HOST_PROC_SMALL_FILE_READ_LIMIT);
    if (!content) {
      // The processors are numbered contiguously: a missing one ends the list.
      break;
    }
    double cpu_mhz = 0.;
    bool parsed = host_freq_parse_scaling_freq_text(content, &cpu_mhz);
    free(content);
    if (!parsed)
      break;
    sum += cpu_mhz;
    ++count;
  }
  if (count == 0)
    return false;
  *mhz = sum / (double)count;
  return true;
}

// The frequency reported by the architecture in /proc/cpuinfo, averaged over
// the logical threads it lists.
static bool read_cpuinfo_frequency(double *mhz) {
  char *content = read_text_file(HOST_PROC_CPUINFO_PATH, HOST_PROC_CPUINFO_READ_LIMIT);
  if (!content)
    return false;
  bool parsed = host_cpuinfo_parse_freq_text(content, mhz);
  free(content);
  return parsed;
}

static bool read_load_average(double load_avg[3]) {
  char *content = read_text_file(HOST_PROC_LOADAVG_PATH, HOST_PROC_SMALL_FILE_READ_LIMIT);
  if (!content)
    return false;
  bool parsed = host_loadavg_parse_text(content, load_avg);
  free(content);
  return parsed;
}
#else
static bool read_cpu_ticks(struct host_cpu_ticks *ticks) {
  (void)ticks;
  return false;
}

static char *read_meminfo_text(void) { return NULL; }

static bool read_cpu_identity(struct host_cpu_identity *identity) {
  (void)identity;
  return false;
}

static bool read_scaling_frequency(double *mhz, unsigned logical_threads) {
  (void)mhz;
  (void)logical_threads;
  return false;
}

static bool read_cpuinfo_frequency(double *mhz) {
  (void)mhz;
  return false;
}

static bool read_load_average(double load_avg[3]) {
  (void)load_avg;
  return false;
}
#endif


// The package power /////////////////////////////////////////////////
// The power of the whole CPU package, from the standard kernel interfaces: the
// powercap (RAPL) energy counter first, since it is what a package power is
// really made of - an energy over a time - and, where there is none, a hwmon
// power input whose label says it is the package. No raw MSR access, no external
// tool: whatever the kernel does not hand out to this process simply is not
// reported.

// The powercap zone is found by the `name` it declares rather than by a fixed
// path: the same package is intel-rapl:0 on one kernel, amd-rapl:0 on another,
// and the numbering of the zones is not promised to anybody.
#define HOST_POWERCAP_ROOT "/sys/class/powercap"
#define HOST_HWMON_ROOT "/sys/class/hwmon"

#define HOST_POWER_FILE_READ_LIMIT 64u
#define HOST_POWER_NAME_READ_LIMIT 128u
#define HOST_POWER_PATH_SIZE 256u
// Bound the sysfs scans: a machine has a handful of powercap zones and a handful
// of hwmon chips. A scan that finds nothing must not walk a whole tree at every
// refresh.
#define HOST_POWER_MAX_ZONES 16u
#define HOST_HWMON_MAX_CHIPS 16u
#define HOST_HWMON_MAX_INPUTS 16u
// Same give up and retry rhythm as the frequency sources: a machine that has no
// package power at all is not worth reopening two directories per refresh.
#define HOST_POWER_PROBE_GIVE_UP_AFTER 4u
#define HOST_POWER_PROBE_RETRY_PERIOD 256u

#ifdef HOST_METRICS_LINUX
static bool host_is_lenovo(void) {
  char *vendor = read_text_file(HOST_DMI_SYS_VENDOR_PATH, HOST_POWER_NAME_READ_LIMIT);
  if (!vendor)
    return false;
  for (char *cursor = vendor; *cursor; ++cursor)
    if (*cursor >= 'a' && *cursor <= 'z')
      *cursor = (char)(*cursor - 'a' + 'A');
  const bool lenovo = strstr(vendor, "LENOVO") != NULL;
  free(vendor);
  return lenovo;
}

static bool read_lenovo_fan_rpm_file(const char *path, unsigned *rpm) {
  char *content = read_text_file(path, HOST_POWER_FILE_READ_LIMIT);
  if (!content)
    return false;
  bool parsed = host_lenovo_fan_parse_rpm_text(content, rpm);
  free(content);
  return parsed;
}

static bool helper_lenovo_cpu_fan(unsigned *rpm) {
  const char *path = getenv("NVTOP_LENOVO_CPU_FAN_RPM_PATH");
  if (!path || !path[0])
    path = HOST_LENOVO_FAN_HELPER_PATH;
  struct stat attributes;
  if (stat(path, &attributes) != 0)
    return false;
  const time_t now = time(NULL);
  if (now != (time_t)-1 && attributes.st_mtime <= now &&
      now - attributes.st_mtime > HOST_LENOVO_FAN_HELPER_MAX_AGE_SECONDS)
    return false;
  return read_lenovo_fan_rpm_file(path, rpm);
}
#else
static bool host_is_lenovo(void) { return false; }
static bool helper_lenovo_cpu_fan(unsigned *rpm) { (void)rpm; return false; }
#endif

static void update_lenovo_cpu_fan(struct host_metrics_state *state) {
  state->lenovo_fan_valid = false;
  if (!state->lenovo_fan_support_probed) {
    state->lenovo_fan_supported = host_is_lenovo();
    state->lenovo_fan_support_probed = true;
  }
  if (!state->lenovo_fan_supported)
    return;
  if (state->lenovo_fan_probe_failures >= HOST_LENOVO_FAN_PROBE_GIVE_UP_AFTER &&
      (state->sample_count % HOST_LENOVO_FAN_PROBE_RETRY_PERIOD) != 0u)
    return;
  unsigned rpm = 0;
  if (helper_lenovo_cpu_fan(&rpm)) {
    state->lenovo_fan_rpm = rpm;
    state->lenovo_fan_valid = true;
    state->lenovo_fan_probe_failures = 0u;
    return;
  }
  if (state->lenovo_fan_probe_failures < HOST_LENOVO_FAN_PROBE_GIVE_UP_AFTER)
    ++state->lenovo_fan_probe_failures;
}

enum host_power_source {
  host_power_source_none = 0,
  host_power_source_energy,  // A cumulative energy counter: two samples make a power
  host_power_source_instant, // A sensor that reports watts right away
};

#ifdef HOST_METRICS_LINUX

static bool read_power_counter(const char *path, uint64_t *value) {
  char *content = read_text_file(path, HOST_POWER_FILE_READ_LIMIT);
  if (!content)
    return false;
  bool parsed = host_power_parse_energy_counter_text(content, value);
  free(content);
  return parsed;
}

// true if the entry of /sys/class/powercap declares a whole package, in which
// case its package index and the path of the zone are returned.
static bool powercap_zone_is_package(const char *entry_name, unsigned *index, char *path, size_t path_size) {
  char name_path[HOST_POWER_PATH_SIZE];
  if (snprintf(name_path, sizeof(name_path), HOST_POWERCAP_ROOT "/%s/name", entry_name) >= (int)sizeof(name_path))
    return false;
  char *content = read_text_file(name_path, HOST_POWER_NAME_READ_LIMIT);
  if (!content)
    return false;
  char name[64];
  bool parsed = host_power_parse_zone_name_text(content, name, sizeof(name));
  free(content);
  if (!parsed)
    return false;
  unsigned zone_index = 0;
  if (!host_power_zone_name_index(name, &zone_index))
    return false;
  if (snprintf(path, path_size, HOST_POWERCAP_ROOT "/%s", entry_name) >= (int)path_size)
    return false;
  *index = zone_index;
  return true;
}

// The energy counter of the first package of the machine.
static bool powercap_package_power(struct host_power_sample *sample, char *source, size_t source_size) {
  DIR *root = opendir(HOST_POWERCAP_ROOT);
  if (!root)
    return false;

  bool found = false;
  struct host_power_sample best;
  memset(&best, 0, sizeof(best));
  char best_source[HOST_POWER_SOURCE_MAX_LENGTH];
  best_source[0] = '\0';
  unsigned best_index = 0;
  unsigned scanned = 0;
  struct dirent *entry = NULL;
  while (scanned < HOST_POWER_MAX_ZONES && (entry = readdir(root)) != NULL) {
    ++scanned;
    if (entry->d_name[0] == '.')
      continue;
    char zone[HOST_POWER_SOURCE_MAX_LENGTH];
    unsigned index = 0;
    if (!powercap_zone_is_package(entry->d_name, &index, zone, sizeof(zone)))
      continue;
    if (found && index >= best_index)
      continue; // The lowest numbered package is the one reported: it is the
                // first CPU of the machine, and the one the rest of the block
                // describes.
    uint64_t energy = 0;
    uint64_t range = 0;
    char counter[HOST_POWER_PATH_SIZE];
    if (snprintf(counter, sizeof(counter), "%s/energy_uj", zone) >= (int)sizeof(counter))
      continue;
    if (!read_power_counter(counter, &energy) || energy == 0)
      continue;
    if (snprintf(counter, sizeof(counter), "%s/max_energy_range_uj", zone) >= (int)sizeof(counter))
      continue;
    // Without the range a wrap cannot be told apart from a reset, and a reset
    // read as a wrap is a burst of power that never happened.
    if (!read_power_counter(counter, &range) || range == 0)
      continue;
    best.energy_uj = energy;
    best.max_range_uj = range;
    snprintf(best_source, sizeof(best_source), "%s", zone);
    found = true;
  }
  closedir(root);

  if (!found)
    return false;
  *sample = best;
  snprintf(source, source_size, "%s", best_source);
  return true;
}

// Fallback for the machines that have no powercap package zone but do have a
// hwmon sensor labelled as one: platform monitors, some embedded boards. The
// label is required - a power input that does not say what it measures is not
// allowed to stand for the package.
static bool hwmon_package_power(double *watts, char *source, size_t source_size) {
  static const char input_suffix[] = "_input";
  DIR *root = opendir(HOST_HWMON_ROOT);
  if (!root)
    return false;

  bool found = false;
  unsigned chips = 0;
  struct dirent *chip = NULL;
  while (!found && chips < HOST_HWMON_MAX_CHIPS && (chip = readdir(root)) != NULL) {
    ++chips;
    if (chip->d_name[0] == '.')
      continue;
    char chip_dir[HOST_POWER_PATH_SIZE];
    if (snprintf(chip_dir, sizeof(chip_dir), HOST_HWMON_ROOT "/%s", chip->d_name) >= (int)sizeof(chip_dir))
      continue;
    DIR *dir = opendir(chip_dir);
    if (!dir)
      continue;
    unsigned inputs = 0;
    struct dirent *attribute = NULL;
    while (inputs < HOST_HWMON_MAX_INPUTS && (attribute = readdir(dir)) != NULL) {
      ++inputs;
      const char *name = attribute->d_name;
      size_t length = strlen(name);
      size_t base_length = length > sizeof(input_suffix) - 1u ? length - (sizeof(input_suffix) - 1u) : 0;
      if (strncmp(name, "power", 5) != 0 || base_length == 0 || base_length >= length)
        continue;
      if (strcmp(name + base_length, input_suffix) != 0)
        continue;
      char label_path[HOST_POWER_PATH_SIZE];
      if (snprintf(label_path, sizeof(label_path), "%s/%.*s_label", chip_dir, (int)base_length, name) >=
          (int)sizeof(label_path))
        continue;
      char *label = read_text_file(label_path, HOST_POWER_NAME_READ_LIMIT);
      if (!label)
        continue;
      bool is_package = host_power_hwmon_label_is_package(label);
      free(label);
      if (!is_package)
        continue;
      char input_path[HOST_POWER_PATH_SIZE];
      if (snprintf(input_path, sizeof(input_path), "%s/%s", chip_dir, name) >= (int)sizeof(input_path))
        continue;
      char *content = read_text_file(input_path, HOST_POWER_FILE_READ_LIMIT);
      if (!content)
        continue;
      double value = 0.;
      bool parsed = host_power_parse_hwmon_input_text(content, &value);
      free(content);
      if (!parsed)
        continue;
      *watts = value;
      snprintf(source, source_size, "%s", input_path);
      found = true;
      break;
    }
    closedir(dir);
  }
  closedir(root);
  return found;
}

static enum host_power_source read_package_power(struct host_power_sample *sample, double *watts, char *source,
                                                 size_t source_size) {
  source[0] = '\0';
  if (powercap_package_power(sample, source, source_size))
    return host_power_source_energy;
  if (hwmon_package_power(watts, source, source_size))
    return host_power_source_instant;
  return host_power_source_none;
}
#else
static enum host_power_source read_package_power(struct host_power_sample *sample, double *watts, char *source,
                                                 size_t source_size) {
  (void)sample;
  (void)watts;
  if (source && source_size > 0)
    source[0] = '\0';
  return host_power_source_none;
}
#endif

// The power of the CPU package. An energy counter is read twice before it means
// anything: the first sample only builds the reference, exactly like the CPU
// tick counters do, and every sample after that gives the watts of the interval
// it covers. A source that disappears, or that hands out something that cannot
// be trusted, makes the power unavailable: the field reads N/A, never a power
// that was made up.
static void update_package_power(struct host_metrics_state *state) {
  state->power_valid = false;

  if (state->power_probe_failures >= HOST_POWER_PROBE_GIVE_UP_AFTER &&
      (state->sample_count % HOST_POWER_PROBE_RETRY_PERIOD) != 0u) {
    state->has_last_power = false;
    return;
  }

  struct host_power_sample sample;
  memset(&sample, 0, sizeof(sample));
  double watts = 0.;
  char source[HOST_POWER_SOURCE_MAX_LENGTH];
  enum host_power_source source_kind = read_package_power(&sample, &watts, source, sizeof(source));
  if (source_kind == host_power_source_none) {
    // Nothing to sample: the reference is worthless, since whatever shows up
    // later may be another counter entirely.
    state->has_last_power = false;
    state->power_source[0] = '\0';
    if (state->power_probe_failures < HOST_POWER_PROBE_GIVE_UP_AFTER)
      ++state->power_probe_failures;
    return;
  }

  if (strncmp(source, state->power_source, sizeof(state->power_source)) != 0) {
    // Another file than the one the reference came from: start over.
    state->has_last_power = false;
    snprintf(state->power_source, sizeof(state->power_source), "%s", source);
  }

  nvtop_time now_time;
  nvtop_get_current_time(&now_time);
  const uint64_t now_nsec = nvtop_time_u64(now_time);

  if (source_kind == host_power_source_instant) {
    state->package_power_watts = watts;
    state->power_valid = true;
    state->has_last_power = false;
    state->power_probe_failures = 0u;
    return;
  }

  if (state->has_last_power) {
    // The elapsed time is monotonic: suspend does not inflate it, and a counter
    // that jumped forward stays the delta of what this interval really was.
    // A clock that went backwards, if any ever does, gives no elapsed time and
    // therefore no power.
    const double elapsed =
        now_nsec > state->last_power_time_nsec ? (double)(now_nsec - state->last_power_time_nsec) / 1e9 : 0.;
    if (host_power_watts_between(&state->last_power, &sample, elapsed, &watts)) {
      state->package_power_watts = watts;
      state->power_valid = true;
    }
  }
  // Keep the most recent trustworthy sample as the reference, even when the rate
  // could not be computed from it: the next refresh starts from there, so a
  // counter reset costs one interval and no more.
  state->last_power = sample;
  state->last_power_time_nsec = now_nsec;
  state->has_last_power = true;
  state->power_probe_failures = 0u;
}

// The number of logical threads and the threads per core, from the sysfs
// topology. They are the sources that work on every architecture, unlike the
// optional fields of /proc/cpuinfo.
static bool read_sysfs_processor_count(const char *path, unsigned *count) {
#ifdef HOST_METRICS_LINUX
  char *content = read_text_file(path, HOST_PROC_SMALL_FILE_READ_LIMIT);
  if (!content)
    return false;
  bool parsed = host_cpu_parse_processor_list(content, count);
  free(content);
  return parsed;
#else
  (void)path;
  (void)count;
  return false;
#endif
}

// The identity of the CPU does not change while the process runs: it is read
// once, and the fields the /proc/cpuinfo parsing could not fill are completed
// with the sysfs topology.
static void probe_cpu_identity(struct host_metrics_state *state) {
  struct host_cpu_identity identity;
  memset(&identity, 0, sizeof(identity));
  read_cpu_identity(&identity);

  unsigned logical_threads = 0;
  if (read_sysfs_processor_count(HOST_SYSFS_CPU_ONLINE_PATH, &logical_threads))
    identity.logical_threads = logical_threads;

  unsigned threads_per_core = 0;
  if (identity.logical_threads > 0 && read_sysfs_processor_count(HOST_SYSFS_THREAD_SIBLINGS_PATH, &threads_per_core) &&
      threads_per_core > 0 && identity.logical_threads % threads_per_core == 0)
    identity.physical_cores = identity.logical_threads / threads_per_core;

  state->identity = identity;
  state->identity_valid = identity.model_valid || identity.physical_cores > 0 || identity.logical_threads > 0;
  state->identity_probed = true;
}

// The current frequency of the CPU, from the cpufreq interface first and from
// /proc/cpuinfo otherwise. A source that answers nothing is not polled again
// at every refresh.
static void update_cpu_frequency(struct host_metrics_state *state) {
  if (state->freq_probe_failures >= HOST_FREQ_PROBE_GIVE_UP_AFTER &&
      (state->sample_count % HOST_FREQ_PROBE_RETRY_PERIOD) != 0u) {
    state->freq_valid = false;
    return;
  }
  double mhz = 0.;
  if (read_scaling_frequency(&mhz, state->identity.logical_threads) || read_cpuinfo_frequency(&mhz)) {
    state->cpu_freq_mhz = mhz;
    state->freq_valid = true;
    state->freq_probe_failures = 0u;
    return;
  }
  state->freq_valid = false;
  if (state->freq_probe_failures < HOST_FREQ_PROBE_GIVE_UP_AFTER)
    ++state->freq_probe_failures;
}

bool host_metrics_update(struct host_metrics_state *state) {
  if (!state)
    return false;
  state->supported = host_metrics_platform_supported();
  state->cpu_valid = false;
  state->memory_valid = false;
  state->swap_valid = false;
  state->freq_valid = false;
  state->load_valid = false;
  state->power_valid = false;
  if (!state->supported)
    return false;

  struct host_cpu_ticks ticks;
  if (read_cpu_ticks(&ticks)) {
    double utilization;
    if (state->has_last_cpu && host_cpu_utilization(&state->last_cpu, &ticks, &utilization)) {
      state->cpu_percent = utilization;
      state->cpu_valid = true;
      history_push(state, host_metric_cpu, utilization);
    }
    // Keep the most recent valid sample as the reference, even when the rate
    // could not be computed, so that the next refresh starts from there.
    state->last_cpu = ticks;
    state->has_last_cpu = true;
  }

  char *meminfo = read_meminfo_text();
  if (meminfo) {
    struct host_memory_info memory;
    if (host_memory_parse_meminfo_text(meminfo, &memory)) {
      double used_gib, total_gib, percent;
      if (host_memory_usage(&memory, &used_gib, &total_gib, &percent)) {
        state->memory = memory;
        state->memory_used_gib = used_gib;
        state->memory_total_gib = total_gib;
        state->memory_percent = percent;
        state->memory_valid = true;
        history_push(state, host_metric_memory, percent);
      }
    }
    // Used swap is SwapTotal - SwapFree: an inconsistent pair is reported as
    // unavailable, never as a plausible wrap around.
    struct host_swap_info swap;
    if (host_swap_parse_meminfo_text(meminfo, &swap)) {
      double swap_used_gib, swap_total_gib;
      if (host_swap_usage(&swap, &swap_used_gib, &swap_total_gib)) {
        state->swap = swap;
        state->swap_used_gib = swap_used_gib;
        state->swap_total_gib = swap_total_gib;
        state->swap_valid = true;
      }
    }
    free(meminfo);
  }

  if (!state->identity_probed)
    probe_cpu_identity(state);
  update_cpu_frequency(state);
  // The package power is its own sample: it is not a chart curve, and it is
  // read from its own sources, at the same rhythm as everything else.
  update_package_power(state);
  update_lenovo_cpu_fan(state);

  double load_avg[3];
  if (read_load_average(load_avg)) {
    for (unsigned i = 0; i < 3; ++i)
      state->load_avg[i] = load_avg[i];
    state->load_valid = true;
  }

  ++state->sample_count;
  return state->cpu_valid || state->memory_valid;
}

static struct host_metrics_state *global_host_metrics = NULL;

struct host_metrics_state *host_metrics_get_state(void) {
  if (!global_host_metrics) {
    global_host_metrics = calloc(1, sizeof(*global_host_metrics));
    if (!global_host_metrics)
      return NULL;
    if (!host_metrics_init(global_host_metrics, HOST_METRICS_HISTORY_SIZE)) {
      free(global_host_metrics);
      global_host_metrics = NULL;
      return NULL;
    }
    // Seed the CPU baseline so that the first refresh after startup already
    // reports a rate instead of an unavailable value.
#ifdef HOST_METRICS_LINUX
    if (read_cpu_ticks(&global_host_metrics->last_cpu))
      global_host_metrics->has_last_cpu = true;
#endif
  }
  return global_host_metrics;
}

void host_metrics_shutdown(void) {
  if (!global_host_metrics)
    return;
  host_metrics_release(global_host_metrics);
  free(global_host_metrics);
  global_host_metrics = NULL;
}

bool host_metrics_refresh(void) { return host_metrics_update(host_metrics_get_state()); }
