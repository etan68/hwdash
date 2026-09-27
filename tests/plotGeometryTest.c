/* Standalone checks for chart scale, left-edge behavior and readouts. */

#include "nvtop/common.h"
#include "nvtop/plot_geometry.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

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

// The oldest edge of a chart holds no mark at all: the oldest sample and the
// connector that used to follow it disappear together, and the line begins one
// column in. The reversed axis keeps its newest edge.
static void test_left_edge_keeps_no_mark(void) {
  CHECK(nvtop_plot_first_drawn_column(false) == 1u);
  CHECK(nvtop_plot_first_drawn_column(true) == 0u);

  CHECK(nvtop_plot_sample_action_at(false, 0u) == nvtop_plot_sample_drop);
  CHECK(nvtop_plot_sample_action_at(false, 1u) == nvtop_plot_sample_point);
  CHECK(nvtop_plot_sample_action_at(false, 2u) == nvtop_plot_sample_connect);
  CHECK(nvtop_plot_sample_action_at(false, 100u) == nvtop_plot_sample_connect);

  CHECK(nvtop_plot_sample_action_at(true, 0u) == nvtop_plot_sample_point);
  CHECK(nvtop_plot_sample_action_at(true, 1u) == nvtop_plot_sample_connect);
  CHECK(nvtop_plot_sample_action_at(true, 2u) == nvtop_plot_sample_connect);
  CHECK(nvtop_plot_sample_action_at(true, 100u) == nvtop_plot_sample_connect);

  // Nothing is ever dropped on a reversed axis, and only the leftmost column
  // of an ordinary one.
  for (unsigned column = 0u; column < 40u; ++column) {
    CHECK((nvtop_plot_sample_action_at(false, column) == nvtop_plot_sample_drop) == (column == 0u));
    CHECK(nvtop_plot_sample_action_at(true, column) != nvtop_plot_sample_drop);
    CHECK((nvtop_plot_sample_action_at(true, column) == nvtop_plot_sample_point) == (column == 0u));
  }
}

// The readout of a line is the value the visible end of that line lies on.
static void test_newest_drawn_value_follows_the_time_direction(void) {
  // Three columns, two lines, interleaved by column then line.
  const double data[] = {10., 90., 30., 70., 50., NAN};
  double value = -1.;

  // Ordinary axis: the newest sample is on the right of the chart.
  CHECK(nvtop_plot_newest_drawn_value(3u, false, data, 2u, 0u, &value) && value == 50.);
  CHECK(nvtop_plot_newest_drawn_value(3u, false, data, 2u, 1u, &value) && value == 70.);

  CHECK(nvtop_plot_newest_drawn_value(3u, true, data, 2u, 0u, &value) && value == 10.);
  CHECK(nvtop_plot_newest_drawn_value(3u, true, data, 2u, 1u, &value) && value == 90.);

  // The newest sample of a reversed chart is its leftmost drawn column.
  const double single[] = {42., 43.};
  CHECK(nvtop_plot_newest_drawn_value(1u, true, single, 2u, 0u, &value) && value == 42.);
  // An ordinary chart never draws its leftmost column: it has nothing to show.
  CHECK(!nvtop_plot_newest_drawn_value(1u, false, single, 2u, 0u, &value));
  CHECK(!nvtop_plot_newest_drawn_value(0u, false, single, 2u, 0u, &value));
  CHECK(!nvtop_plot_newest_drawn_value(1u, false, NULL, 2u, 0u, &value));
  CHECK(!nvtop_plot_newest_drawn_value(1u, false, single, 0u, 0u, &value));
  CHECK(!nvtop_plot_newest_drawn_value(1u, false, single, 2u, 2u, &value));

  // A hole at the newest edge: the readout comes from the next drawn sample,
  // the one the line really ends on, and never from a hole.
  const double holes[] = {11., NAN, NAN, 22., 33., NAN, NAN, NAN, NAN, NAN};
  CHECK(nvtop_plot_newest_drawn_value(5u, false, holes, 2u, 0u, &value));
  CHECK(value == 33.); // column 2, the newest drawn sample of line 0
  CHECK(nvtop_plot_newest_drawn_value(5u, false, holes, 2u, 1u, &value));
  CHECK(value == 22.); // column 1, the oldest column an ordinary chart draws
  CHECK(nvtop_plot_newest_drawn_value(5u, true, holes, 2u, 0u, &value) && value == 11.);
  CHECK(nvtop_plot_newest_drawn_value(5u, true, holes, 2u, 1u, &value) && value == 22.);
  CHECK(!nvtop_plot_newest_drawn_value(5u, false, holes, 2u, 3u, &value));
  // A line that is only ever missing, or that only has a value in the column
  // the chart does not draw, has no current value.
  const double only_oldest[] = {NAN, 7., NAN, NAN};
  CHECK(!nvtop_plot_newest_drawn_value(2u, false, only_oldest, 2u, 1u, &value));
  CHECK(!nvtop_plot_newest_drawn_value(2u, false, only_oldest, 2u, 0u, &value));
  CHECK(nvtop_plot_newest_drawn_value(2u, true, only_oldest, 2u, 1u, &value) && value == 7.);
}

