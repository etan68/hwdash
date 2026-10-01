/*
 * Copyright (C) 2023 Robin Voetter <robin@voetter.nl>
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

#include "nvtop/common.h"
#include "nvtop/device_discovery.h"
#include "nvtop/extract_gpuinfo_common.h"
#include "nvtop/time.h"

#include <assert.h>
#include <Metal/Metal.h>
#include <IOKit/IOKitLib.h>
#include <QuartzCore/QuartzCore.h>
#include <math.h>

struct gpu_info_apple {
  struct gpu_info base;
  id<MTLDevice> device;
  io_service_t gpu_service;
};

static bool gpuinfo_apple_init(void);
static void gpuinfo_apple_shutdown(void);
static const char *gpuinfo_apple_last_error_string(void);
static bool gpuinfo_apple_get_device_handles(struct list_head *devices, unsigned *count);
static void gpuinfo_apple_populate_static_info(struct gpu_info *_gpu_info);
static void gpuinfo_apple_refresh_dynamic_info(struct gpu_info *_gpu_info);
static void gpuinfo_apple_get_running_processes(struct gpu_info *_gpu_info);

static struct gpu_vendor gpu_vendor_apple = {
  .init = gpuinfo_apple_init,
  .shutdown = gpuinfo_apple_shutdown,
  .last_error_string = gpuinfo_apple_last_error_string,
  .get_device_handles = gpuinfo_apple_get_device_handles,
  .populate_static_info = gpuinfo_apple_populate_static_info,
  .refresh_dynamic_info = gpuinfo_apple_refresh_dynamic_info,
  .refresh_running_processes = gpuinfo_apple_get_running_processes,
  .name = "apple",
};

static unsigned apple_gpu_count;
static struct gpu_info_apple *gpu_infos;

__attribute__((constructor)) static void init_extract_gpuinfo_apple(void) { register_gpu_vendor(&gpu_vendor_apple); }

static bool gpuinfo_apple_init(void) {
  apple_gpu_count = 0;
  gpu_infos = NULL;
  return true;
}

static void gpuinfo_apple_shutdown(void) {
  for (unsigned i = 0; i < apple_gpu_count; ++i) {
    struct gpu_info_apple *gpu_info = &gpu_infos[i];
    [gpu_info->device release];
    // A device the registry never answered for was kept with no service.
    if (MACH_PORT_VALID(gpu_info->gpu_service))
      IOObjectRelease(gpu_info->gpu_service);
  }

  free(gpu_infos);
  gpu_infos = NULL;
  apple_gpu_count = 0;
}

static const char *gpuinfo_apple_last_error_string(void) {
  return "An unanticipated error occurred while accessing Apple "
         "information\n";
}

static bool gpuinfo_apple_get_device_handles(struct list_head *devices, unsigned *count) {
  NSArray<id<MTLDevice>> *mtl_devices = MTLCopyAllDevices();

  const unsigned mtl_count = (unsigned)[mtl_devices count];
  if (mtl_count == 0) {
    [mtl_devices release];
    return false;
  }

  gpu_infos = calloc(mtl_count, sizeof(*gpu_infos));
  if (!gpu_infos) {
    [mtl_devices release];
    return false;
  }

  for (unsigned int i = 0; i < mtl_count; ++i) {
    id<MTLDevice> dev = mtl_devices[i];
    // The array the copy handed over is released below and the devices are read
    // again at every refresh, so the reference kept here has to be its own.
    [dev retain];
    const uint64_t registry_id = [dev registryID];
    const io_service_t gpu_service = IOServiceGetMatchingService(kIOMainPortDefault, IORegistryEntryIDMatching(registry_id));

    // The registry entry holds the performance statistics, not the device: a
    // machine that answers nothing here keeps a device whose name and memory can
    // still be asked of Metal, with the statistics left unavailable. The assert
    // that used to sit here went away with NDEBUG and a release build asked the
    // null port for its properties at every refresh.
    gpu_infos[apple_gpu_count].base.vendor = &gpu_vendor_apple;
    gpu_infos[apple_gpu_count].device = dev;
    gpu_infos[apple_gpu_count].gpu_service = MACH_PORT_VALID(gpu_service) ? gpu_service : IO_OBJECT_NULL;
    list_add_tail(&gpu_infos[apple_gpu_count].base.list, devices);
    ++apple_gpu_count;
  }

  *count = apple_gpu_count;

  [mtl_devices release];
  return true;
}

static void gpuinfo_apple_populate_static_info(struct gpu_info *_gpu_info) {
  struct gpu_info_apple *gpu_info = container_of(_gpu_info, struct gpu_info_apple, base);
  struct gpuinfo_static_info *static_info = &gpu_info->base.static_info;
  RESET_ALL(static_info->valid);

  if (gpu_info->device == nil)
    return;

  const char *name = [[gpu_info->device name] UTF8String];
  if (name == NULL)
    return;
  strncpy(static_info->device_name, name, sizeof(static_info->device_name));
  // A name as long as the field leaves the copy unterminated.
  static_info->device_name[sizeof(static_info->device_name) - 1] = '\0';
  SET_VALID(gpuinfo_device_name_valid, static_info->valid);

  // What makes a processor integrated is that it works on the memory of the
  // machine; the location of a card in a slot still says the same thing, and the
  // property is deprecated where there is no slot to be in.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
  static_info->integrated_graphics = [gpu_info->device hasUnifiedMemory] ||
                                     [gpu_info->device location] == MTLDeviceLocationBuiltIn;
#pragma clang diagnostic pop
  static_info->encode_decode_shared = true;
}

// A dictionary a driver publishes is read for the values it really answers with:
// a key that is missing, or is not a number the way the key names it, leaves the
// metric unavailable rather than 0 or 100.
static bool gpuinfo_apple_number(NSDictionary *dictionary, NSString *key, uint64_t *value) {
  if (dictionary == nil)
    return false;
  id info = [dictionary objectForKey:key];
  if (![info isKindOfClass:[NSNumber class]])
    return false;
  const double numeric = [info doubleValue];
  if (!isfinite(numeric) || numeric < 0. || numeric >= 0x1p64 || floor(numeric) != numeric)
    return false;
  *value = (uint64_t)numeric;
  return true;
}

// The used share of a memory, without the multiplication of a byte count that
// would wrap around on a machine big enough for it to matter.
static unsigned gpuinfo_apple_memory_percent(uint64_t used, uint64_t total) {
  if (total == 0u || used > total)
    return 0u;
  const double percent = (double)used * 100. / (double)total;
  return (percent > 100. || !isfinite(percent)) ? 100u : (unsigned)percent;
}

static void gpuinfo_apple_refresh_dynamic_info(struct gpu_info *_gpu_info) {
  struct gpu_info_apple *gpu_info = container_of(_gpu_info, struct gpu_info_apple, base);
  struct gpuinfo_dynamic_info *dynamic_info = &gpu_info->base.dynamic_info;
  RESET_ALL(dynamic_info->valid);

  // A device the registry never answered for has no properties to ask for: what
  // the device answers itself is taken below and the rest stays unavailable.
  const bool has_service = MACH_PORT_VALID(gpu_info->gpu_service);

  NSDictionary *performance_statistics = nil;
  CFMutableDictionaryRef cf_props = NULL;
  if (has_service &&
      IORegistryEntryCreateCFProperties(gpu_info->gpu_service, &cf_props, kCFAllocatorDefault, kNilOptions) == kIOReturnSuccess) {
    NSDictionary *props = (__bridge NSDictionary*) cf_props;
    id statistics = [props objectForKey:@"PerformanceStatistics"];
    if ([statistics isKindOfClass:[NSDictionary class]])
      performance_statistics = statistics;
  }

  const bool unified_memory = [gpu_info->device hasUnifiedMemory];
  uint64_t gpu_utilization = 0;
  const bool has_utilization = gpuinfo_apple_number(performance_statistics, @"Device Utilization %", &gpu_utilization);

  // [gpu_info->device currentAllocatedSize] is the memory of this process, not the
  // memory the GPU allocated on the machine, which is what the statistics answer.
  uint64_t used_memory = 0;
  const bool has_used_memory =
      unified_memory && gpuinfo_apple_number(performance_statistics, @"Alloc system memory", &used_memory);

  // The copy the registry gave this function, given back whether or not a
  // statistics block came out of it: the refresh runs once per redraw.
  if (cf_props)
    CFRelease(cf_props);

  // A utilization outside the percent it claims to be is not a machine running
  // flat out, it is an answer that means something else, and it is refused.
  if (has_utilization && gpu_utilization <= 100u)
    SET_GPUINFO_DYNAMIC(dynamic_info, gpu_util_rate, (unsigned)gpu_utilization);

  if (unified_memory) {
    if (has_used_memory)
      SET_GPUINFO_DYNAMIC(dynamic_info, used_memory, used_memory);

    // Memory is unified, so query the amount of system memory instead.
    const mach_port_t host = mach_host_self();
    if (MACH_PORT_VALID(host)) {
      mach_msg_type_number_t host_size = HOST_BASIC_INFO_COUNT;
      host_basic_info_data_t info;
      if (host_info(host, HOST_BASIC_INFO, (host_info_t) &info, &host_size) == KERN_SUCCESS)
        SET_GPUINFO_DYNAMIC(dynamic_info, total_memory, info.max_mem);
      // mach_host_self() hands out a reference of this process to the host port.
      // Never given back, it is one leaked right per device and per refresh.
      mach_port_deallocate(mach_task_self(), host);
    }
  } else {
    // TODO: Figure out how to get used memory for this case.

    // It does not really seem to be possible to get the amount of memory of a particular GPU.
    // In this case, just get the recommended working set size. This is what MoltenVK also does.
    const uint64_t mem_total = [gpu_info->device recommendedMaxWorkingSetSize];
    if (mem_total > 0)
      SET_GPUINFO_DYNAMIC(dynamic_info, total_memory, mem_total);
  }

  if (GPUINFO_DYNAMIC_FIELD_VALID(dynamic_info, used_memory) && GPUINFO_DYNAMIC_FIELD_VALID(dynamic_info, total_memory)) {
    // The allocated memory and the memory of the machine answer two questions and
    // nothing holds the first inside the second: an impossible pair is dropped
    // rather than wrapped around into a free memory of sixteen exabytes and a
    // percentage computed out of it.
    if (dynamic_info->total_memory > 0 && dynamic_info->used_memory <= dynamic_info->total_memory) {
      SET_GPUINFO_DYNAMIC(dynamic_info, free_memory, dynamic_info->total_memory - dynamic_info->used_memory);
      SET_GPUINFO_DYNAMIC(dynamic_info, mem_util_rate,
                          gpuinfo_apple_memory_percent(dynamic_info->used_memory, dynamic_info->total_memory));
    } else {
      RESET_GPUINFO_DYNAMIC(dynamic_info, used_memory);
      RESET_GPUINFO_DYNAMIC(dynamic_info, total_memory);
    }
  }
}

static bool gpuinfo_apple_get_process_info(struct gpu_process* process, io_object_t user_client) {
  RESET_ALL(process->valid);
  process->type = gpu_process_graphical_compute;

  CFMutableDictionaryRef cf_props;
  if (IORegistryEntryCreateCFProperties(user_client, &cf_props, kCFAllocatorDefault, kNilOptions) != kIOReturnSuccess) {
    return false;
  }
  NSDictionary* user_client_info = (__bridge NSDictionary*) cf_props;

  // Every way out of this copy gives it back: the iterator over the user clients
  // of a busy GPU runs it once per client and per refresh.
  id client_creator_info = [user_client_info objectForKey:@"IOUserClientCreator"];
  if (![client_creator_info isKindOfClass:[NSString class]]) {
    CFRelease(cf_props);
    return false;
  }

  const char* client_creator = [client_creator_info UTF8String];
  // Client creator is in form: pid <pid>, <name>
  if (client_creator == NULL || sscanf(client_creator, "pid %u,", &process->pid) < 1) {
    CFRelease(cf_props);
    return false;
  }

  CFRelease(cf_props);

  return true;
}

static void gpuinfo_apple_get_running_processes(struct gpu_info *_gpu_info) {
  struct gpu_info_apple *gpu_info = container_of(_gpu_info, struct gpu_info_apple, base);
  _gpu_info->processes_count = 0;

  // We can find out which processes are running on a particular GPU using the IO Registry. The
  // IOService associated to the MTLDevice has "AGXDeviceUserClient" child nodes, which hold some
  // basic information about processes that are running on the GPU.

  if (!MACH_PORT_VALID(gpu_info->gpu_service))
    return;

  io_iterator_t iterator;
  if (IORegistryEntryGetChildIterator(gpu_info->gpu_service, kIOServicePlane, &iterator) != kIOReturnSuccess) {
    return;
  }

  unsigned int count = 0;
  for (io_object_t child = IOIteratorNext(iterator); child; child = IOIteratorNext(iterator)) {
    io_name_t class_name;
    // The child is this loop's reference whatever it turns out to be: the two ways
    // out of an uninteresting one used to walk past the release, one object left
    // behind per child and per refresh.
    const bool is_user_client = IOObjectGetClass(child, class_name) == kIOReturnSuccess &&
                                strncmp(class_name, "AGXDeviceUserClient", sizeof(class_name)) == 0;
    if (is_user_client) {
      if (_gpu_info->processes_array_size < count + 1) {
        _gpu_info->processes_array_size += COMMON_PROCESS_LINEAR_REALLOC_INC;
        _gpu_info->processes =
            reallocarray(_gpu_info->processes, _gpu_info->processes_array_size, sizeof(*_gpu_info->processes));
        if (!_gpu_info->processes) {
          perror("Could not allocate memory: ");
          exit(EXIT_FAILURE);
        }
      }

      if (gpuinfo_apple_get_process_info(&_gpu_info->processes[count], child)) {
        ++count;
      }
    }

    IOObjectRelease(child);
  }

  // The iterator is a reference of the same kind, on a loop per redraw.
  IOObjectRelease(iterator);

  _gpu_info->processes_count = count;
}
