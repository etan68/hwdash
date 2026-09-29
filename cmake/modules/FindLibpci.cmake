#.rst:
# FindLibpci
# -------
#
# Try to find the libpci library of pciutils, the library and name database
# lspci uses, on a Unix system. Looking up a name needs the development
# headers and libpci only; no PCI device is opened by the lookup itself.
#
# This will define the following variables:
#
# ``Libpci_FOUND``
#     True if libpci is available
# ``Libpci_VERSION``
#     The version of libpci
# ``Libpci_LIBRARIES``
#     This can be passed to target_link_libraries() instead of the ``Libpci::Libpci``
#     target
# ``Libpci_INCLUDE_DIRS``
#     This should be passed to target_include_directories() if the target is not
#     used for linking
#
# If ``Libpci_FOUND`` is TRUE, it will also define the following imported target:
#
# ``Libpci::Libpci``
#     The libpci library
#
# Point CMAKE_PREFIX_PATH at an extracted package tree that is not installed
# system-wide, for example ``-DCMAKE_PREFIX_PATH=/path/to/deps/usr``.

#=============================================================================
# SPDX-License-Identifier: GPL-3.0-or-later
#=============================================================================

if(NOT WIN32)
    # Use pkg-config to get the directories and then use these values
    # in the FIND_PATH() and FIND_LIBRARY() calls
    find_package(PkgConfig)
    if(PkgConfig_FOUND)
        pkg_check_modules(PKG_Libpci QUIET libpci)
    endif()

    set(Libpci_VERSION ${PKG_Libpci_VERSION})

    find_path(Libpci_INCLUDE_DIR
        NAMES
            pci/pci.h
        HINTS
            ${PKG_Libpci_INCLUDE_DIRS}
    )

    find_library(Libpci_LIBRARY
        NAMES
            pci
        HINTS
            ${PKG_Libpci_LIBRARY_DIRS}
    )

    include(FindPackageHandleStandardArgs)
    find_package_handle_standard_args(Libpci
        FOUND_VAR
            Libpci_FOUND
        REQUIRED_VARS
            Libpci_LIBRARY
            Libpci_INCLUDE_DIR
        VERSION_VAR
            Libpci_VERSION
    )

    if(Libpci_FOUND AND NOT TARGET Libpci::Libpci)
        # Global so that the test directory can use the target as well.
        add_library(Libpci::Libpci UNKNOWN IMPORTED GLOBAL)
        set_target_properties(Libpci::Libpci PROPERTIES
            IMPORTED_LOCATION "${Libpci_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${Libpci_INCLUDE_DIR}"
        )
    endif()

    mark_as_advanced(Libpci_LIBRARY Libpci_INCLUDE_DIR)

    # compatibility variables
    set(Libpci_LIBRARIES ${Libpci_LIBRARY})
    set(Libpci_INCLUDE_DIRS ${Libpci_INCLUDE_DIR})
    set(Libpci_VERSION_STRING ${Libpci_VERSION})

else()
    message(STATUS "FindLibpci.cmake cannot find libpci on Windows systems.")
    set(Libpci_FOUND FALSE)
endif()

include(FeatureSummary)
set_package_properties(Libpci PROPERTIES
    URL "https://github.com/pciutils/pciutils"
    DESCRIPTION "PCI access library and name database used by lspci"
    PURPOSE "Optional dependency that resolves the Intel GPU names udev does not know"
)