static void test_readout_formatting(void) {
  char text[PLOT_READOUT_VALUE_SIZE];

  CHECK(nvtop_format_plot_readout(text, sizeof(text), 0.4) && strcmp(text, "  0.4%") == 0);
  CHECK(nvtop_format_plot_readout(text, sizeof(text), 92.6) && strcmp(text, " 92.6%") == 0);
  CHECK(nvtop_format_plot_readout(text, sizeof(text), 0.) && strcmp(text, "  0.0%") == 0);
  CHECK(nvtop_format_plot_readout(text, sizeof(text), -0.) && strcmp(text, "  0.0%") == 0);
  CHECK(nvtop_format_plot_readout(text, sizeof(text), 100.) && strcmp(text, "100.0%") == 0);

  // The clamped ends of the scale never escape the fixed width either.
  CHECK(nvtop_format_plot_readout(text, sizeof(text), -12.5) && strcmp(text, "  0.0%") == 0);
  CHECK(nvtop_format_plot_readout(text, sizeof(text), 300.) && strcmp(text, "100.0%") == 0);

  CHECK(strlen(text) == PLOT_READOUT_VALUE_WIDTH);

  // A line without a value has no readout, and never a fabricated zero.
  CHECK(!nvtop_format_plot_readout(text, sizeof(text), NAN));
  CHECK(!nvtop_format_plot_readout(text, sizeof(text), INFINITY));
  CHECK(!nvtop_format_plot_readout(NULL, sizeof(text), 50.));
  CHECK(!nvtop_format_plot_readout(text, 0u, 50.));
  char too_small[6];
  CHECK(!nvtop_format_plot_readout(too_small, sizeof(too_small), 50.));
}

// Every placed readout is inside the data region, on a row of its own, and the
// rows go down the screen in the order of the values.
static void check_placed(const double *values, unsigned num_values, const struct plot_readout *readouts,
                         unsigned placed, unsigned window_rows) {
  CHECK(placed <= num_values);
  CHECK(placed <= MAX_LINES_PER_PLOT);
  for (unsigned i = 0; i < placed; ++i) {
    CHECK(readouts[i].row >= (int)PLOT_DATA_TOP_ROW);
    CHECK(readouts[i].row <= (int)window_rows - 1);
    CHECK(readouts[i].line < num_values);
    CHECK(isfinite(readouts[i].value));
    if (i > 0u) {
      CHECK(readouts[i].row > readouts[i - 1u].row);
      CHECK(values[readouts[i - 1u].line] >= values[readouts[i].line]);
      if (values[readouts[i - 1u].line] == values[readouts[i].line])
        CHECK(readouts[i - 1u].line < readouts[i].line);
    }
  }
}

