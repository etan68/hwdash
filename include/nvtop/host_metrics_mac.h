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

#ifndef HOST_METRICS_MAC_H__
#define HOST_METRICS_MAC_H__

#include <stdbool.h>
#include <stdint.h>

#include "nvtop/host_metrics.h"

// macOS host metrics, read from the Mach host ports and the sysctl MIB and
// never from a /proc compatibility layer. The sampling lives in
// src/collectors/host/host_metrics_mac.c; the conversions below are pure, so
// their arithmetic, bounds and memory model are testable without a machine
// whose memory pressure is known.

// The memory counters as host_statistics64 and sysctl hand them out. The model
// the collector uses:
//
//   used      = (internal - purgeable) + wired + compressor
//   available = total - used
//
// internal is the resident anonymous memory and purgeable the part of it a
// caller declared discardable, so purgeable is subtracted instead of being
// counted a second time on the available side. wired is the memory the kernel
// pinned and compressor the space the compressed pager occupies in RAM; the
// pages the compressor holds uncompressed are the content of those pages and
// are charged nowhere, which is the double count this avoids. The kernel
// accounts the speculative pages inside the free ones, so available is derived
// from total - used rather than by summing the free side.
//
// Unlike Linux MemAvailable, available here is total minus the chosen used
// formula, without kernel reserve or reclaim heuristics. It is not a memory
// pressure measure. Swap usage is reported separately.
struct host_mac_memory_pages {
  uint64_t page_size;                     // Bytes in a page, from the kernel
  uint64_t total_bytes;                   // Physical memory, hw.memsize
  uint64_t wired_pages;                   // wire_count
  uint64_t internal_pages;                // internal_page_count, anonymous and resident
  uint64_t purgeable_pages;               // purgeable_count, a subset of internal_pages
  uint64_t compressor_pages;              // compressor_page_count, occupied in RAM
  uint64_t compressor_uncompressed_pages; // total_uncompressed_pages_in_compressor, not charged
};

// Convert the page counters into the KiB pair the collector displays. Every
// combination that cannot be true of one snapshot is refused, so an
// inconsistent answer reads unavailable instead of a plausible usage: no page
// size, no memory, a page larger than the machine, a purgeable part bigger than
// the anonymous whole, counters whose sum cannot be represented, and a usage
// larger than the machine it is charged to.
bool host_mac_memory_convert(const struct host_mac_memory_pages *pages, struct host_memory_info *info);

// Convert the vm.swapusage pair into the KiB pair the collector displays. A
// kernel that answers total and used both zero has no swap allocated right now
// and converts to the same zero pair. A usage larger than the swap, and a total
// that is not zero but does not reach one KiB, are refused.
bool host_mac_swap_convert(uint64_t total_bytes, uint64_t used_bytes, struct host_swap_info *info);

// The swap pair as the macOS collector displays it. A kernel that answered none
// allocated is a usable 0 GiB of 0 GiB rather than the unavailable swap the Linux
// reading of a zero total makes, which host_swap_usage keeps unchanged: every
// other pair goes through it. A pair that is not trustworthy is refused, so that
// the swap reads N/A rather than a plausible usage.
bool host_mac_swap_display(const struct host_swap_info *swap, double *used_gib, double *total_gib);

// The memory of the machine: hw.memsize and the host_statistics64 counters,
// converted by the model above. The send right taken from the host port is
// given back before returning.
bool hwdash_host_mac_memory(struct host_memory_info *info);

// The swap of the machine, from vm.swapusage. Some configurations answer it for
// the superuser only: a kernel that does not answer leaves the swap
// unavailable, which reads N/A, which is not the same thing as a swap of zero.
bool hwdash_host_mac_swap(struct host_swap_info *info);

// The number of cumulative tick states the kernel answers for a processor:
// CPU_STATE_MAX of <mach/processor_info.h>, user, nice, system and idle. Spelled
// out here so the counters can be handed to the conversion below without the
// kernel header in hand.
#define HOST_MAC_CPU_STATE_COUNT 4u

// The cumulative tick counters of one processor since it started, widened out
// of the signed 32 bits of the kernel reply by way of the unsigned 32 bits of
// the same width: a count that passes 2^31 keeps counting up instead of becoming
// a negative number. The wrap of the 32 bits themselves stays a counter that
// went backward.
struct host_mac_cpu_ticks {
  uint64_t tick[HOST_MAC_CPU_STATE_COUNT]; // In the order the kernel answers them
};

// The whole host utilization out of two samples of those counters, one sample
// per processor, in the same order and the same number on both sides. user,
// nice and system are the busy states, idle the only idle one, and the rate is
// the sum of the busy deltas over the sum of the elapsed deltas.
//
// An interval that cannot have happened is refused rather than rounded into a
// plausible number: no reference sample, a processor count that differs between
// the samples (a hotplug, a wake up with another topology), a counter that went
// backward, more idle ticks than elapsed ones on a processor, an interval in
// which no tick moved, or a sum that cannot be represented.
bool host_mac_cpu_ticks_utilization(const struct host_mac_cpu_ticks *previous, unsigned previous_count,
                                    const struct host_mac_cpu_ticks *current, unsigned current_count, double *percent);

// The reference the CPU rate of one host_metrics_state is taken against: the
// last tick sample of every processor. It is per state, so two samplers each
// measure the interval between their own two samples and a released state takes
// nobody else's reference with it.
struct host_mac_cpu_reference;

// Build the reference of `state` from a first sample, so that the next sample of
// the same state already reports a rate. Returns false when the kernel gave no
// tick counter at all, which leaves the reference of this state unset.
bool hwdash_host_mac_cpu_seed(struct host_metrics_state *state);

// Sample the tick counters and report the utilization of the interval since the
// previous sample of this same state. The first sample of a state has no
// interval to measure and reports nothing. Another processor count or a counter
// that went backward costs one refresh and rebuilds the reference.
bool hwdash_host_mac_cpu_sample(struct host_metrics_state *state, double *percent);

// Give back the reference `state` holds and clear the pointer. Idempotent, and
// it leaves every other state as it was.
void hwdash_host_mac_cpu_release(struct host_metrics_state *state);

// The identity of the CPU: the brand, the physical core count and the logical
// thread count of the sysctl MIB. A field the kernel does not answer is left
// invalid, reported N/A and never guessed from a neighbouring field.
bool hwdash_host_mac_identity(struct host_cpu_identity *identity);

// The 1, 5 and 15 minute load averages.
bool hwdash_host_mac_load_average(double load_avg[3]);

#endif // HOST_METRICS_MAC_H__
