#ifndef HWDASH_VENDOR_LENOVO_CPU_FAN_H__
#define HWDASH_VENDOR_LENOVO_CPU_FAN_H__

#include <stdbool.h>

// Unprivileged Lenovo-specific adapter. It never accesses the embedded
// controller directly; it only validates data published by hwdash-helper.
struct hwdash_lenovo_cpu_fan {
  unsigned rpm;
  bool valid;
  bool supported;
  bool support_probed;
  unsigned probe_failures;
};

bool hwdash_lenovo_cpu_fan_parse_rpm(const char *text, unsigned *rpm);
void hwdash_lenovo_cpu_fan_reset(struct hwdash_lenovo_cpu_fan *fan);
void hwdash_lenovo_cpu_fan_update(struct hwdash_lenovo_cpu_fan *fan, unsigned sample_count);

#endif
