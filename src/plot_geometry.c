#include "nvtop/plot_geometry.h"

#include <math.h>
#include <stdio.h>

int nvtop_plot_data_level(unsigned window_rows, double percent) {
  if (window_rows <= PLOT_DATA_TOP_ROW || !isfinite(percent))
    return -1;
  if (percent < 0.)
    percent = 0.;
  else if (percent > 100.)
    percent = 100.;

  const unsigned bottom = window_rows - 1u;
  const unsigned span = bottom - PLOT_DATA_TOP_ROW;
  return (int)bottom - (int)lround(percent * (double)span / 100.);
}

unsigned nvtop_plot_first_drawn_column(bool reversed_time_axis) {
  (void)reversed_time_axis;
  return 0u;
}

enum nvtop_plot_sample_action nvtop_plot_sample_action_at(bool reversed_time_axis, unsigned sample_column) {
  (void)reversed_time_axis;
  if (sample_column > 0u)
    return nvtop_plot_sample_connect;
  return nvtop_plot_sample_point;
}

bool nvtop_plot_newest_drawn_value(size_t num_columns, bool reversed_time_axis, const double *data, unsigned num_lines,
                                   unsigned line, double *value) {
  if (!data || num_lines == 0u || line >= num_lines || num_columns == 0u)
    return false;

  // The newest sample is at the right edge of the chart, or at the left edge
  // when the time axis is reversed, and the samples get older from there.
  size_t column = reversed_time_axis ? 0u : num_columns - 1u;
  const size_t oldest_drawn = nvtop_plot_first_drawn_column(reversed_time_axis);
  for (;;) {
    if (column >= oldest_drawn) {
      const double sample = data[column * num_lines + line];
      if (isfinite(sample)) {
        if (value)
          *value = sample;
        return true;
      }
    }
    if (reversed_time_axis) {
      if (column + 1u >= num_columns)
        return false;
      column++;
    } else {
      if (column <= oldest_drawn)
        return false;
      column--;
    }
  }
}

bool nvtop_format_plot_readout(char *buffer, size_t size, double percent) {
  if (!buffer || size == 0u || !isfinite(percent))
    return false;
  double clamped = percent;
  if (clamped < 0.)
    clamped = 0.;
  else if (clamped > 100.)
    clamped = 100.;
  if (clamped == 0.)
    clamped = 0.; // A value that rounds to nothing reads 0.0%, never -0.0%

  const int written = snprintf(buffer, size, "%*.*f%%", (int)PLOT_READOUT_VALUE_WIDTH - 1, 1, clamped);
  return written > 0 && (size_t)written < size;
}

// The row the first readout of a stack takes when the last one is pulled up to
// bottom_row and every readout stays strictly above the one below it. The
// readouts below it keep the rows they already have.
static int squeezed_first_row(const struct plot_readout *readouts, unsigned count, int bottom_row) {
  int above = bottom_row + 1; // The last readout of the stack may use bottom_row
  for (unsigned i = count; i-- > 0;) {
    const int room_above = above - 1;
    above = readouts[i].row < room_above ? readouts[i].row : room_above;
  }
  return above;
}

unsigned nvtop_place_plot_readouts(unsigned window_rows, const double *values, unsigned num_values,
                                   struct plot_readout *readouts, unsigned max_readouts) {
  if (!values || !readouts || num_values == 0u || max_readouts == 0u || window_rows <= PLOT_DATA_TOP_ROW)
    return 0;

  const int top = (int)PLOT_DATA_TOP_ROW;
  const int bottom = (int)window_rows - 1;

  unsigned placed = 0u;
  for (;;) {
    // The next readout to place is the one with the highest value that has no
    // row yet, the lower plot line winning the ties.
    unsigned line = num_values;
    for (unsigned candidate = 0; candidate < num_values; ++candidate) {
      if (!isfinite(values[candidate]))
        continue;
      bool already_placed = false;
      for (unsigned i = 0; i < placed; ++i)
        already_placed = already_placed || readouts[i].line == candidate;
      if (already_placed)
        continue;
      if (line == num_values || values[candidate] > values[line])
        line = candidate;
    }
    if (line == num_values || placed == max_readouts)
      return placed;

    // As low on the screen as its own value asks for, and never on the row of
    // the readout above it, which stands for a higher value.
    int row = nvtop_plot_data_level(window_rows, values[line]);
    if (row < 0)
      return placed;
    if (placed > 0u && row <= readouts[placed - 1u].row)
      row = readouts[placed - 1u].row + 1;

    // Where the readouts already placed would have to go if that one joined
    // them. The data region is all the room there is: what cannot be pushed
    // into it leaves the gutter, and so do the lower values after it.
    readouts[placed].line = line;
    readouts[placed].row = row;
    if (squeezed_first_row(readouts, placed + 1u, bottom) < top)
      return placed;

    if (readouts[placed].row > bottom)
      readouts[placed].row = bottom;
    for (unsigned i = placed; i-- > 0;) {
      const int room_above = i + 1u < placed + 1u ? readouts[i + 1u].row - 1 : bottom;
      if (readouts[i].row > room_above)
        readouts[i].row = room_above;
    }
    readouts[placed].value = values[line];
    ++placed;
  }
}
