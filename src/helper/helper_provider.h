#ifndef HWDASH_HELPER_PROVIDER_H__
#define HWDASH_HELPER_PROVIDER_H__

#include <stdbool.h>
#include <stddef.h>

// A privileged collector plugged into the generic helper. Providers own the
// hardware access; the helper owns lifecycle, polling and atomic publication.
struct hwdash_helper_provider {
  const char *name;
  const char *default_output_path;
  bool (*is_supported)(void);
  bool (*open)(const char *source_override, void **context);
  bool (*sample)(void *context, char *value, size_t size);
  void (*close)(void *context);
};

extern const struct hwdash_helper_provider hwdash_lenovo_ec_provider;

#endif
