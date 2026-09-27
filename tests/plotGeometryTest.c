/* Standalone checks for chart scale and left-edge behavior. */

#include "nvtop/plot_geometry.h"

#include <math.h>
#include <stdio.h>

static unsigned checks = 0u, failures = 0u;

#define CHECK(condition)                                                                                               \
  do {                                                                                                                 \
    ++checks;                                                                                                          \
    if (!(condition)) {                                                                                                \
      ++failures;                                                                                                      \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition);                                                    \
    }                                                                                                                  \
  } while (0)

static void test_legend_clearance_and_scale(void) {
  const unsigned rows = 20u;
  CHECK(nvtop_plot_data_level(rows, 100.) == (int)PLOT_DATA_TOP_ROW);
  CHECK(nvtop_plot_data_level(rows, 0.) == (int)rows - 1);
  CHECK(nvtop_plot_data_level(rows, 100.) > (int)PLOT_LEGEND_ROW + 1);

  const int y100 = nvtop_plot_data_level(rows, 100.);
  const int y75 = nvtop_plot_data_level(rows, 75.);
  const int y50 = nvtop_plot_data_level(rows, 50.);
  const int y25 = nvtop_plot_data_level(rows, 25.);
  const int y0 = nvtop_plot_data_level(rows, 0.);
  CHECK(y100 < y75 && y75 < y50 && y50 < y25 && y25 < y0);

  // Values outside the chart percentage range stay on its two data edges.
  CHECK(nvtop_plot_data_level(rows, 150.) == y100);
  CHECK(nvtop_plot_data_level(rows, -10.) == y0);
  CHECK(nvtop_plot_data_level(rows, NAN) == -1);
  CHECK(nvtop_plot_data_level(PLOT_DATA_TOP_ROW, 50.) == -1);
}

static void test_left_edge_drops_the_connector(void) {
  CHECK(!nvtop_plot_connect_from_previous(0u));
  CHECK(!nvtop_plot_connect_from_previous(1u));
  CHECK(nvtop_plot_connect_from_previous(2u));
  CHECK(nvtop_plot_connect_from_previous(100u));
}

int main(void) {
  test_legend_clearance_and_scale();
  test_left_edge_drops_the_connector();
  if (failures) {
    printf("%u/%u plot geometry checks failed\n", failures, checks);
    return 1;
  }
  printf("%u plot geometry checks passed\n", checks);
  return 0;
}
