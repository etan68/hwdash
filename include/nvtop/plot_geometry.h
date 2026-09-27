/* Pure geometry shared by the chart frame and line renderer. */

#ifndef PLOT_GEOMETRY_H__
#define PLOT_GEOMETRY_H__

#include <stdbool.h>

// The first row belongs to the legend. Keep one clear row below it before the
// 100% data line so a full-scale curve can never overwrite the legend.
#define PLOT_LEGEND_ROW 0u
#define PLOT_DATA_TOP_ROW 2u

// Left-side chart geometry: three label columns, one blank, one short tick,
// the vertical axis, then the data area.
#define PLOT_Y_TICK_COL 4u
#define PLOT_Y_AXIS_COL 5u
#define PLOT_DATA_X_OFFSET 6u
#define PLOT_HORIZONTAL_OVERHEAD 7u

// Map a percentage to a row of the inner plot window. Values are clamped to
// 0..100. Returns -1 when the window cannot hold the legend, gap and data.
int nvtop_plot_data_level(unsigned window_rows, double percent);

// The connector between the first two visible samples is deliberately omitted.
// When an old transition reaches the left edge it therefore disappears instead
// of surviving for one refresh as a vertical line.
bool nvtop_plot_connect_from_previous(unsigned sample_column);

#endif // PLOT_GEOMETRY_H__
