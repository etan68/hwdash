#include "nvtop/plot_geometry.h"

#include <math.h>

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

bool nvtop_plot_connect_from_previous(unsigned sample_column) { return sample_column > 1u; }
