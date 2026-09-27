# The host metric checks only need the parsers: no GPU, no download, no gtest,
# and no ncurses: src/plot_legend.c lays the legend row out as plain text.
if (BUILD_TESTING)
  add_executable(
    hostMetricsTest
    hostMetricsTest.c
    ${PROJECT_SOURCE_DIR}/src/time.c
    ${PROJECT_SOURCE_DIR}/src/host_metrics.c
  )
  target_include_directories(hostMetricsTest PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_link_libraries(hostMetricsTest PRIVATE m)
  add_test(NAME hostMetricsTest COMMAND hostMetricsTest)

  # The section layout checks the shape of the monitoring screen from the top
  # of the terminal to the bottom: no GPU, no ncurses and no gtest either.
  add_executable(
    sectionLayoutTest
    sectionLayoutTest.c
    stubProcessinfo.c
    ${PROJECT_SOURCE_DIR}/src/time.c
    ${PROJECT_SOURCE_DIR}/src/interface_layout_selection.c
    ${PROJECT_SOURCE_DIR}/src/interface_options.c
    ${PROJECT_SOURCE_DIR}/src/host_metrics.c
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
  # Y-axis tick with the data scale, and drops the final left-edge connector.
  add_executable(
    plotGeometryTest
    plotGeometryTest.c
    ${PROJECT_SOURCE_DIR}/src/plot_geometry.c
  )
  target_include_directories(plotGeometryTest PRIVATE
    ${PROJECT_SOURCE_DIR}/include)
  target_link_libraries(plotGeometryTest PRIVATE m)
  add_test(NAME plotGeometryTest COMMAND plotGeometryTest)
endif()

find_package(GTest)
if (BUILD_TESTING AND GTest_FOUND)

  option(THOROUGH_TESTING "Enable extensive testing (takes hours, e.g., use once per release)" OFF)

  # Create a library for testing
  add_library(testLib
    ${PROJECT_SOURCE_DIR}/src/interface_layout_selection.c
    ${PROJECT_SOURCE_DIR}/src/extract_processinfo_fdinfo.c
    ${PROJECT_SOURCE_DIR}/src/interface_options.c
    ${PROJECT_SOURCE_DIR}/src/pcie_utilization.c
    ${PROJECT_SOURCE_DIR}/src/host_metrics.c
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
