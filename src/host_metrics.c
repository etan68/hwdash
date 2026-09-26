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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// HOST_METRICS_FORCE_PROC only exists to compile check the /proc code paths on
// a platform that has no /proc filesystem.
#if defined(__linux__) || defined(HOST_METRICS_FORCE_PROC)
#define HOST_METRICS_LINUX 1
#endif

#define HOST_METRICS_ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define HOST_METRICS_HISTORY_MIN_CAPACITY 16u
#define HOST_METRICS_HISTORY_MAX_CAPACITY 4096u

#ifdef HOST_METRICS_LINUX
#define HOST_PROC_STAT_PATH "/proc/stat"
#define HOST_PROC_MEMINFO_PATH "/proc/meminfo"
// The aggregate cpu line and the Mem* entries are always near the top of the
// files, reading more would be wasteful for a periodic poll.
#define HOST_PROC_FILE_READ_LIMIT 4096u
#endif

bool host_metrics_platform_supported(void) {
#ifdef HOST_METRICS_LINUX
  return true;
#else
  return false;
#endif
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

unsigned host_metrics_format_legend(const struct host_metrics_state *state, enum host_metric metric, bool short_form,
                                    char *buffer, size_t size) {
  if (!buffer || size == 0)
    return 0;
  if (metric >= host_metric_count)
    return (unsigned)snprintf(buffer, size, "Host N/A");
  const char *label = host_metric_name(metric);
  bool valid = state && ((metric == host_metric_cpu) ? state->cpu_valid : state->memory_valid);
  if (!valid)
    return (unsigned)snprintf(buffer, size, "%s N/A", label);
  if (metric == host_metric_cpu)
    return (unsigned)snprintf(buffer, size, "%s %4.1f%%", label, state->cpu_percent);
  if (short_form)
    return (unsigned)snprintf(buffer, size, "%s %4.1f%%", label, state->memory_percent);
  return (unsigned)snprintf(buffer, size, "%s %.2f/%.2f GiB %4.1f%%", label, state->memory_used_gib,
                            state->memory_total_gib, state->memory_percent);
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

static bool read_memory_info(struct host_memory_info *info) {
  char *content = read_text_file(HOST_PROC_MEMINFO_PATH, HOST_PROC_FILE_READ_LIMIT);
  if (!content)
    return false;
  bool parsed = host_memory_parse_meminfo_text(content, info);
  free(content);
  return parsed;
}
#else
static bool read_cpu_ticks(struct host_cpu_ticks *ticks) {
  (void)ticks;
  return false;
}

static bool read_memory_info(struct host_memory_info *info) {
  (void)info;
  return false;
}
#endif

bool host_metrics_update(struct host_metrics_state *state) {
  if (!state)
    return false;
  state->supported = host_metrics_platform_supported();
  state->cpu_valid = false;
  state->memory_valid = false;
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

  struct host_memory_info memory;
  if (read_memory_info(&memory)) {
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
