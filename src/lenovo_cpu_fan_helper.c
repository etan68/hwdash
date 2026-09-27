/*
 * Copyright (C) 2026 etan68
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Read the Lenovo CPU fan tachometer from the kernel ACPI EC interface and
 * publish only the sanitized RPM value.
 *
 * The Lenovo firmware stores the 16-bit RPM at EC 0x0a:0x09.  This is the
 * same register pair used by P3FanMonitor.  The high byte is read twice so a
 * rollover cannot combine bytes from two different samples.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
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

static volatile sig_atomic_t stopping = 0;

static void stop_handler(int signal_number) {
  (void)signal_number;
  stopping = 1;
}

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

// RD_EC is a read handshake: the two port writes select the read command and
// register address. No WR_EC command and no write to EC RAM exists here.
static bool read_register_from_ports(int fd, unsigned char offset, unsigned char *value) {
  unsigned char status = 0;
  if (!read_byte_at(fd, EC_STATUS_PORT, &status) || (status & 3u) != 0u) {
    errno = EBUSY;
    return false;
  }
  if (!write_byte_at(fd, EC_STATUS_PORT, EC_READ_COMMAND) ||
      !wait_port_status(fd, EC_INPUT_BUFFER_FULL, 0u) || !write_byte_at(fd, EC_DATA_PORT, offset) ||
      !wait_port_status(fd, EC_OUTPUT_BUFFER_FULL, EC_OUTPUT_BUFFER_FULL) ||
      !read_byte_at(fd, EC_DATA_PORT, value))
    return false;
  return true;
}

static bool read_register(int fd, bool direct_ports, unsigned char offset, unsigned char *value) {
  return direct_ports ? read_register_from_ports(fd, offset, value) : read_byte_at(fd, offset, value);
}

static bool read_rpm(int fd, bool direct_ports, unsigned *rpm) {
  unsigned char high_before = 0, low = 0, high_after = 0;
  if (!read_register(fd, direct_ports, RPM_HIGH_OFFSET, &high_before) ||
      !read_register(fd, direct_ports, RPM_LOW_OFFSET, &low) ||
      !read_register(fd, direct_ports, RPM_HIGH_OFFSET, &high_after) || high_before != high_after)
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

static bool publish_rpm(const char *path, unsigned rpm) {
  char temporary[512];
  if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(temporary)) {
    errno = ENAMETOOLONG;
    return false;
  }
  int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0)
    return false;
  char value[32];
  const int length = snprintf(value, sizeof(value), "%u\n", rpm);
  ssize_t written = write(fd, value, (size_t)length);
  const int saved_errno = errno;
  if (close(fd) != 0 && written == length) {
    unlink(temporary);
    return false;
  }
  if (written != length) {
    unlink(temporary);
    errno = written < 0 ? saved_errno : EIO;
    return false;
  }
  if (rename(temporary, path) != 0) {
    unlink(temporary);
    return false;
  }
  return true;
}

static void usage(const char *program) {
  fprintf(stderr, "Usage: %s [--once] [--force] [--source PATH] [--output PATH]\n", program);
}

int main(int argc, char **argv) {
  const char *source = DEFAULT_EC_PATH;
  const char *output = DEFAULT_OUTPUT_PATH;
  bool once = false, force = false, source_overridden = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--once") == 0)
      once = true;
    else if (strcmp(argv[i], "--force") == 0)
      force = true;
    else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc) {
      source = argv[++i];
      source_overridden = true;
    }
    else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
      output = argv[++i];
    else {
      usage(argv[0]);
      return 2;
    }
  }

  if (!force && !is_lenovo()) {
    fprintf(stderr, "Refusing Lenovo EC offsets on a non-Lenovo machine; use --force only for a test fixture.\n");
    return 1;
  }
  bool direct_ports = false;
  int ec = open(source, O_RDONLY | O_CLOEXEC);
  int debugfs_error = errno;
  if (ec < 0 && !source_overridden) {
    ec = open(DEV_PORT_PATH, O_RDWR | O_CLOEXEC);
    direct_ports = ec >= 0;
  }
  if (ec < 0) {
    if (source_overridden)
      fprintf(stderr, "Cannot open %s: %s\n", source, strerror(errno));
    else
      fprintf(stderr, "Cannot open %s (%s) or %s (%s)\n", DEFAULT_EC_PATH, strerror(debugfs_error), DEV_PORT_PATH,
              strerror(errno));
    return 1;
  }

  int lock_fd = -1;
  if (direct_ports) {
    if (mkdir(LOCK_DIRECTORY, 0755) != 0 && errno != EEXIST) {
      fprintf(stderr, "Cannot create %s: %s\n", LOCK_DIRECTORY, strerror(errno));
      close(ec);
      return 1;
    }
    lock_fd = open(LOCK_PATH, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock_fd < 0) {
      fprintf(stderr, "Cannot open %s: %s\n", LOCK_PATH, strerror(errno));
      close(ec);
      return 1;
    }
    if (flock(lock_fd, LOCK_EX | LOCK_NB) != 0) {
      fprintf(stderr, "Another process is reading the Lenovo EC.\n");
      close(lock_fd);
      close(ec);
      return 1;
    }
  }

  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = stop_handler;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);

  int result = 0;
  do {
    unsigned rpm = 0;
    if (read_rpm(ec, direct_ports, &rpm)) {
      if (!publish_rpm(output, rpm)) {
        fprintf(stderr, "Cannot publish %s: %s\n", output, strerror(errno));
        result = 1;
        break;
      }
    } else {
      // Never leave a stale valid-looking speed behind when the EC read is not
      // coherent. hwdash also rejects an old file if this process is killed.
      unlink(output);
      if (once) {
        fprintf(stderr, "No coherent CPU fan RPM in EC offsets 0x0a:0x09.\n");
        result = 1;
      }
    }
    if (!once && !stopping) {
      struct timespec delay = {.tv_sec = 1, .tv_nsec = 0};
      while (nanosleep(&delay, &delay) != 0 && errno == EINTR && !stopping)
        ;
    }
  } while (!once && !stopping);

  close(ec);
  if (lock_fd >= 0)
    close(lock_fd);
  if (!once)
    unlink(output);
  return result;
}
