/*
 *
 * Copyright (C) 2026 etan68
 *
 * This file is part of HWDash.
 *
 * HWDash is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * HWDash is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with HWDash.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

// macOS host collector: the whole host CPU, its identity, the memory, the swap
// and the load averages, from the native interfaces of the kernel.
//
// Plain Mach and BSD API rather than Objective-C or Metal: host_processor_info,
// host_statistics64 and the sysctl MIB answer the whole host, and none of them
// needs a privilege this process does not already have. No private or entitled
// interface is used and no external tool is run, so what the kernel does not
// hand out to an unprivileged process - the live CPU frequency, the package
// power, a fan - is simply not reported.

#include "nvtop/host_metrics_mac.h"

#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/processor_info.h>
#include <mach/vm_page_size.h>
#include <mach/vm_types.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

#define HOST_MAC_BYTES_PER_KIB 1024u
// More processors than that is not a machine but a malformed reply.
#define HOST_MAC_MAX_PROCESSORS 4096u

static bool mac_checked_add(uint64_t *accumulator, uint64_t value) {
  if (value > 0xFFFFFFFFFFFFFFFFull - *accumulator)
    return false;
  *accumulator += value;
  return true;
}

static bool mac_checked_mul(uint64_t *value, uint64_t factor) {
  if (*value != 0 && factor > 0xFFFFFFFFFFFFFFFFull / *value)
    return false;
  *value *= factor;
  return true;
}

static uint64_t bytes_to_kib(uint64_t bytes) { return bytes / HOST_MAC_BYTES_PER_KIB; }

// The memory ///////////////////////////////////////////////////
// The model and why the figure is not the Linux one are written down in
// nvtop/host_metrics_mac.h. Only the arithmetic and its bounds live here.

bool host_mac_memory_convert(const struct host_mac_memory_pages *pages, struct host_memory_info *info) {
  if (!pages || !info)
    return false;
  if (pages->page_size == 0 || pages->total_bytes == 0)
    return false;
  // A page larger than the whole machine holds nothing in it.
  if (pages->page_size > pages->total_bytes)
    return false;

  // purgeable is a part of internal. An answer where the part is bigger than the
  // whole is a torn snapshot rather than a memory model, and it is refused: what
  // is known about it is that it is discardable, so charging the whole would be
  // the wrong side of the doubt and charging nothing would be a plausible low
  // usage out of counters that cannot be true together.
  if (pages->purgeable_pages > pages->internal_pages)
    return false;
  const uint64_t app_pages = pages->internal_pages - pages->purgeable_pages;
  // The pages the compressor holds uncompressed are the content of the pages
  // already paid for in compressor_pages: they are charged nowhere.
  uint64_t used_pages = app_pages;
  if (!mac_checked_add(&used_pages, pages->wired_pages))
    return false;
  if (!mac_checked_add(&used_pages, pages->compressor_pages))
    return false;

  // The machine bounds the charge: an answer above it does not come from one
  // coherent snapshot.
  const uint64_t total_pages = pages->total_bytes / pages->page_size;
  if (total_pages == 0 || used_pages > total_pages)
    return false;

  uint64_t used_bytes = used_pages;
  if (!mac_checked_mul(&used_bytes, pages->page_size))
    return false;
  if (used_bytes > pages->total_bytes)
    return false;

  const uint64_t total_kib = bytes_to_kib(pages->total_bytes);
  const uint64_t used_kib = bytes_to_kib(used_bytes);
  if (total_kib == 0 || used_kib > total_kib)
    return false;

  info->total_kib = total_kib;
  info->available_kib = total_kib - used_kib;
  return true;
}

// vm.swapusage answers two byte counts, total and used, and the collector keeps
// a total and a free one. A kernel that answers zero and zero has no swap
// allocated right now, which is an answer and not a hole; a total that is not
// zero but does not reach one KiB would be indistinguishable from it and is
// refused, as is a used above the total.
bool host_mac_swap_convert(uint64_t total_bytes, uint64_t used_bytes, struct host_swap_info *info) {
  if (!info)
    return false;
  if (used_bytes > total_bytes)
    return false;
  const uint64_t total_kib = bytes_to_kib(total_bytes);
  if (total_bytes != 0 && total_kib == 0)
    return false;
  const uint64_t used_kib = bytes_to_kib(used_bytes);
  if (used_kib > total_kib)
    return false;
  info->total_kib = total_kib;
  info->free_kib = total_kib - used_kib;
  return true;
}

bool host_mac_swap_display(const struct host_swap_info *swap, double *used_gib, double *total_gib) {
  if (!swap || !used_gib || !total_gib)
    return false;
  if (swap->total_kib != 0)
    return host_swap_usage(swap, used_gib, total_gib);
  // None allocated is an answer the interface displays as 0/0. Everything else
  // about that pair is as inconsistent here as it is anywhere else.
  if (swap->free_kib != 0)
    return false;
  *used_gib = 0.;
  *total_gib = 0.;
  return true;
}

bool hwdash_host_mac_memory(struct host_memory_info *info) {
  if (!info)
    return false;

  uint64_t total_bytes = 0;
  size_t size = sizeof(total_bytes);
  if (sysctlbyname("hw.memsize", &total_bytes, &size, NULL, 0) != 0 || size != sizeof(total_bytes) || total_bytes == 0)
    return false;

  vm_statistics64_data_t stats;
  memset(&stats, 0, sizeof(stats));
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  const mach_port_t host = mach_host_self();
  if (!MACH_PORT_VALID(host))
    return false;
  const kern_return_t status = host_statistics64(host, HOST_VM_INFO64, (host_info_t)&stats, &count);
  // The send right mach_host_self() hands out is this process' reference to the
  // host port: giving it back is what keeps a refresh, once per redraw, from
  // leaking one right every time.
  mach_port_deallocate(mach_task_self(), host);
  if (status != KERN_SUCCESS)
    return false;

  struct host_mac_memory_pages pages;
  memset(&pages, 0, sizeof(pages));
  pages.page_size = vm_page_size;
  pages.total_bytes = total_bytes;
  pages.wired_pages = (uint64_t)stats.wire_count;
  pages.internal_pages = (uint64_t)stats.internal_page_count;
  pages.purgeable_pages = (uint64_t)stats.purgeable_count;
  pages.compressor_pages = (uint64_t)stats.compressor_page_count;
  pages.compressor_uncompressed_pages = stats.total_uncompressed_pages_in_compressor;
  return host_mac_memory_convert(&pages, info);
}

bool hwdash_host_mac_swap(struct host_swap_info *info) {
  if (!info)
    return false;
  struct xsw_usage usage;
  memset(&usage, 0, sizeof(usage));
  size_t size = sizeof(usage);
  // A kernel that will not answer - a restricted session, a sandbox - leaves the
  // swap unavailable, which reads N/A. That is not the same thing as the kernel
  // answering that it has none allocated, which is a real answer.
  if (sysctlbyname("vm.swapusage", &usage, &size, NULL, 0) != 0 || size != sizeof(usage))
    return false;
  return host_mac_swap_convert(usage.xsu_total, usage.xsu_used, info);
}

// The whole host CPU utilization ///////////////////////////////
// PROCESSOR_CPU_LOAD_INFO answers a cumulative tick count per processor and per
// state since boot, so the reference of a rate is kept per processor and per
// sampling state: a delta is always taken between two samples of the same
// processor of the same state, never between two aggregates.

_Static_assert(CPU_STATE_MAX == HOST_MAC_CPU_STATE_COUNT, "the kernel answers another number of tick states");

struct host_mac_cpu_reference {
  struct host_mac_cpu_ticks *tick; // One sample per processor, owned by this
  unsigned count;
};

void hwdash_host_mac_cpu_release(struct host_metrics_state *state) {
  if (!state || !state->mac_cpu_reference)
    return;
  free(state->mac_cpu_reference->tick);
  free(state->mac_cpu_reference);
  state->mac_cpu_reference = NULL;
}

// The reference of `state` becomes `sample`, whose ownership the collector takes
// with it.
static bool cpu_reference_adopt(struct host_metrics_state *state, struct host_mac_cpu_ticks *sample, unsigned count) {
  struct host_mac_cpu_reference *reference = state->mac_cpu_reference;
  if (!reference) {
    reference = calloc(1, sizeof(*reference));
    if (!reference) {
      free(sample);
      return false;
    }
    state->mac_cpu_reference = reference;
  }
  free(reference->tick);
  reference->tick = sample;
  reference->count = count;
  return true;
}

// The utilization out of two samples of the counters. Only the math is here, on
// the samples that were given, so an impossible interval, a restarted counter or
// a machine that answered another number of processors can all be checked on a
// table rather than on a machine whose counters had to be put in that state.
bool host_mac_cpu_ticks_utilization(const struct host_mac_cpu_ticks *previous, unsigned previous_count,
                                    const struct host_mac_cpu_ticks *current, unsigned current_count, double *percent) {
  if (!previous || !current || !percent)
    return false;
  // Two samples of another number of processors are samples of two different
  // machines - a hotplug, a wake up with another topology. No reference is the
  // first sample of a state.
  if (previous_count == 0 || previous_count != current_count)
    return false;

  uint64_t total_delta = 0, idle_delta = 0;
  for (unsigned cpu = 0; cpu < previous_count; ++cpu) {
    uint64_t cpu_total = 0, cpu_idle = 0;
    for (unsigned state = 0; state < HOST_MAC_CPU_STATE_COUNT; ++state) {
      const uint64_t before = previous[cpu].tick[state];
      const uint64_t after = current[cpu].tick[state];
      // A counter that went backward is not the same counter anymore, be it a
      // reset, a processor that restarted or a machine that woke up with another
      // clock. The rate of two different counters would be a plausible number
      // about a machine that never ran it.
      if (after < before)
        return false;
      const uint64_t delta = after - before;
      if (!mac_checked_add(&cpu_total, delta))
        return false;
      if (state == CPU_STATE_IDLE && !mac_checked_add(&cpu_idle, delta))
        return false;
    }
    // user, nice and system are the busy states and idle the only idle one: an
    // interval with more idle than elapsed ticks cannot have happened.
    if (cpu_idle > cpu_total)
      return false;
    if (!mac_checked_add(&total_delta, cpu_total) || !mac_checked_add(&idle_delta, cpu_idle))
      return false;
  }

  // No elapsed tick at all - the two samples landed in the same tick - is neither
  // an idle machine nor a busy one: it is no sample.
  if (total_delta == 0)
    return false;

  double utilization = 100. * (double)(total_delta - idle_delta) / (double)total_delta;
  if (!isfinite(utilization))
    return false;
  if (utilization < 0.)
    utilization = 0.;
  if (utilization > 100.)
    utilization = 100.;
  *percent = utilization;
  return true;
}

// One snapshot of the tick counters of every processor. The Mach allocation the
// reply arrives in is copied out and given back to the kernel here: the array
// belongs to the kernel, not to this process.
static bool cpu_read_sample(struct host_mac_cpu_ticks **sample, unsigned *count) {
  if (!sample || !count)
    return false;
  *sample = NULL;
  *count = 0;

  natural_t processor_count = 0;
  processor_info_array_t info = NULL;
  mach_msg_type_number_t info_count = 0;
  const mach_port_t host = mach_host_self();
  if (!MACH_PORT_VALID(host))
    return false;
  const kern_return_t status = host_processor_info(host, PROCESSOR_CPU_LOAD_INFO, &processor_count, &info, &info_count);
  mach_port_deallocate(mach_task_self(), host);

  const uint64_t reply_length = (uint64_t)info_count;
  if (status != KERN_SUCCESS || info == NULL) {
    if (info != NULL)
      vm_deallocate(mach_task_self(), (vm_address_t)info, (vm_size_t)(reply_length * sizeof(integer_t)));
    return false;
  }

  // The reply is one processor_cpu_load_info_data_t per processor, four ticks
  // each. Anything else is not a topology this collector can sample.
  if (processor_count == 0 || processor_count > HOST_MAC_MAX_PROCESSORS || reply_length == 0 ||
      reply_length % CPU_STATE_MAX != 0 || reply_length / CPU_STATE_MAX != (uint64_t)processor_count) {
    vm_deallocate(mach_task_self(), (vm_address_t)info, (vm_size_t)(reply_length * sizeof(integer_t)));
    return false;
  }

  struct host_mac_cpu_ticks *read = calloc(processor_count, sizeof(*read));
  if (!read) {
    vm_deallocate(mach_task_self(), (vm_address_t)info, (vm_size_t)(info_count * sizeof(integer_t)));
    return false;
  }
  for (natural_t cpu = 0; cpu < processor_count; ++cpu) {
    const processor_cpu_load_info_data_t *load = (const processor_cpu_load_info_data_t *)&info[cpu * CPU_STATE_MAX];
    for (unsigned state = 0; state < CPU_STATE_MAX; ++state)
      // The ticks are an `integer_t`, a signed 32 bit count. Read as a signed
      // count, a processor up for long enough to pass 2^31 ticks would jump
      // forward by the whole range of the type and the interval around the
      // crossing would be a rate out of a delta that never happened. Read as the
      // 32 bit counter it is, the crossing is two ticks and the wrap of the type
      // itself is a counter that went backward, which rebuilds the reference.
      read[cpu].tick[state] = (uint32_t)load->cpu_ticks[state];
  }
  vm_deallocate(mach_task_self(), (vm_address_t)info, (vm_size_t)(info_count * sizeof(integer_t)));

  *sample = read;
  *count = (unsigned)processor_count;
  return true;
}

bool hwdash_host_mac_cpu_seed(struct host_metrics_state *state) {
  if (!state)
    return false;
  struct host_mac_cpu_ticks *sample = NULL;
  unsigned count = 0;
  if (!cpu_read_sample(&sample, &count)) {
    hwdash_host_mac_cpu_release(state);
    return false;
  }
  return cpu_reference_adopt(state, sample, count);
}

bool hwdash_host_mac_cpu_sample(struct host_metrics_state *state, double *percent) {
  if (!state || !percent)
    return false;

  struct host_mac_cpu_ticks *sample = NULL;
  unsigned count = 0;
  if (!cpu_read_sample(&sample, &count)) {
    // Nothing could be read: whatever reference this state had has no pair left.
    hwdash_host_mac_cpu_release(state);
    return false;
  }

  const struct host_mac_cpu_reference *reference = state->mac_cpu_reference;
  const bool measured = host_mac_cpu_ticks_utilization(reference ? reference->tick : NULL,
                                                       reference ? reference->count : 0u, sample, count, percent);

  // The reference becomes the sample that was just read, whatever the interval
  // gave: a restarted counter, another topology or an interval too short to move
  // a tick costs one refresh and no more, and the rates that follow are the
  // rates of the intervals they are really measured on.
  if (!cpu_reference_adopt(state, sample, count))
    return false;
  return measured;
}

// The identity of the CPU //////////////////////////////////////

static bool read_sysctl_u32(const char *name, uint32_t *value) {
  uint32_t read = 0;
  size_t size = sizeof(read);
  if (sysctlbyname(name, &read, &size, NULL, 0) != 0 || size != sizeof(read))
    return false;
  *value = read;
  return true;
}

static bool read_sysctl_string(const char *name, char *buffer, size_t size) {
  char read[HOST_CPU_MODEL_MAX_LENGTH + 32u];
  size_t length = sizeof(read);
  if (sysctlbyname(name, read, &length, NULL, 0) != 0 || length == 0)
    return false;
  read[sizeof(read) - 1u] = '\0';
  snprintf(buffer, size, "%s", read);
  return buffer[0] != '\0';
}

bool hwdash_host_mac_identity(struct host_cpu_identity *identity) {
  if (!identity)
    return false;
  memset(identity, 0, sizeof(*identity));

  // The brand the kernel gives the part, or the board identifier where the
  // machine does not answer the first.
  if (read_sysctl_string("machdep.cpu.brand_string", identity->model, sizeof(identity->model)))
    identity->model_valid = true;
  else if (read_sysctl_string("hw.model", identity->model, sizeof(identity->model)))
    identity->model_valid = true;

  // hw.activecpu is the processors online right now, which is what the
  // utilization is measured over; hw.logicalcpu the processors this boot can
  // run threads on; hw.physicalcpu the cores behind them. The threads are the
  // greater count on a machine with thread level parallelism and the two are
  // equal without it.
  uint32_t logical = 0;
  if (!read_sysctl_u32("hw.activecpu", &logical) || logical == 0)
    read_sysctl_u32("hw.logicalcpu", &logical);
  if (logical == 0)
    read_sysctl_u32("hw.ncpu", &logical);
  if (logical > 0 && logical <= HOST_MAC_MAX_PROCESSORS)
    identity->logical_threads = logical;

  // The core count is only ever what the kernel answered for it. The threads are
  // not a substitute: on a machine with thread level parallelism they are twice
  // the cores, so a missing hw.physicalcpu leaves the cores unknown rather than
  // prints the thread count as a core count.
  uint32_t physical = 0;
  if (read_sysctl_u32("hw.physicalcpu", &physical) && physical > 0 && physical <= HOST_MAC_MAX_PROCESSORS &&
      (identity->logical_threads == 0 || physical <= identity->logical_threads))
    identity->physical_cores = physical;

  return identity->model_valid || identity->physical_cores > 0 || identity->logical_threads > 0;
}

bool hwdash_host_mac_load_average(double load_avg[3]) {
  if (!load_avg)
    return false;
  double sample[3] = {0., 0., 0.};
  // The same three numbers uptime prints and /proc/loadavg holds on Linux. The
  // remaining fields of the Linux line have no macOS equivalent and are not
  // needed here.
  if (getloadavg(sample, 3) != 3)
    return false;
  for (unsigned i = 0; i < 3; ++i)
    if (!isfinite(sample[i]) || sample[i] < 0.)
      return false;
  for (unsigned i = 0; i < 3; ++i)
    load_avg[i] = sample[i];
  return true;
}