static void test_readout_placement(void) {
  struct plot_readout readouts[MAX_LINES_PER_PLOT];
  const unsigned rows = 21u; // The data region holds rows 2 to 20.

  // A value sits on the row its own line ends on when nothing collides.
  const double apart[] = {0., 100.};
  unsigned placed = nvtop_place_plot_readouts(rows, apart, 2u, readouts, MAX_LINES_PER_PLOT);
  CHECK(placed == 2u);
  check_placed(apart, 2u, readouts, placed, rows);
  CHECK(readouts[0].line == 1u && readouts[0].row == nvtop_plot_data_level(rows, 100.));
  CHECK(readouts[1].line == 0u && readouts[1].row == nvtop_plot_data_level(rows, 0.));

  // Two lines that are fully overlapping: the higher value goes above, and the
  // two readouts land on adjacent rows.
  const double tied[] = {100., 100.};
  placed = nvtop_place_plot_readouts(rows, tied, 2u, readouts, MAX_LINES_PER_PLOT);
  CHECK(placed == 2u);
  check_placed(tied, 2u, readouts, placed, rows);
  CHECK(readouts[0].line == 0u && readouts[1].line == 1u);
  CHECK(readouts[0].row == (int)PLOT_DATA_TOP_ROW);
  CHECK(readouts[1].row == (int)PLOT_DATA_TOP_ROW + 1);

  // 100% and 0% stay on the two edges of the data region.
  const double ends[] = {100., 0.};
  placed = nvtop_place_plot_readouts(rows, ends, 2u, readouts, MAX_LINES_PER_PLOT);
  CHECK(placed == 2u);
  check_placed(ends, 2u, readouts, placed, rows);
  CHECK(readouts[0].row == (int)PLOT_DATA_TOP_ROW);
  CHECK(readouts[1].row == (int)rows - 1);

  // 0% for both lines: neither leaves the chart, and the higher line index
  // wins the row below.
  const double both_zero[] = {0., 0.};
  placed = nvtop_place_plot_readouts(rows, both_zero, 2u, readouts, MAX_LINES_PER_PLOT);
  CHECK(placed == 2u);
  check_placed(both_zero, 2u, readouts, placed, rows);
  CHECK(readouts[0].line == 0u && readouts[1].line == 1u);
  CHECK(readouts[1].row == (int)rows - 1);
  CHECK(readouts[0].row == (int)rows - 2);

  // Values so close that their rows cannot coexist are moved apart.
  const double close[] = {50.2, 49.9};
  const int y_close_0 = nvtop_plot_data_level(rows, close[0]);
  const int y_close_1 = nvtop_plot_data_level(rows, close[1]);
  placed = nvtop_place_plot_readouts(rows, close, 2u, readouts, MAX_LINES_PER_PLOT);
  CHECK(placed == 2u);
  check_placed(close, 2u, readouts, placed, rows);
  if (y_close_0 == y_close_1)
    CHECK(readouts[0].row == y_close_0 && readouts[1].row == y_close_1 + 1);

  // Missing values are read out by nobody: neither a zero nor a row.
  const double missing[] = {NAN, 25.};
  placed = nvtop_place_plot_readouts(rows, missing, 2u, readouts, MAX_LINES_PER_PLOT);
  CHECK(placed == 1u);
  check_placed(missing, 2u, readouts, placed, rows);
  CHECK(readouts[0].line == 1u);
  CHECK(nvtop_place_plot_readouts(rows, missing, 1u, readouts, MAX_LINES_PER_PLOT) == 0u);

  // The line index breaks the ties of any number of lines, in order.
  const double all_tied[] = {66., 66., 66., 66.};
  placed = nvtop_place_plot_readouts(rows, all_tied, MAX_LINES_PER_PLOT, readouts, MAX_LINES_PER_PLOT);
  CHECK(placed == MAX_LINES_PER_PLOT);
  check_placed(all_tied, MAX_LINES_PER_PLOT, readouts, placed, rows);
  for (unsigned i = 0; i < placed; ++i)
    CHECK(readouts[i].line == i);

  // A data region that cannot hold every line gives its rows to the highest
  // values, and nothing ever leaves the region.
  const double crowded[] = {10., 90., 50., 30.};
  const unsigned short_rows = PLOT_DATA_TOP_ROW + 2u; // Two rows only
  placed = nvtop_place_plot_readouts(short_rows, crowded, MAX_LINES_PER_PLOT, readouts, MAX_LINES_PER_PLOT);
  CHECK(placed == 2u);
  check_placed(crowded, MAX_LINES_PER_PLOT, readouts, placed, short_rows);
  CHECK(readouts[0].line == 1u); // 90%, the highest value
  CHECK(readouts[1].line == 2u); // 50%, the next one

  // A window that cannot hold the legend and one data row holds no readout.
  CHECK(nvtop_place_plot_readouts(PLOT_DATA_TOP_ROW, apart, 2u, readouts, MAX_LINES_PER_PLOT) == 0u);
  CHECK(nvtop_place_plot_readouts(0u, apart, 2u, readouts, MAX_LINES_PER_PLOT) == 0u);
  CHECK(nvtop_place_plot_readouts(rows, NULL, 2u, readouts, MAX_LINES_PER_PLOT) == 0u);
  CHECK(nvtop_place_plot_readouts(rows, apart, 0u, readouts, MAX_LINES_PER_PLOT) == 0u);
  CHECK(nvtop_place_plot_readouts(rows, apart, 2u, readouts, 0u) == 0u);

  // Whatever the values and whatever the height, the rows stay apart and
  // inside the data region, in the order of decreasing values.
  for (unsigned height = PLOT_DATA_TOP_ROW + 1u; height <= 12u; ++height) {
    for (unsigned first = 0; first <= 100u; first += 25u) {
      for (unsigned second = 0; second <= 100u; second += 25u) {
        const double sweep[] = {100., (double)first, (double)second, 100. - (double)first};
        placed = nvtop_place_plot_readouts(height, sweep, MAX_LINES_PER_PLOT, readouts, MAX_LINES_PER_PLOT);
        CHECK(placed <= height - PLOT_DATA_TOP_ROW);
        check_placed(sweep, MAX_LINES_PER_PLOT, readouts, placed, height);
      }
    }
  }
}

int main(void) {
  test_legend_clearance_and_scale();
  test_left_edge_keeps_no_mark();
  test_newest_drawn_value_follows_the_time_direction();
  test_readout_formatting();
  test_readout_placement();
  if (failures) {
    printf("%u/%u plot geometry checks failed\n", failures, checks);
    return 1;
  }
  printf("%u plot geometry checks passed\n", checks);
  return 0;
}
