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

#include "nvtop/host_metrics.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Returns true when *p is at the end of the cpu line: only whitespace and
// the line terminator may follow.
static bool cpu_line_at_end(const char *p) {
  while (*p == ' ' || *p == '\t')
    p++;
  return *p == '\0' || *p == '\n';
}

// Parses a mandatory unsigned 64-bit field from the current position of *ptr,
// skipping leading whitespace. A missing, malformed or overflowing field
// fails the whole parse.
static bool parse_u64_field(const char **ptr, unsigned long long *out) {
  const char *p = *ptr;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p < '0' || *p > '9')
    return false;
  errno = 0;
  char *endptr = NULL;
  unsigned long long value = strtoull(p, &endptr, 10);
  if (endptr == p || errno == ERANGE)
    return false;
  *out = value;
  *ptr = endptr;
  return true;
}

// Parses an optional unsigned 64-bit field (iowait, irq, ...). A field that
// is absent (end of line) stays zero, but a field that is present must be a
// valid number: malformed or overflowing tokens fail the whole parse.
static bool parse_optional_u64_field(const char **ptr, unsigned long long *out) {
  const char *p = *ptr;
  while (*p == ' ' || *p == '\t')
    p++;
  if (cpu_line_at_end(p)) {
    *out = 0;
    return true;
  }
  if (*p < '0' || *p > '9')
    return false;
  errno = 0;
  char *endptr = NULL;
  unsigned long long value = strtoull(p, &endptr, 10);
  if (endptr == p || errno == ERANGE)
    return false;
  *out = value;
  *ptr = endptr;
  return true;
}

bool host_metrics_parse_cpu_stat_line(const char *line, host_cpu_stat *out) {
  if (!line || !out)
    return false;
  // Only the aggregate line starts with "cpu "; per-core lines are "cpu0" etc.
  if (strncmp(line, "cpu ", 4) != 0)
    return false;
  host_cpu_stat stat;
  memset(&stat, 0, sizeof(stat));
  const char *p = line + 4;
  // user, nice, system and idle are present on every kernel
  if (!parse_u64_field(&p, &stat.user) || !parse_u64_field(&p, &stat.nice) || !parse_u64_field(&p, &stat.system) ||
      !parse_u64_field(&p, &stat.idle))
    return false;
  // The fourth field must be well delimited: the next character is a
  // whitespace introducing the optional fields, or the line terminator
  if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\0')
    return false;
  // iowait, irq, softirq, steal, guest and guest_nice appeared in later
  // kernels: absent fields stay zero, but present fields must be valid
  if (!parse_optional_u64_field(&p, &stat.iowait) || !parse_optional_u64_field(&p, &stat.irq) ||
      !parse_optional_u64_field(&p, &stat.softirq) || !parse_optional_u64_field(&p, &stat.steal) ||
      !parse_optional_u64_field(&p, &stat.guest) || !parse_optional_u64_field(&p, &stat.guest_nice))
    return false;
  // Nothing may follow the optional fields
  if (!cpu_line_at_end(p))
    return false;
  *out = stat;
  return true;
}

// Sums values into *out; returns false when the sum would overflow 64 bits
// so garbage inputs cannot wrap around into a plausible result.
static bool sum_u64(const unsigned long long *values, size_t count, unsigned long long *out) {
  unsigned long long sum = 0;
  for (size_t i = 0; i < count; ++i) {
    if (values[i] > ULLONG_MAX - sum)
      return false;
    sum += values[i];
  }
  *out = sum;
  return true;
}

bool host_metrics_cpu_utilization_percent(const host_cpu_stat *prev, const host_cpu_stat *curr,
                                          double *out_percent) {
  if (!prev || !curr || !out_percent)
    return false;
  // Reject decreasing counters (wraparound / reset / malformed input)
  const unsigned long long *prev_fields[10] = {&prev->user,  &prev->nice,   &prev->system, &prev->idle,
                                               &prev->iowait, &prev->irq,    &prev->softirq, &prev->steal,
                                               &prev->guest,  &prev->guest_nice};
  const unsigned long long *curr_fields[10] = {&curr->user,  &curr->nice,   &curr->system, &curr->idle,
                                               &curr->iowait, &curr->irq,   &curr->softirq, &curr->steal,
                                               &curr->guest,  &curr->guest_nice};
  for (unsigned i = 0; i < 10; ++i) {
    if (*curr_fields[i] < *prev_fields[i])
      return false;
  }
  // Whole-host busy time: user, nice, system, irq, softirq and steal.
  // guest and guest_nice are virtual CPU time already accounted in user and
  // nice, so they are NOT added again: guest execution is busy CPU time.
  const unsigned long long busy_deltas[6] = {curr->user - prev->user,       curr->nice - prev->nice,
                                             curr->system - prev->system,   curr->irq - prev->irq,
                                             curr->softirq - prev->softirq, curr->steal - prev->steal};
  // Both idle and iowait count as idle time
  const unsigned long long idle_deltas[2] = {curr->idle - prev->idle, curr->iowait - prev->iowait};
  unsigned long long idle = 0, busy = 0;
  if (!sum_u64(idle_deltas, 2, &idle) || !sum_u64(busy_deltas, 6, &busy) || busy > ULLONG_MAX - idle)
    return false;
  unsigned long long total = idle + busy;
  if (total == 0)
    return false; // No tick elapsed between the samples
  *out_percent = 100.0 * (double)busy / (double)total;
  if (*out_percent < 0.)
    *out_percent = 0.;
  if (*out_percent > 100.)
    *out_percent = 100.;
  return true;
}

