# The host metric checks only need the parsers: no GPU, no download, no gtest,
# and no ncurses: src/plot_legend.c lays the legend row out as plain text.
if (BUILD_TESTING)
  add_executable(
    hostMetricsTest
    hostMetricsTest.c
    ${PROJECT_SOURCE_DIR}/src/time.c
    ${PROJECT_SOURCE_DIR}/src/collectors/host/host_metrics.c
    ${PROJECT_SOURCE_DIR}/src/collectors/vendor/lenovo_cpu_fan.c
  )
  target_include_directories(hostMetricsTest PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_link_libraries(hostMetricsTest PRIVATE m)
  add_test(NAME hostMetricsTest COMMAND hostMetricsTest)

  # The /proc code paths are the Linux half of the host collector. On a
  # platform that has no /proc filesystem they would never be compiled at all,
  # let alone run, so this target asks for them explicitly: the very same test
  # source and the very same expectations, with HOST_METRICS_FORCE_PROC on.
  # Running it on a platform whose /proc is not there keeps the parsers and the
  # sysfs scans compiled and their results checked, and the live sample simply
  # reports itself unavailable, which is what an unreadable /proc must do.
  if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(
      hostMetricsProcTest
      hostMetricsTest.c
      ${PROJECT_SOURCE_DIR}/src/time.c
      ${PROJECT_SOURCE_DIR}/src/collectors/host/host_metrics.c
      ${PROJECT_SOURCE_DIR}/src/collectors/vendor/lenovo_cpu_fan.c
    )
    target_compile_definitions(hostMetricsProcTest PRIVATE HOST_METRICS_FORCE_PROC)
    target_include_directories(hostMetricsProcTest PRIVATE
      ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(hostMetricsProcTest PRIVATE m)
    add_test(NAME hostMetricsProcTest COMMAND hostMetricsProcTest)
  endif()

  # The macOS host collector sampled against this machine: two samples of the
  # whole host CPU, the memory, the identity of the CPU and the load averages,
  # each checked against the same kernel questions asked directly. Small, live
  # and runnable on its own without a GPU, without ncurses and without gtest.
  if(APPLE)
    add_executable(
      hostMetricsMacTest
      hostMetricsMacTest.c
      ${PROJECT_SOURCE_DIR}/src/time.c
      ${PROJECT_SOURCE_DIR}/src/collectors/host/host_metrics.c
      ${PROJECT_SOURCE_DIR}/src/collectors/vendor/lenovo_cpu_fan.c
    )
    target_include_directories(hostMetricsMacTest PRIVATE
      ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(hostMetricsMacTest PRIVATE m)
    add_test(NAME hostMetricsMacTest COMMAND hostMetricsMacTest)
  endif()

  # The section layout checks the shape of the monitoring screen from the top
  # of the terminal to the bottom: no GPU, no ncurses and no gtest either.
  add_executable(
    sectionLayoutTest
    sectionLayoutTest.c
    stubProcessinfo.c
    ${PROJECT_SOURCE_DIR}/src/time.c
    ${PROJECT_SOURCE_DIR}/src/interface_layout_selection.c
    ${PROJECT_SOURCE_DIR}/src/interface_options.c
    ${PROJECT_SOURCE_DIR}/src/collectors/host/host_metrics.c
    ${PROJECT_SOURCE_DIR}/src/collectors/vendor/lenovo_cpu_fan.c
    ${PROJECT_SOURCE_DIR}/src/ini.c
  )
  target_include_directories(sectionLayoutTest PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_link_libraries(sectionLayoutTest PRIVATE m)
  add_test(NAME sectionLayoutTest COMMAND sectionLayoutTest)

  # The legend row checks the placement and the truncation of the chart keys:
  # still no GPU, no ncurses and no gtest.
  add_executable(
    plotLegendTest
    plotLegendTest.c
    ${PROJECT_SOURCE_DIR}/src/plot_legend.c
  )
  target_include_directories(plotLegendTest PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  add_test(NAME plotLegendTest COMMAND plotLegendTest)

  # The plot geometry keeps the 100% line clear of the legend, aligns every
  # Y-axis tick with the data scale, keeps the left edge of a chart free of any
  # leftover mark, and places the current value of every line of a chart in its
  # readout gutter. No ncurses either: the placement is plain arithmetic.
  add_executable(
    plotGeometryTest
    plotGeometryTest.c
    ${PROJECT_SOURCE_DIR}/src/plot_geometry.c
  )
  target_include_directories(plotGeometryTest PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_link_libraries(plotGeometryTest PRIVATE m)
  add_test(NAME plotGeometryTest COMMAND plotGeometryTest)

  # The PCI name check keeps the display-name priority of a card that udev
  # does not know: the PCI name database first, the sysfs identifiers after
  # it. No GPU, no udev, no gtest and no network either. The lookup is built
  # without libpci here so that the identifier name is always predictable.
  add_executable(
    pciNameTest
    pciNameTest.c
    ${PROJECT_SOURCE_DIR}/src/collectors/gpu/pci_name_lookup.c
  )
  target_include_directories(pciNameTest PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  add_test(NAME pciNameTest COMMAND pciNameTest)

  # The same checks with the optional libpci lookup enabled, so that opening
  # the name database and the fallback of an unknown card are exercised too.
  # libpci belongs to the Intel backend, so it is not even looked for here
  # without it; asking again just repeats the answer src/ already got.
  if(INTEL_SUPPORT AND INTEL_PCI_NAME_LOOKUP)
    find_package(Libpci QUIET)
  endif()
  if(Libpci_FOUND)
    add_executable(
      pciNameLibpciTest
      pciNameTest.c
      ${PROJECT_SOURCE_DIR}/src/collectors/gpu/pci_name_lookup.c
    )
    target_compile_definitions(pciNameLibpciTest PRIVATE
      HAS_LIBPCI
      HWDASH_PCI_TEST_WITH_LIBPCI)
    target_include_directories(pciNameLibpciTest PRIVATE
      ${PROJECT_SOURCE_DIR}/include)
    target_link_libraries(pciNameLibpciTest PRIVATE Libpci::Libpci)
    add_test(NAME pciNameLibpciTest COMMAND pciNameLibpciTest)
  endif()
endif()

find_package(GTest)
if (BUILD_TESTING AND GTest_FOUND)

  option(THOROUGH_TESTING "Enable extensive testing (takes hours, e.g., use once per release)" OFF)

  # Create a library for testing
  add_library(testLib
    ${PROJECT_SOURCE_DIR}/src/interface_layout_selection.c
    ${PROJECT_SOURCE_DIR}/src/collectors/gpu/extract_processinfo_fdinfo.c
    ${PROJECT_SOURCE_DIR}/src/interface_options.c
    ${PROJECT_SOURCE_DIR}/src/collectors/gpu/pcie_utilization.c
    ${PROJECT_SOURCE_DIR}/src/collectors/host/host_metrics.c
    ${PROJECT_SOURCE_DIR}/src/collectors/vendor/lenovo_cpu_fan.c
    ${PROJECT_SOURCE_DIR}/src/plot_legend.c
    ${PROJECT_SOURCE_DIR}/src/time.c
    ${PROJECT_SOURCE_DIR}/src/ini.c
  )
  target_include_directories(testLib PUBLIC
    ${PROJECT_SOURCE_DIR}/include
    ${PROJECT_BINARY_DIR}/include)

  # Tests
  add_executable(
    interfaceTests
    interfaceTests.cpp
  )
  target_link_libraries(interfaceTests PRIVATE testLib GTest::gtest_main)
  gtest_discover_tests(interfaceTests)

  if (THOROUGH_TESTING)
    target_compile_definitions(interfaceTests PRIVATE THOROUGH_TESTING)
  endif()

  add_executable(
    hostMetricsTests
    hostMetricsTests.cpp
  )
  target_link_libraries(hostMetricsTests PRIVATE testLib GTest::gtest_main)
  gtest_discover_tests(hostMetricsTests)


endif()

# The macOS collector is the platform half of the host collector: whatever checks
# host_metrics.c as the program does on that platform has to link it. The forced
# /proc target is deliberately not one of them: it asks for the Linux collector,
# which has nothing to do with the Mach one, and it keeps compiling it alone.
if (APPLE AND BUILD_TESTING)
  foreach(hwdash_host_target IN ITEMS
          hostMetricsTest hostMetricsMacTest sectionLayoutTest testLib)
    if(TARGET ${hwdash_host_target})
      target_sources(${hwdash_host_target} PRIVATE
        ${PROJECT_SOURCE_DIR}/src/collectors/host/host_metrics_mac.c)
    endif()
  endforeach()
endif()
