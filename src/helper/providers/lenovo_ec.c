/* Copyright (C) 2026 etan68
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Privileged Lenovo EC provider for hwdash-helper. The register pair is the
 * same one used by P3FanMonitor. Reads are coherent and never write EC RAM.
 */

#include "../helper_provider.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_EC_PATH "/sys/kernel/debug/ec/ec0/io"
#define DEV_PORT_PATH "/dev/port"
#define DEFAULT_OUTPUT_PATH "/run/hwdash/lenovo-cpu-fan-rpm"
#define LOCK_DIRECTORY "/run/hwdash"
#define LOCK_PATH LOCK_DIRECTORY "/lenovo-ec.lock"
#define SYS_VENDOR_PATH "/sys/class/dmi/id/sys_vendor"
#define EC_STATUS_PORT 0x66
#define EC_DATA_PORT 0x62
#define EC_READ_COMMAND 0x80
#define EC_INPUT_BUFFER_FULL 0x02
#define EC_OUTPUT_BUFFER_FULL 0x01
#define RPM_LOW_OFFSET 0x09
#define RPM_HIGH_OFFSET 0x0a
#define MAX_RPM 30000u

struct lenovo_ec_context {
  int ec_fd;
  int lock_fd;
  bool direct_ports;
};

static bool read_byte_at(int fd, off_t offset, unsigned char *value) {
  ssize_t count;
  do {
    count = pread(fd, value, 1u, offset);
  } while (count < 0 && errno == EINTR);
  return count == 1;
}

static bool write_byte_at(int fd, off_t offset, unsigned char value) {
  ssize_t count;
  do {
    count = pwrite(fd, &value, 1u, offset);
  } while (count < 0 && errno == EINTR);
  return count == 1;
}

static bool wait_port_status(int fd, unsigned char mask, unsigned char expected) {
  for (unsigned attempt = 0; attempt < 100u; ++attempt) {
    unsigned char status = 0;
    if (!read_byte_at(fd, EC_STATUS_PORT, &status))
      return false;
    if ((status & mask) == expected)
      return true;
    struct timespec delay = {.tv_sec = 0, .tv_nsec = 1000000};
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
      ;
  }
  errno = ETIMEDOUT;
  return false;
}

static bool read_register_from_ports(int fd, unsigned char offset, unsigned char *value) {
  unsigned char status = 0;
  if (!read_byte_at(fd, EC_STATUS_PORT, &status) || (status & 3u) != 0u) {
    errno = EBUSY;
    return false;
  }
  return write_byte_at(fd, EC_STATUS_PORT, EC_READ_COMMAND) &&
         wait_port_status(fd, EC_INPUT_BUFFER_FULL, 0u) &&
         write_byte_at(fd, EC_DATA_PORT, offset) &&
         wait_port_status(fd, EC_OUTPUT_BUFFER_FULL, EC_OUTPUT_BUFFER_FULL) &&
         read_byte_at(fd, EC_DATA_PORT, value);
}

static bool read_register(const struct lenovo_ec_context *context, unsigned char offset, unsigned char *value) {
  return context->direct_ports ? read_register_from_ports(context->ec_fd, offset, value)
                               : read_byte_at(context->ec_fd, offset, value);
}

static bool read_rpm(const struct lenovo_ec_context *context, unsigned *rpm) {
  unsigned char high_before = 0, low = 0, high_after = 0;
  if (!read_register(context, RPM_HIGH_OFFSET, &high_before) ||
      !read_register(context, RPM_LOW_OFFSET, &low) ||
      !read_register(context, RPM_HIGH_OFFSET, &high_after) || high_before != high_after)
    return false;
  const unsigned value = ((unsigned)high_after << 8u) | (unsigned)low;
  if (value == 0xffffu || value > MAX_RPM)
    return false;
  *rpm = value;
  return true;
}

static bool is_lenovo(void) {
  FILE *file = fopen(SYS_VENDOR_PATH, "r");
  if (!file)
    return false;
  char vendor[128];
  const bool read = fgets(vendor, sizeof(vendor), file) != NULL;
  fclose(file);
  if (!read)
    return false;
  for (char *cursor = vendor; *cursor; ++cursor)
    if (*cursor >= 'a' && *cursor <= 'z')
      *cursor = (char)(*cursor - 'a' + 'A');
  return strstr(vendor, "LENOVO") != NULL;
}

static bool provider_open(const char *source_override, void **opaque) {
  struct lenovo_ec_context *context = calloc(1u, sizeof(*context));
  if (!context)
    return false;
  context->ec_fd = -1;
  context->lock_fd = -1;
  const char *source = source_override ? source_override : DEFAULT_EC_PATH;
  context->ec_fd = open(source, O_RDONLY | O_CLOEXEC);
  const int debugfs_error = errno;
  if (context->ec_fd < 0 && !source_override) {
    context->ec_fd = open(DEV_PORT_PATH, O_RDWR | O_CLOEXEC);
    context->direct_ports = context->ec_fd >= 0;
  }
  if (context->ec_fd < 0) {
    if (source_override)
      fprintf(stderr, "Cannot open %s: %s\n", source, strerror(errno));
    else
      fprintf(stderr, "Cannot open %s (%s) or %s (%s)\n", DEFAULT_EC_PATH, strerror(debugfs_error), DEV_PORT_PATH,
              strerror(errno));
    free(context);
    return false;
  }
  if (context->direct_ports) {
    if (mkdir(LOCK_DIRECTORY, 0755) != 0 && errno != EEXIST)
      goto fail;
    context->lock_fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (context->lock_fd < 0 || flock(context->lock_fd, LOCK_EX | LOCK_NB) != 0)
      goto fail;
  }
  *opaque = context;
  return true;

fail:
  fprintf(stderr, "Cannot lock Lenovo EC access: %s\n", strerror(errno));
  if (context->lock_fd >= 0)
    close(context->lock_fd);
  close(context->ec_fd);
  free(context);
  return false;
}

static bool provider_sample(void *opaque, char *value, size_t size) {
  struct lenovo_ec_context *context = opaque;
  unsigned rpm = 0;
  if (!context || !read_rpm(context, &rpm))
    return false;
  const int length = snprintf(value, size, "%u\n", rpm);
  return length > 0 && (size_t)length < size;
}

static void provider_close(void *opaque) {
  struct lenovo_ec_context *context = opaque;
  if (!context)
    return;
  if (context->lock_fd >= 0)
    close(context->lock_fd);
  if (context->ec_fd >= 0)
    close(context->ec_fd);
  free(context);
}

const struct hwdash_helper_provider hwdash_lenovo_ec_provider = {
    .name = "lenovo-cpu-fan",
    .default_output_path = DEFAULT_OUTPUT_PATH,
    .is_supported = is_lenovo,
    .open = provider_open,
    .sample = provider_sample,
    .close = provider_close,
};
