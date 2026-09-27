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
// The value begins immediately after the frame. Keep one blank column at the
// outer edge of the section.
#define PLOT_READOUT_LEFT_PAD 0u
#define PLOT_READOUT_RIGHT_PAD 1u
#define PLOT_READOUT_GUTTER_SIZE (PLOT_READOUT_LEFT_PAD + PLOT_READOUT_VALUE_WIDTH + PLOT_READOUT_RIGHT_PAD)

// One shared display column carries every line to the right frame without
// adding another history sample.
#define PLOT_EDGE_EXTENSION 1u

// Columns of a chart that can never hold a data column: the labels, the axis,
// the two borders and the readout gutter. What is left of the width the layout
// gave the chart is shared out between the lines it draws.
#define PLOT_COLUMNS_NOT_DATA (PLOT_HORIZONTAL_OVERHEAD + PLOT_READOUT_GUTTER_SIZE + PLOT_EDGE_EXTENSION)

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

// The first drawn sample touches the left Y axis in either time direction. It
// starts a segment; the following sample connects to it. When the first sample
// scrolls out, its transition leaves in the same refresh.
unsigned nvtop_plot_first_drawn_column(bool reversed_time_axis);

// What to draw for the sample of a line at a column. Column zero starts the
// visible segment and every later sample connects normally.
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