bool host_metrics_cpu_sampler_update(host_cpu_sampler *sampler, const host_cpu_stat *curr, double *out_percent) {
  if (!sampler || !curr || !out_percent)
    return false;
  if (!sampler->has_prev) {
    sampler->prev = *curr;
    sampler->has_prev = true;
    return false; // First sample only primes the sampler
  }
  bool valid = host_metrics_cpu_utilization_percent(&sampler->prev, curr, out_percent);
  // Always re-prime the baseline with the newest sample. When the delta is
  // unusable (e.g. the counters were reset or decreased), keeping the old
  // baseline would leave the sampler stuck; re-priming recovers on the next
  // sample.
  sampler->prev = *curr;
  return valid;
}

bool host_metrics_read_cpu_stat(const char *path, host_cpu_stat *out) {
  if (!path || !out)
    return false;
  FILE *file = fopen(path, "r");
  if (!file)
    return false;
  char line[256];
  bool found = false;
  while (!found && fgets(line, sizeof(line), file))
    found = host_metrics_parse_cpu_stat_line(line, out);
  fclose(file);
  return found;
}

bool host_metrics_parse_meminfo_value(const char *line, const char *name, unsigned long long *out_kb) {
  if (!line || !name || !out_kb)
    return false;
  size_t name_len = strlen(name);
  if (strncmp(line, name, name_len) != 0)
    return false;
  const char *p = line + name_len;
  if (*p != ':')
    return false;
  p++;
  while (*p == ' ' || *p == '\t')
    p++;
  if (*p < '0' || *p > '9')
    return false;
  errno = 0;
  char *endptr = NULL;
  unsigned long long value = strtoull(p, &endptr, 10);
  if (endptr == p || errno == ERANGE)
    return false;
  // MemTotal and MemAvailable use kB; reject other units or a missing unit.
  while (*endptr == ' ' || *endptr == '\t')
    endptr++;
  if (strncmp(endptr, "kB", 2) != 0)
    return false;
  endptr += 2;
  if (*endptr != '\0' && *endptr != '\n')
    return false;
  *out_kb = value;
  return true;
}

bool host_metrics_parse_meminfo(const char *content, host_meminfo *out) {
  if (!content || !out)
    return false;
  *out = (host_meminfo){false, false, 0, 0};
  const char *p = content;
  while (*p != '\0') {
    const char *eol = strchr(p, '\n');
    size_t line_len = eol ? (size_t)(eol - p) : strlen(p);
    if (line_len < 256) {
      char line[256];
      memcpy(line, p, line_len);
      line[line_len] = '\0';
      if (host_metrics_parse_meminfo_value(line, "MemTotal", &out->total_kb))
        out->has_total = true;
      if (host_metrics_parse_meminfo_value(line, "MemAvailable", &out->available_kb))
        out->has_available = true;
    }
    if (!eol)
      break;
    p = eol + 1;
  }
  return true;
}

bool host_metrics_read_meminfo(const char *path, host_meminfo *out) {
  if (!path || !out)
    return false;
  FILE *file = fopen(path, "r");
  if (!file)
    return false;
  char buffer[4096];
  size_t total_read = 0;
  for (;;) {
    size_t n = fread(buffer + total_read, 1, sizeof(buffer) - 1 - total_read, file);
    total_read += n;
    if (n == 0 || total_read + 1 >= sizeof(buffer))
      break;
  }
  buffer[total_read] = '\0';
  fclose(file);
  return host_metrics_parse_meminfo(buffer, out);
}

bool host_metrics_ram_usage(const host_meminfo *info, double *out_used_gib, double *out_total_gib,
                            double *out_percent) {
  if (!info || !out_used_gib || !out_total_gib || !out_percent)
    return false;
  if (!info->has_total || info->total_kb == 0)
    return false;
  if (!info->has_available)
    return false;
  if (info->available_kb > info->total_kb)
    return false; // Underflow: inconsistent data
  *out_total_gib = (double)info->total_kb / (1024.0 * 1024.0);
  *out_used_gib = (double)(info->total_kb - info->available_kb) / (1024.0 * 1024.0);
  *out_percent = 100.0 * (double)(info->total_kb - info->available_kb) / (double)info->total_kb;
  if (*out_percent < 0.)
    *out_percent = 0.;
  if (*out_percent > 100.)
    *out_percent = 100.;
  return true;
}

bool host_metrics_platform_supported(void) {
#ifdef __linux__
  return true;
#else
  return false;
#endif
}
