/* Standalone checks for the PCI device display-name fallback.
 * Copyright (C) 2026 etan68
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

// No GPU, no udev, no gtest and no network. The test target builds this file
// twice: once with the PCI name database disabled, where the expected name is
// always the identifier text, and once with libpci enabled, where a name may
// also come from the database of the machine. Run the one you built:
//   ./pciNameTest          or          ./pciNameLibpciTest

#include "hwdash/pci_name_lookup.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static unsigned checks = 0, failures = 0;

#define CHECK(cond)                                                                                                    \
  do {                                                                                                                 \
    ++checks;                                                                                                          \
    if (!(cond)) {                                                                                                     \
      ++failures;                                                                                                      \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                                          \
    }                                                                                                                  \
  } while (0)

#define NAME_LEN 128
#define GENERIC_PREFIX "Intel GPU ["

static bool name_is(const char *model, const char *vendor, const char *device, const char *expected) {
  char name[NAME_LEN] = "untouched";
  if (!hwdash_pci_name_lookup(model, vendor, device, "Intel GPU", name, sizeof(name)))
    return false;
  return strcmp(name, expected) == 0;
}

// The identifier name is the only name that starts with the generic prefix,
// and it then holds exactly the identifiers that were asked about.
#ifdef HWDASH_PCI_TEST_WITH_LIBPCI
static bool name_is_valid(const char *name, const char *identifiers) {
  if (name[0] == '\0')
    return false;
  const char *generic = strstr(name, GENERIC_PREFIX);
  if (!generic)
    return true;
  char expected[NAME_LEN];
  snprintf(expected, sizeof(expected), "%s%s]", GENERIC_PREFIX, identifiers);
  return strcmp(generic, expected) == 0;
}
#endif // HWDASH_PCI_TEST_WITH_LIBPCI

static void test_udev_name_wins(void) {
  // A nonempty udev name is used verbatim, brackets included, whatever the
  // identifiers say; the caller turns the brackets into parentheses.
  CHECK(name_is("Arrow Lake-U [Intel Graphics]", "0x8086", "0x7D67", "Arrow Lake-U [Intel Graphics]"));
  CHECK(name_is("UHD Graphics 620", NULL, NULL, "UHD Graphics 620"));

  // A too small buffer keeps the name inside its bounds.
  char name[16] = "untouched";
  CHECK(hwdash_pci_name_lookup("Intel Iris Xe Max Discrete Graphics", NULL, NULL, "Intel GPU", name, sizeof(name)));
  CHECK(strcmp(name, "Intel Iris Xe M") == 0);
}

static void test_malformed_ids_never_make_a_name(void) {
  static const char *const broken[] = {"",        "0x",      "0x808",   "0x80861", "80861",
                                       "0xZZZZ",  "0x8086 ", " 0x8086", "+0x8086", "0x-8086",
                                       "vendor",  "0x8086\n", "0x8086z", "0X808",  "0x80860"};
  char name[NAME_LEN];
  for (size_t i = 0; i < sizeof(broken) / sizeof(broken[0]); ++i) {
    name[0] = 'x';
    CHECK(!hwdash_pci_name_lookup(NULL, broken[i], "0x7D67", "Intel GPU", name, sizeof(name)));
    CHECK(name[0] == '\0');
    CHECK(!hwdash_pci_name_lookup(NULL, "0x8086", broken[i], "Intel GPU", name, sizeof(name)));
    CHECK(name[0] == '\0');
  }

  // No identifiers, or a buffer that cannot hold any name, are no name
  // either: the caller keeps its own placeholder.
  name[0] = 'x';
  CHECK(!hwdash_pci_name_lookup(NULL, NULL, NULL, "Intel GPU", name, sizeof(name)));
  CHECK(name[0] == '\0');
  name[0] = 'x';
  CHECK(!hwdash_pci_name_lookup(NULL, "0x8086", "0x7D67", "Intel GPU", name, 8));
  CHECK(name[0] == 'x');
}

#ifdef HWDASH_PCI_TEST_WITH_LIBPCI

static void test_database_name_or_identifier(void) {
  char name[NAME_LEN] = "untouched";

  // The known card may be resolved from the database, which does not
  // necessarily ship the entry, but the answer is never empty and never an
  // identifier that was not asked about.
  CHECK(hwdash_pci_name_lookup(NULL, "0x8086", "0x7D67", "Intel GPU", name, sizeof(name)));
  CHECK(name_is_valid(name, "8086:7D67"));
  CHECK(strstr(name, "0x") == NULL);

  // An unknown card still gets a name from its identifiers.
  memset(name, 0, sizeof(name));
  CHECK(hwdash_pci_name_lookup(NULL, "0xffff", "0xffff", "Intel GPU", name, sizeof(name)));
  CHECK(name_is_valid(name, "FFFF:FFFF"));

  // No udev name and no usable identifiers is no name at all, with the
  // database available as well.
  memset(name, 0, sizeof(name));
  CHECK(!hwdash_pci_name_lookup("", "0x808", NULL, "Intel GPU", name, sizeof(name)));
  CHECK(name[0] == '\0');
}

#else // HWDASH_PCI_TEST_WITH_LIBPCI

static void test_identifier_name_without_database(void) {
  // With no udev name and no database, the identifiers of the card are the
  // name: sysfs vendor 0x8086 and device 0x7D67. Upper case and no 0x prefix
  // are both accepted, and the brackets of the database entries are kept so
  // that the caller normalizes this name the same way.
  CHECK(name_is(NULL, "0x8086", "0x7D67", "Intel GPU [8086:7D67]"));
  CHECK(name_is("", "0x8086", "0x7d67", "Intel GPU [8086:7D67]"));
  CHECK(name_is(NULL, "8086", "7D67", "Intel GPU [8086:7D67]"));
  CHECK(name_is(NULL, "0X0001", "0X000A", "Intel GPU [0001:000A]"));

  // A prefix that is not a word is not a name either.
  char name[NAME_LEN] = "untouched";
  CHECK(!hwdash_pci_name_lookup(NULL, "0x8086", "0x7D67", "", name, sizeof(name)));
  CHECK(name[0] == '\0');
}

#endif // HWDASH_PCI_TEST_WITH_LIBPCI

int main(void) {
#ifdef HWDASH_PCI_TEST_WITH_LIBPCI
  printf("PCI name lookup: libpci available\n");
#else
  printf("PCI name lookup: name database unavailable\n");
#endif
  test_udev_name_wins();
  test_malformed_ids_never_make_a_name();
#ifdef HWDASH_PCI_TEST_WITH_LIBPCI
  test_database_name_or_identifier();
#else
  test_identifier_name_without_database();
#endif
  printf("%s: %u checks, %u failures\n", failures == 0 ? "PASS" : "FAIL", checks, failures);
  return failures == 0 ? 0 : 1;
}
