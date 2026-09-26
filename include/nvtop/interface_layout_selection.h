#ifndef INTERFACE_LAYOUT_SELECTION_H__
#define INTERFACE_LAYOUT_SELECTION_H__

#include "nvtop/interface_common.h"
#include "nvtop/interface_options.h"

#include <stdbool.h>

struct window_position {
  unsigned posX, posY, sizeX, sizeY;
};

// Should be fine
#define MAX_CHARTS 64

// Request for the combined whole host chart. The CPU and the memory
// utilization share a single chart, the way the GPU utilization and the GPU
// memory share a GPU chart. The host chart is an ordinary chart: it gets the
// same outer dimensions, the same row height and the full width of a chart
// row, and it owns the first chart row, right below the device headers and
// above all the GPU chart rows.
struct host_chart_input {
  bool show;          // At least one of the two host metrics is enabled
  unsigned num_lines; // Number of percentage lines of the chart: 1 or 2
};

void compute_sizes_from_layout(unsigned monitored_dev_count, unsigned device_header_rows, unsigned device_header_cols,
                               unsigned rows, unsigned cols, const nvtop_interface_gpu_opts *gpu_opts,
                               process_field_displayed process_field_displayed,
                               struct window_position *device_positions, unsigned *num_plots,
                               struct window_position plot_positions[MAX_CHARTS], unsigned *map_device_to_plot,
                               struct window_position *process_position, struct window_position *setup_position,
                               bool process_win_hide, const struct host_chart_input *host_chart,
                               struct window_position *host_chart_position);

#endif // INTERFACE_LAYOUT_SELECTION_H__
