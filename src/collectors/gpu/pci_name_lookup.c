/* Copyright (C) 2026 etan68
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// Display name resolution for a PCI device whose identifiers are already
// known. The optional libpci level uses the same name database as lspci; see
// INTEL_PCI_NAME_LOOKUP in src/CMakeLists.txt.

#include "hwdash/pci_name_lookup.h"

#include <limits.h>
#include <setjmp.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HAS_LIBPCI
#include <pci/pci.h>
#endif

#define PCI_ID_DIGITS 4

// sysfs writes an identifier as "0x8086". Accept an optional 0x prefix and
// exactly four hexadecimal digits, and never repair anything else, so a
// truncated or malformed attribute cannot smuggle a guessed identifier into
// a name.
static bool parse_pci_id(const char *value, unsigned *id) {
  if (!value)
    return false;
  if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
    value += 2;
  unsigned parsed = 0;
  unsigned digits = 0;
  for (; *value != '\0'; ++value) {
    const char character = *value;
    char digit;
    if (character >= '0' && character <= '9')
      digit = (char)(character - '0');
    else if (character >= 'a' && character <= 'f')
      digit = (char)(character - 'a' + 10);
    else if (character >= 'A' && character <= 'F')
      digit = (char)(character - 'A' + 10);
    else
      return false;
    if (digits == PCI_ID_DIGITS)
      return false;
    parsed = (parsed << 4) | (unsigned)digit;
    ++digits;
  }
  if (digits != PCI_ID_DIGITS)
    return false;
  *id = parsed;
  return true;
}

#ifdef HAS_LIBPCI

// libpci reports an unusable environment through its error callback and does
// not expect that callback to return, while pci_alloc() leaves the callbacks
// empty. Every call below is therefore armed with a jump target the error
// callback unwinds to, and the warning and debug callbacks only drop text.
static struct pci_access *pci_access;
static bool pci_access_unusable;
static jmp_buf pci_call_guard;
static volatile bool pci_call_armed;

__attribute__((noreturn)) static void pci_fail_call(char *message, ...) {
  (void)message;
  if (pci_call_armed)
    longjmp(pci_call_guard, 1);
  // Unreachable: libpci is only called while armed. Returning would let it
  // carry on with the state it just called broken.
  abort();
}

static void pci_drop_message(char *message, ...) {
  (void)message;
}

// Name lookup only needs pci_alloc's database path and the callbacks below.
// Do not call pci_init: that selects a hardware access method we do not need.
// Used only by the single-threaded static-info collector.
static struct pci_access *get_pci_access(void) {
  if (pci_access_unusable)
    return NULL;
  if (pci_access)
    return pci_access;

  struct pci_access *access = pci_alloc();
  if (!access) {
    pci_access_unusable = true;
    return NULL;
  }
  access->error = pci_fail_call;
  access->warning = pci_drop_message;
  access->debug = pci_drop_message;
  access->id_lookup_mode = 0; // Neither DNS lookup nor the per-user DNS cache.
  pci_access = access;
  return pci_access;
}

static bool pci_database_device_name(unsigned vendor, unsigned device, char *name, size_t name_len) {
  struct pci_access *access = get_pci_access();
  if (!access)
    return false;

  const int size = name_len > (size_t)INT_MAX ? INT_MAX : (int)name_len;
  pci_call_armed = true;
  if (setjmp(pci_call_guard) != 0) {
    pci_call_armed = false;
    pci_access_unusable = true;
    name[0] = '\0';
    return false;
  }
  // Return NULL for an unknown ID; never ask hwdb, DNS or a hardware backend.
  const char *resolved = pci_lookup_name(access, name, size,
                                        PCI_LOOKUP_DEVICE | PCI_LOOKUP_NO_NUMBERS | PCI_LOOKUP_NO_HWDB,
                                        (int)vendor, (int)device);
  pci_call_armed = false;

  // An unknown entry, a failed query, or a database entry without text is no
  // name at all.
  if (!resolved || resolved[0] == '\0') {
    name[0] = '\0';
    return false;
  }
  if (resolved != name)
    snprintf(name, name_len, "%s", resolved);
  return true;
}

#else // HAS_LIBPCI

static bool pci_database_device_name(unsigned vendor, unsigned device, char *name, size_t name_len) {
  (void)vendor;
  (void)device;
  (void)name;
  (void)name_len;
  return false;
}

#endif // HAS_LIBPCI

bool hwdash_pci_name_lookup(const char *model_from_database, const char *vendor_attribute,
                            const char *device_attribute, const char *unknown_prefix, char *name, size_t name_len) {
  if (!name || name_len < 16)
    return false;
  name[0] = '\0';

  if (model_from_database && model_from_database[0] != '\0') {
    snprintf(name, name_len, "%s", model_from_database);
    return name[0] != '\0';
  }

  unsigned vendor, device;
  if (!parse_pci_id(vendor_attribute, &vendor) || !parse_pci_id(device_attribute, &device))
    return false;

  if (pci_database_device_name(vendor, device, name, name_len))
    return true;

  if (!unknown_prefix || unknown_prefix[0] == '\0')
    return false;

  // Brackets, like the database entries, so that the caller normalizes this
  // name the same way. Use upper case consistently for hexadecimal digits.
  return snprintf(name, name_len, "%s [%04X:%04X]", unknown_prefix, vendor, device) > 0;
}

#ifdef HAS_LIBPCI

void hwdash_pci_name_lookup_shutdown(void) {
  pci_access_unusable = false;
  if (!pci_access)
    return;
  struct pci_access *access = pci_access;
  pci_access = NULL;

  pci_call_armed = true;
  if (setjmp(pci_call_guard) == 0)
    pci_cleanup(access);
  pci_call_armed = false;
}

#else // HAS_LIBPCI

void hwdash_pci_name_lookup_shutdown(void) {}

#endif // HAS_LIBPCI
