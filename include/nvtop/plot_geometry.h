/* Pure geometry shared by the chart frame and the line renderer. */

#ifndef PLOT_GEOMETRY_H__
#define PLOT_GEOMETRY_H__

#include <stdbool.h>
#include <stddef.h>

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

// Right-side chart geometry: the readout gutter. The frame of a chart stops
// short of the right edge of its section by the width of the gutter, and the
// gutter holds the current value of every line of the chart. The section keeps
// the width the layout gave it: the gutter is taken off the data area, never
// added to the section, and the values never sit on a curve or on the frame.
// The widest value the gutter holds is a clamped "100.0%".
#define PLOT_READOUT_VALUE_WIDTH 6u
#define PLOT_READOUT_VALUE_SIZE (PLOT_READOUT_VALUE_WIDTH + 1u)
// One blank column between the frame and the values, one at the edge.
#define PLOT_READOUT_LEFT_PAD 1u
#define PLOT_READOUT_RIGHT_PAD 1u
#define PLOT_READOUT_GUTTER_SIZE (PLOT_READOUT_LEFT_PAD + PLOT_READOUT_VALUE_WIDTH + PLOT_READOUT_RIGHT_PAD)

// Columns of a chart that can never hold a data column: the labels, the axis,
// the two borders and the readout gutter. What is left of the width the layout
// gave the chart is shared out between the lines it draws.
#define PLOT_COLUMNS_NOT_DATA (PLOT_HORIZONTAL_OVERHEAD + PLOT_READOUT_GUTTER_SIZE)

// What the renderer makes of one sample of a line, given the column it lies in
// and the direction the time flows in.
enum nvtop_plot_sample_action {
  nvtop_plot_sample_drop,   // No mark: the sample left the chart at its oldest edge
  nvtop_plot_sample_point,  // A mark with nothing on its left: the start of a visible segment
  nvtop_plot_sample_connect // A transition from the previous sample of the line
};

// The current value of one line of a chart, as the gutter displays it.
struct plot_readout {
  unsigned line; // Plot line the value belongs to
  int row;       // Row the value is written on, in the plot rows
  double value;  // The percentage the value shows
};

// Map a percentage to a row of the inner plot window. Values are clamped to
// 0..100. Returns -1 when the window cannot hold the legend, gap and data.
int nvtop_plot_data_level(unsigned window_rows, double percent);

// The column of the oldest sample of a line that still gets a mark. With the
// ordinary direction of time the left edge of a chart is its oldest edge and
// nothing is drawn there: the connector between the first two visible samples
// is deliberately omitted, so the oldest sample has no connector at all and it
// must leave the chart together with it, instead of surviving one refresh as an
// isolated mark. A line therefore begins one column in from the left edge, at
// the first sample that can be connected to. With the time axis reversed the
// left edge is the newest edge, which holds the head of every line, so nothing
// is ever left out there.
unsigned nvtop_plot_first_drawn_column(bool reversed_time_axis);

// What to draw for the sample of a line at a column. On an ordinary axis the
// disappearing sample at column zero is dropped together with its connector;
// on a reversed axis the left edge is the newest sample and starts a normally
// connected line.
enum nvtop_plot_sample_action nvtop_plot_sample_action_at(bool reversed_time_axis, unsigned sample_column);

// The value to read out for a line: the most recent sample of that line that is
// drawn on the chart, that is the sample the visible end of the line lies on.
// The samples are interleaved the way the renderer reads them,
// data[column * num_lines + line], and a sample that is not a number is a hole
// in the history. num_columns is the number of columns of the chart, the number
// of samples of a line. Returns false when the line has no drawn value at all,
// leaving *value untouched: nothing is drawn for such a line, and never a zero.
bool nvtop_plot_newest_drawn_value(size_t num_columns, bool reversed_time_axis, const double *data, unsigned num_lines,
                                   unsigned line, double *value);

// The current value of a line, as the gutter shows it: a percentage with one
// decimal, right aligned in PLOT_READOUT_VALUE_WIDTH columns, clamped to the
// 0.0% and 100.0% the chart can draw. Returns false for a value that is not a
// number: a line without a current value has no readout, not a zero.
bool nvtop_format_plot_readout(char *buffer, size_t size, double percent);

// Place the current value of every line of a chart in its gutter, one row each,
// inside the data region: below the legend row and its clear row, above the
// time axis. values has one entry per plot line, in plot line order, and a
// value that is not a number gets no readout at all.
//
// A value sits, as far as it can, on the row its own line ends on, so that the
// number is level with the end of its curve. Values whose rows collide are
// moved apart onto separate rows, the highest value of them all staying
// highest on the screen and the lowest one lowest, the lines going down the
// gutter in order of decreasing value and of increasing line index when two
// values are equal. When the data region cannot hold every readout, the ones
// that would leave it are left out, from the lowest value up.
//
// Returns the number of readouts placed, at most max_readouts, and fills
// readouts in order of increasing row. Returns 0 when the window is too short
// to hold the legend and a single data row.
unsigned nvtop_place_plot_readouts(unsigned window_rows, const double *values, unsigned num_values,
                                   struct plot_readout *readouts, unsigned max_readouts);

#endif // PLOT_GEOMETRY_H__
