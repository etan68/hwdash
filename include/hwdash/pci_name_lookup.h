/* Copyright (C) 2026 etan68
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef HWDASH_PCI_NAME_LOOKUP_H__
#define HWDASH_PCI_NAME_LOOKUP_H__

#include <stdbool.h>
#include <stddef.h>

// Resolve the display name of a PCI device from values a caller already read
// from sysfs or udev. No PCI device is opened, scanned or read here, so the
// lookup stays unprivileged and never touches the network.
//
// The name is chosen in this order:
//   1. model_from_database, the udev ID_MODEL_FROM_DATABASE property, when it
//      holds anything other than an empty string;
//   2. the device entry of the PCI name database, the same database lspci
//      reads through libpci, when the build found that optional library;
//   3. "<unknown_prefix> [vvvv:dddd]" built from the sysfs vendor and device
//      attributes, for example "Intel GPU [8086:7D67]".
//
// vendor_attribute and device_attribute are the raw sysfs texts, "0x8086" and
// "0x7D67"; anything but four hexadecimal digits is rejected. The name is
// written to name, which must hold at least 16 bytes. Level 3 may be
// shortened when the buffer is too small. The database entries keep the
// brackets of the PCI database, so callers keep normalizing them.
//
// Returns false when no level can produce a usable name, clearing a valid
// output buffer. NULL or undersized buffers are left untouched. An identifier
// is never guessed to make a name.
bool hwdash_pci_name_lookup(const char *model_from_database, const char *vendor_attribute,
                            const char *device_attribute, const char *unknown_prefix, char *name, size_t name_len);

// Release the PCI name database handle. Calling it without a previous lookup,
// or twice, does nothing harmful.
void hwdash_pci_name_lookup_shutdown(void);

#endif // HWDASH_PCI_NAME_LOOKUP_H__
