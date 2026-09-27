/* Copyright (C) 2026 etan68
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hwdash/vendor/lenovo_cpu_fan.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__linux__) || defined(HOST_METRICS_FORCE_PROC)
#define HWDASH_VENDOR_LINUX 1
#include <sys/stat.h>
#endif

#define LENOVO_CPU_FAN_MAX_PLAUSIBLE_RPM 30000u
#define LENOVO_CPU_FAN_PROBE_GIVE_UP_AFTER 4u
#define LENOVO_CPU_FAN_PROBE_RETRY_PERIOD 64u
#define LENOVO_CPU_FAN_HELPER_PATH "/run/hwdash/lenovo-cpu-fan-rpm"
#define LENOVO_CPU_FAN_HELPER_MAX_AGE_SECONDS 5
#define DMI_SYS_VENDOR_PATH "/sys/class/dmi/id/sys_vendor"

static const char *skip_blanks(const char *text) {
  while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r')
    ++text;
  return text;
}

bool hwdash_lenovo_cpu_fan_parse_rpm(const char *text, unsigned *rpm) {
  if (!text || !rpm)
    return false;
  text = skip_blanks(text);
  if (*text < '0' || *text > '9')
    return false;
  errno = 0;
  char *end = NULL;
  unsigned long value = strtoul(text, &end, 10);
  if (end == text || errno == ERANGE || value > LENOVO_CPU_FAN_MAX_PLAUSIBLE_RPM || *skip_blanks(end) != '\0')
    return false;
  *rpm = (unsigned)value;
  return true;
}

void hwdash_lenovo_cpu_fan_reset(struct hwdash_lenovo_cpu_fan *fan) {
  if (fan)
    memset(fan, 0, sizeof(*fan));
}

#ifdef HWDASH_VENDOR_LINUX
static char *read_small_text_file(const char *path) {
  FILE *file = fopen(path, "r");
  if (!file)
    return NULL;
  char *text = calloc(129u, 1u);
  if (!text) {
    fclose(file);
    return NULL;
  }
  const size_t length = fread(text, 1u, 128u, file);
  const bool failed = ferror(file) != 0;
  fclose(file);
  if (failed || length == 0u) {
    free(text);
    return NULL;
  }
  text[length] = '\0';
  return text;
}

static bool machine_is_lenovo(void) {
  char *vendor = read_small_text_file(DMI_SYS_VENDOR_PATH);
  if (!vendor)
    return false;
  for (char *cursor = vendor; *cursor; ++cursor)
    if (*cursor >= 'a' && *cursor <= 'z')
      *cursor = (char)(*cursor - 'a' + 'A');
  const bool matches = strstr(vendor, "LENOVO") != NULL;
  free(vendor);
  return matches;
}

static bool read_helper_value(unsigned *rpm) {
  const char *path = getenv("HWDASH_LENOVO_CPU_FAN_RPM_PATH");
  if (!path || !path[0])
    path = LENOVO_CPU_FAN_HELPER_PATH;
  struct stat attributes;
  if (stat(path, &attributes) != 0)
    return false;
  const time_t now = time(NULL);
  if (now != (time_t)-1 && attributes.st_mtime <= now &&
      now - attributes.st_mtime > LENOVO_CPU_FAN_HELPER_MAX_AGE_SECONDS)
    return false;
  char *text = read_small_text_file(path);
  if (!text)
    return false;
  const bool parsed = hwdash_lenovo_cpu_fan_parse_rpm(text, rpm);
  free(text);
  return parsed;
}
#else
static bool machine_is_lenovo(void) { return false; }
static bool read_helper_value(unsigned *rpm) {
  (void)rpm;
  return false;
}
#endif

void hwdash_lenovo_cpu_fan_update(struct hwdash_lenovo_cpu_fan *fan, unsigned sample_count) {
  if (!fan)
    return;
  fan->valid = false;
  if (!fan->support_probed) {
    fan->supported = machine_is_lenovo();
    fan->support_probed = true;
  }
  if (!fan->supported)
    return;
  if (fan->probe_failures >= LENOVO_CPU_FAN_PROBE_GIVE_UP_AFTER &&
      (sample_count % LENOVO_CPU_FAN_PROBE_RETRY_PERIOD) != 0u)
    return;
  unsigned rpm = 0;
  if (read_helper_value(&rpm)) {
    fan->rpm = rpm;
    fan->valid = true;
    fan->probe_failures = 0u;
  } else if (fan->probe_failures < LENOVO_CPU_FAN_PROBE_GIVE_UP_AFTER) {
    ++fan->probe_failures;
  }
}
