/* Copyright (C) 2026 etan68
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "helper_provider.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define PROVIDER_COUNT (sizeof(providers) / sizeof(providers[0]))

static const struct hwdash_helper_provider *providers[] = {
    &hwdash_lenovo_ec_provider,
};

struct active_provider {
  const struct hwdash_helper_provider *provider;
  const char *output_path;
  void *context;
};

static volatile sig_atomic_t stopping = 0;

static void stop_handler(int signal_number) {
  (void)signal_number;
  stopping = 1;
}

static bool publish_value(const char *path, const char *value) {
  char temporary[512];
  if (snprintf(temporary, sizeof(temporary), "%s.tmp.%ld", path, (long)getpid()) >= (int)sizeof(temporary)) {
    errno = ENAMETOOLONG;
    return false;
  }
  int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0)
    return false;
  const size_t length = strlen(value);
  const ssize_t written = write(fd, value, length);
  const int saved_errno = errno;
  if (close(fd) != 0 && written == (ssize_t)length) {
    unlink(temporary);
    return false;
  }
  if (written != (ssize_t)length) {
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

static const struct hwdash_helper_provider *find_provider(const char *name) {
  for (size_t i = 0; i < PROVIDER_COUNT; ++i)
    if (strcmp(providers[i]->name, name) == 0)
      return providers[i];
  return NULL;
}

static void usage(const char *program) {
  fprintf(stderr,
          "Usage: %s [--once] [--force] [--provider NAME] [--source PATH] [--output PATH]\n",
          program);
}

int main(int argc, char **argv) {
  const char *selected_name = NULL;
  const char *source_override = NULL;
  const char *output_override = NULL;
  bool once = false;
  bool force = false;
  for (int i = 1; i < argc; ++i) {
    if (strcmp(argv[i], "--once") == 0)
      once = true;
    else if (strcmp(argv[i], "--force") == 0)
      force = true;
    else if (strcmp(argv[i], "--provider") == 0 && i + 1 < argc)
      selected_name = argv[++i];
    else if (strcmp(argv[i], "--source") == 0 && i + 1 < argc)
      source_override = argv[++i];
    else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc)
      output_override = argv[++i];
    else {
      usage(argv[0]);
      return 2;
    }
  }

  if ((source_override || output_override) && !selected_name && PROVIDER_COUNT > 1u) {
    fprintf(stderr, "--source and --output require --provider when more than one provider is built.\n");
    return 2;
  }
  if (selected_name && !find_provider(selected_name)) {
    fprintf(stderr, "Unknown helper provider: %s\n", selected_name);
    return 2;
  }

  struct active_provider active[PROVIDER_COUNT];
  size_t active_count = 0;
  for (size_t i = 0; i < PROVIDER_COUNT; ++i) {
    const struct hwdash_helper_provider *provider = providers[i];
    if (selected_name && strcmp(provider->name, selected_name) != 0)
      continue;
    if (!force && !provider->is_supported())
      continue;
    void *context = NULL;
    if (!provider->open(source_override, &context))
      return 1;
    active[active_count++] = (struct active_provider){
        .provider = provider,
        .output_path = output_override ? output_override : provider->default_output_path,
        .context = context,
    };
  }
  if (active_count == 0u) {
    if (selected_name)
      fprintf(stderr, "Provider %s is not supported on this machine.\n", selected_name);
    return selected_name || once ? 1 : 0;
  }

  struct sigaction action;
  memset(&action, 0, sizeof(action));
  action.sa_handler = stop_handler;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, NULL);
  sigaction(SIGTERM, &action, NULL);

  int result = 0;
  do {
    for (size_t i = 0; i < active_count; ++i) {
      char value[128];
      if (active[i].provider->sample(active[i].context, value, sizeof(value))) {
        if (!publish_value(active[i].output_path, value)) {
          fprintf(stderr, "Cannot publish %s: %s\n", active[i].output_path, strerror(errno));
          result = 1;
        }
      } else {
        unlink(active[i].output_path);
        if (once) {
          fprintf(stderr, "Provider %s returned no coherent sample.\n", active[i].provider->name);
          result = 1;
        }
      }
    }
    if (!once && !stopping) {
      struct timespec delay = {.tv_sec = 1, .tv_nsec = 0};
      while (nanosleep(&delay, &delay) != 0 && errno == EINTR && !stopping)
        ;
    }
  } while (!once && !stopping && result == 0);

  for (size_t i = 0; i < active_count; ++i) {
    active[i].provider->close(active[i].context);
    if (!once)
      unlink(active[i].output_path);
  }
  return result;
}
