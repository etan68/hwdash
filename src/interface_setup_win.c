/*
 *
 * Copyright (C) 2021 Maxime Schmitt <maxime.schmitt91@gmail.com>
 *
 * This file is part of Nvtop.
 *
 * Nvtop is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Nvtop is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with nvtop.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "nvtop/interface.h"
#include "nvtop/interface_internal_common.h"
#include "nvtop/interface_options.h"
#include "nvtop/interface_ring_buffer.h"
#include "nvtop/interface_setup_win.h"

#include <ncurses.h>
#include <string.h>

static char *setup_window_category_names[setup_window_selection_count] = {
    "General", "Devices", "GPU Display", "GPU Processes"};

// All the windows used to display the setup
enum setup_window_type {
  setup_window_type_setup,
  setup_window_type_single,
  setup_window_type_split_left,
  setup_window_type_split_right,
  setup_window_type_count
};

// General Options

enum setup_general_options {
  setup_general_color,
  setup_general_update_interval,
  setup_general_fahrenheit,
  setup_general_reverse_chart,
  setup_general_options_count
};

static const char *setup_general_option_description[setup_general_options_count] = {
    "Monochrome mode (requires save and restart)", "Update interval (seconds)",
    "Temperature in Fahrenheit", "Reverse chart direction"};

// Device Selection Options

enum setup_device_options {
  setup_device_host_toggle,
  setup_device_gpu_start, // dynamic: one per detected device
};

static const char *setup_device_host_description = "CPU and RAM";

// GPU Display Settings Options

enum setup_gpu_display_options {
  setup_gpu_display_enc_dec_timer,
  setup_gpu_display_info_bar,
  setup_gpu_display_color_start, // dynamic color rows: slots 0..slot_count-1
  // heading at color_start + slot_count (non-selectable)
  // all_gpu  = color_start + slot_count + 1
  // gpu_list = color_start + slot_count + 2
};

static const char *setup_gpu_display_enc_dec_description =
    "Encoder/Decoder idle hiding timer";
static const char *setup_gpu_display_info_bar_description =
    "Show extended GPU information";
static const char *setup_gpu_display_heading = "GPU Metric Selection";
static const char *setup_gpu_display_all_gpu_description = "Metrics for all GPUs";
static const char *setup_gpu_display_gpu_description = "Metrics for GPU";

static const char *setup_chart_gpu_value_descriptions[plot_information_count] = {
    "%s utilization rate",  "%s memory utilization rate",    "%s encoder rate",   "%s decoder rate",
    "%s temperature",       "Power draw rate (current/max)", "Fan speed",         "%s clock rate",
    "%s memory clock rate", "Effective load rate",           "PCIe RX load rate", "PCIe TX load rate",
    "HVX utilization rate", "HMX utilization rate"};

// Formats the description of a plot metric for the given compute unit label.
// Descriptions without a unit placeholder (power, fan, ...) are returned as-is.
static void format_metric_description(char *buf, size_t buflen, enum plot_information info, const char *unit) {
  snprintf(buf, buflen, setup_chart_gpu_value_descriptions[info], unit);
}

static const char *chart_color_names[] = {"Red", "Cyan", "Green", "Yellow", "Blue", "Magenta", "White"};
static const unsigned chart_color_names_count = ARRAY_SIZE(chart_color_names);

// Plot colors belong to line positions, not to particular metrics. Different
// GPUs may use different metrics in the same position, so the menu needs the
// largest number of lines used by any selected GPU rather than the number of
// distinct metrics in their union. Every individual mask is already capped at
// MAX_LINES_PER_PLOT by plot_add_draw_info().
static unsigned active_plot_slot_count(const struct nvtop_interface *interface) {
  unsigned slot_count = 0;
  for (unsigned i = 0; i < interface->monitored_dev_count; ++i) {
    unsigned device_slot_count = plot_count_draw_info(interface->options.gpu_specific_opts[i].to_draw);
    if (device_slot_count > slot_count)
      slot_count = device_slot_count;
  }
  return slot_count;
}

// Process List Options

enum setup_proc_list_options {
  setup_proc_list_show_process_list,
  setup_proc_list_hide_nvtop_process,
  setup_proc_list_sort_order,
  setup_proc_list_sort_field,
  setup_proc_list_columns,
  setup_proc_list_options_count
};

static const char *setup_proc_list_option_description[setup_proc_list_options_count] = {
    "Show process list", "Hide HWDash process", "Sort order", "Sort field", "Visible columns"};

static const char *setup_proc_list_value_descriptions[process_field_count] = {
    "Process Id",    "User name",        "Device Id", "Workload type",    "GPU usage", "Encoder usage",
    "Decoder usage", "GPU memory usage", "CPU usage", "CPU memory usage", "Command"};

static unsigned int sizeof_setup_windows[setup_window_type_count] = {[setup_window_type_setup] = 14,
                                                                     [setup_window_type_single] = 0,
                                                                     [setup_window_type_split_left] = 26,
                                                                     [setup_window_type_split_right] = 0};

// For toggle options
// Show * if on, - if partial and nothing if off
enum option_state {
  option_off,
  option_on,
  option_partially_active,
};

static char option_state_char(enum option_state state) {
  switch (state) {
  case option_on:
    return '*';
  case option_partially_active:
    return '-';
  case option_off:
    return ' ';
  default:
    return ' ';
  }
}

void alloc_setup_window(struct window_position *position, struct setup_window *setup_win) {
  setup_win->visible = false;
  setup_win->clean_space = newwin(position->sizeY, position->sizeX, position->posY, position->posX);

  sizeof_setup_windows[setup_window_type_single] = position->sizeX - sizeof_setup_windows[setup_window_type_setup] - 1;
  if (sizeof_setup_windows[setup_window_type_single] > position->sizeX)
    sizeof_setup_windows[setup_window_type_single] = 0;

  sizeof_setup_windows[setup_window_type_split_right] = position->sizeX -
                                                        sizeof_setup_windows[setup_window_type_setup] -
                                                        sizeof_setup_windows[setup_window_type_split_left] - 2;
  if (sizeof_setup_windows[setup_window_type_split_right] > position->sizeX)
    sizeof_setup_windows[setup_window_type_split_right] = 0;

  setup_win->setup =
      newwin(position->sizeY, sizeof_setup_windows[setup_window_type_setup], position->posY, position->posX);

  setup_win->single = newwin(position->sizeY, sizeof_setup_windows[setup_window_type_single], position->posY,
                             position->posX + sizeof_setup_windows[setup_window_type_setup] + 1);

  setup_win->split[0] = newwin(position->sizeY, sizeof_setup_windows[setup_window_type_split_left], position->posY,
                               position->posX + sizeof_setup_windows[setup_window_type_setup] + 1);

  setup_win->split[1] = newwin(position->sizeY, sizeof_setup_windows[setup_window_type_split_right], position->posY,
                               position->posX + sizeof_setup_windows[setup_window_type_setup] +
                                   sizeof_setup_windows[setup_window_type_split_left] + 2);
}

void free_setup_window(struct setup_window *setup_win) {
  delwin(setup_win->clean_space);
  delwin(setup_win->setup);
  delwin(setup_win->single);
  delwin(setup_win->split[0]);
  delwin(setup_win->split[1]);
}

void show_setup_window(struct nvtop_interface *interface) {
  interface->setup_win.visible = true;
  touchwin(interface->setup_win.clean_space);
  wnoutrefresh(interface->setup_win.clean_space);
  interface->setup_win.selected_section = setup_general_selected;
  interface->setup_win.indentation_level = 0;
  interface->setup_win.options_selected[0] = 0;
  interface->setup_win.options_selected[1] = 0;
}

void hide_setup_window(struct nvtop_interface *interface) { interface->setup_win.visible = false; }

static void draw_setup_window_setup(struct nvtop_interface *interface) {
  werase(interface->setup_win.setup);
  mvwprintw(interface->setup_win.setup, 0, 0, "Setup");
  mvwchgat(interface->setup_win.setup, 0, 0, sizeof_setup_windows[setup_window_type_setup], A_STANDOUT, green_color,
           NULL);
  for (enum setup_window_section category = setup_general_selected; category < setup_window_selection_count;
       ++category) {
    mvwprintw(interface->setup_win.setup, category + 1, 0, "%s", setup_window_category_names[category]);
    if (interface->setup_win.selected_section == category) {
      if (interface->setup_win.indentation_level == 0) {
        set_attribute_between(interface->setup_win.setup, category + 1, 0,
                              sizeof_setup_windows[setup_window_type_setup], A_STANDOUT, cyan_color);
      } else {
        mvwprintw(interface->setup_win.setup, category + 1, sizeof_setup_windows[setup_window_type_setup] - 1, ">");
        set_attribute_between(interface->setup_win.setup, category + 1, 0,
                              sizeof_setup_windows[setup_window_type_setup], A_BOLD, cyan_color);
      }
    }
  }
  wnoutrefresh(interface->setup_win.setup);
}

static void draw_setup_window_general(struct nvtop_interface *interface) {
  if (interface->setup_win.indentation_level > 1)
    interface->setup_win.indentation_level = 1;
  if (interface->setup_win.indentation_level == 1 &&
      interface->setup_win.options_selected[0] >= setup_general_options_count)
    interface->setup_win.options_selected[0] = setup_general_options_count - 1;

  WINDOW *win = interface->setup_win.single;
  wattr_set(win, A_STANDOUT, green_color, NULL);
  mvwprintw(win, 0, 0, "General Display Settings");
  wstandend(win);

  unsigned int cur_col, maxcols, tmp;
  (void)tmp;
  getmaxyx(win, tmp, maxcols);
  getyx(win, tmp, cur_col);
  mvwchgat(win, 0, cur_col, maxcols - cur_col, A_STANDOUT, green_color, NULL);

  // Monochrome mode
  enum option_state option_state = !interface->options.use_color;
  mvwprintw(win, setup_general_color + 1, 0, "[%c] %s", option_state_char(option_state),
            setup_general_option_description[setup_general_color]);
  if (interface->setup_win.indentation_level == 1 &&
      interface->setup_win.options_selected[0] == setup_general_color) {
    mvwchgat(win, setup_general_color + 1, 0, 3, A_STANDOUT, cyan_color, NULL);
  }

  // Update interval
  int update_deciseconds = (interface->options.update_interval / 100) % 10;
  int update_seconds = interface->options.update_interval / 1000;
  mvwprintw(win, setup_general_update_interval + 1, 0, "[%2u.%u] %s", update_seconds,
            update_deciseconds, setup_general_option_description[setup_general_update_interval]);
  if (interface->setup_win.indentation_level == 1 &&
      interface->setup_win.options_selected[0] == setup_general_update_interval) {
    mvwchgat(win, setup_general_update_interval + 1, 0, 6, A_STANDOUT, cyan_color, NULL);
  }

  // Temperature in Fahrenheit
  option_state = interface->options.temperature_in_fahrenheit;
  mvwprintw(win, setup_general_fahrenheit + 1, 0, "[%c] %s", option_state_char(option_state),
            setup_general_option_description[setup_general_fahrenheit]);
  if (interface->setup_win.indentation_level == 1 &&
      interface->setup_win.options_selected[0] == setup_general_fahrenheit) {
    mvwchgat(win, setup_general_fahrenheit + 1, 0, 3, A_STANDOUT, cyan_color, NULL);
  }

  // Reverse chart direction
  option_state = interface->options.plot_left_to_right;
  mvwprintw(win, setup_general_reverse_chart + 1, 0, "[%c] %s", option_state_char(option_state),
            setup_general_option_description[setup_general_reverse_chart]);
  if (interface->setup_win.indentation_level == 1 &&
      interface->setup_win.options_selected[0] == setup_general_reverse_chart) {
    mvwchgat(win, setup_general_reverse_chart + 1, 0, 3, A_STANDOUT, cyan_color, NULL);
  }

  wnoutrefresh(win);
}

static void draw_setup_window_devices(unsigned total_dev_count, struct nvtop_interface *interface) {
  unsigned option_count = setup_device_gpu_start + total_dev_count;
  if (interface->setup_win.indentation_level > 1)
    interface->setup_win.indentation_level = 1;
  if (interface->setup_win.options_selected[0] >= option_count)
    interface->setup_win.options_selected[0] = option_count - 1;

  WINDOW *win = interface->setup_win.single;
  wattr_set(win, A_STANDOUT, green_color, NULL);
  mvwprintw(win, 0, 0, "Device Selection");
  wstandend(win);

  unsigned int cur_col, maxcols, tmp;
  (void)tmp;
  getmaxyx(win, tmp, maxcols);
  getyx(win, tmp, cur_col);
  mvwchgat(win, 0, cur_col, maxcols - cur_col, A_STANDOUT, green_color, NULL);

  // CPU and RAM combined toggle
  enum option_state option_state = interface->options.show_host_cpu_usage || interface->options.show_host_mem_usage;
  mvwprintw(win, setup_device_host_toggle + 1, 0, "[%c] %s", option_state_char(option_state),
            setup_device_host_description);
  if (interface->setup_win.indentation_level == 1 &&
      interface->setup_win.options_selected[0] == setup_device_host_toggle) {
    mvwchgat(win, setup_device_host_toggle + 1, 0, 3, A_STANDOUT, cyan_color, NULL);
  }

  // Per-device monitor toggles
  for (unsigned devId = 0; devId < total_dev_count; ++devId) {
    unsigned row = setup_device_gpu_start + devId;
    option_state = !interface->options.gpu_specific_opts[devId].doNotMonitor;
    mvwprintw(win, row + 1, 0, "[%c] %s", option_state_char(option_state),
              interface->options.gpu_specific_opts[devId].linkedGpu->static_info.device_name);
    if (interface->setup_win.indentation_level == 1 && interface->setup_win.options_selected[0] == row)
      mvwchgat(win, row + 1, 0, 3, A_STANDOUT, cyan_color, NULL);
  }

  wnoutrefresh(win);
}

static void draw_setup_window_gpu_display(unsigned devices_count, struct list_head *devices,
                                          struct nvtop_interface *interface) {
  WINDOW *option_list_win;

  unsigned slot_count = active_plot_slot_count(interface);

  // Row layout:
  // 0: enc/dec timer
  // 1: info bar
  // 2..2+slot_count-1: color rows (dynamic)
  // 2+slot_count: heading "GPU Metric Selection" (non-selectable)
  // 2+slot_count+1: all GPUs
  // 2+slot_count+2..: per-GPU
  unsigned heading_row = setup_gpu_display_color_start + slot_count;
  unsigned all_gpu_row = heading_row + 1;
  unsigned gpu_list_row = heading_row + 2;
  unsigned last_row = gpu_list_row + devices_count - 1;

  // Clamp selection, skipping the heading row
  if (interface->setup_win.options_selected[0] > last_row)
    interface->setup_win.options_selected[0] = last_row;
  if (interface->setup_win.options_selected[0] == heading_row)
    interface->setup_win.options_selected[0] = all_gpu_row;

  if (interface->setup_win.options_selected[0] < all_gpu_row) {
    if (interface->setup_win.indentation_level > 1)
      interface->setup_win.indentation_level = 1;
    option_list_win = interface->setup_win.single;
  } else {
    if (interface->setup_win.options_selected[1] >= plot_information_count)
      interface->setup_win.options_selected[1] = plot_information_count - 1;
    option_list_win = interface->setup_win.split[0];
  }

  werase(interface->setup_win.single);
  wnoutrefresh(interface->setup_win.single);
  werase(interface->setup_win.split[0]);
  wnoutrefresh(interface->setup_win.split[0]);
  werase(interface->setup_win.split[1]);
  wnoutrefresh(interface->setup_win.split[1]);

  wattr_set(option_list_win, A_STANDOUT, green_color, NULL);
  mvwprintw(option_list_win, 0, 0, "GPU Display Settings");
  wstandend(option_list_win);

  unsigned int cur_col, maxcols, tmp;
  (void)tmp;
  getmaxyx(option_list_win, tmp, maxcols);
  getyx(option_list_win, tmp, cur_col);
  mvwchgat(option_list_win, 0, cur_col, maxcols - cur_col, A_STANDOUT, green_color, NULL);

  enum option_state option_state;

  // Encoder/Decoder timer
  if (interface->options.encode_decode_hiding_timer > 0) {
    mvwprintw(option_list_win, setup_gpu_display_enc_dec_timer + 1, 0, "[%3.0fsec] %s",
              interface->options.encode_decode_hiding_timer, setup_gpu_display_enc_dec_description);
    if (interface->setup_win.indentation_level == 1 &&
        interface->setup_win.options_selected[0] == setup_gpu_display_enc_dec_timer) {
      mvwchgat(option_list_win, setup_gpu_display_enc_dec_timer + 1, 0, 8, A_STANDOUT, cyan_color, NULL);
    }
  } else {
    mvwprintw(option_list_win, setup_gpu_display_enc_dec_timer + 1, 0, "[always] %s",
              setup_gpu_display_enc_dec_description);
    if (interface->setup_win.indentation_level == 1 &&
        interface->setup_win.options_selected[0] == setup_gpu_display_enc_dec_timer) {
      mvwchgat(option_list_win, setup_gpu_display_enc_dec_timer + 1, 0, 8, A_STANDOUT, cyan_color, NULL);
    }
  }

  // Extended GPU info
  option_state = interface->options.has_gpu_info_bar;
  mvwprintw(option_list_win, setup_gpu_display_info_bar + 1, 0, "[%c] %s", option_state_char(option_state),
            setup_gpu_display_info_bar_description);
  if (interface->setup_win.indentation_level == 1 &&
      interface->setup_win.options_selected[0] == setup_gpu_display_info_bar) {
    mvwchgat(option_list_win, setup_gpu_display_info_bar + 1, 0, 3, A_STANDOUT, cyan_color, NULL);
  }

  // Dynamic color rows. A color applies to the same ordinal line on every
  // chart, even when those lines represent different metrics.
  struct gpu_info *first_device;
  const char *unit = "GPU";
  list_for_each_entry(first_device, devices, list) {
    unit = DEVICE_UNIT_NAME(first_device);
    break;
  }

  for (unsigned s = 0; s < slot_count; ++s) {
    unsigned row = setup_gpu_display_color_start + s;
    char color_row_buf[256];
    snprintf(color_row_buf, sizeof(color_row_buf), "[%s] Chart line %u color",
             chart_color_names[interface->options.gpu_plot_color_idx[s]], s + 1);
    mvwprintw(option_list_win, row + 1, 0, "%.*s", maxcols, color_row_buf);
    if (interface->setup_win.indentation_level == 1 &&
        interface->setup_win.options_selected[0] == row) {
      mvwchgat(option_list_win, row + 1, 0,
               (int)strlen(chart_color_names[interface->options.gpu_plot_color_idx[s]]) + 2,
               A_STANDOUT, cyan_color, NULL);
    }
  }

  // Non-selectable heading
  wattr_set(option_list_win, A_BOLD, green_color, NULL);
  mvwprintw(option_list_win, heading_row + 1, 0, "-- %s --", setup_gpu_display_heading);
  wstandend(option_list_win);

  // All GPUs row
  if (interface->setup_win.options_selected[0] == all_gpu_row) {
    if (interface->setup_win.indentation_level == 1)
      wattr_set(option_list_win, A_STANDOUT, cyan_color, NULL);
    if (interface->setup_win.indentation_level == 2)
      wattr_set(option_list_win, A_BOLD, cyan_color, NULL);
  }
  mvwaddch(option_list_win, all_gpu_row + 1, 1, ACS_HLINE);
  waddch(option_list_win, '>');
  wstandend(option_list_win);
  wprintw(option_list_win, " %s", setup_gpu_display_all_gpu_description);

  // Per-GPU rows
  for (unsigned i = 0; i < devices_count; ++i) {
    if (interface->setup_win.options_selected[0] == gpu_list_row + i) {
      if (interface->setup_win.indentation_level == 1)
        wattr_set(option_list_win, A_STANDOUT, cyan_color, NULL);
      if (interface->setup_win.indentation_level == 2)
        wattr_set(option_list_win, A_BOLD, cyan_color, NULL);
    }
    mvwaddch(option_list_win, gpu_list_row + 1 + i, 1, ACS_HLINE);
    waddch(option_list_win, '>');
    wstandend(option_list_win);
    wprintw(option_list_win, " %s %u", setup_gpu_display_gpu_description, i);
  }
  wnoutrefresh(option_list_win);

  // Right panel: metric checklist
  if (interface->setup_win.options_selected[0] >= all_gpu_row) {
    WINDOW *value_list_win = interface->setup_win.split[1];
    wattr_set(value_list_win, A_STANDOUT, green_color, NULL);
    unsigned selected_gpu = interface->setup_win.options_selected[0] - gpu_list_row;
    if (interface->setup_win.options_selected[0] == all_gpu_row) {
      mvwprintw(value_list_win, 0, 0, "Displayed Metrics - All Selected GPUs");
    } else {
      mvwprintw(value_list_win, 0, 0, "Metric Displayed in Graph");
      struct gpu_info *device;
      unsigned index = 0;
      list_for_each_entry(device, devices, list) {
        if (index == selected_gpu)
          break;
        index++;
      }
      unit = DEVICE_UNIT_NAME(device);
      if (IS_VALID(gpuinfo_device_name_valid, device->static_info.valid)) {
        getmaxyx(value_list_win, tmp, maxcols);
        getyx(value_list_win, tmp, cur_col);
        wprintw(value_list_win, " (%.*s)", maxcols - cur_col - 3, device->static_info.device_name);
      } else {
        wprintw(value_list_win, " (%s %u)", unit, selected_gpu);
      }
    }
    wclrtoeol(value_list_win);
    getmaxyx(value_list_win, tmp, maxcols);
    getyx(value_list_win, tmp, cur_col);
    mvwchgat(value_list_win, 0, cur_col, maxcols - cur_col, A_STANDOUT, green_color, NULL);
    wattr_set(value_list_win, A_NORMAL, magenta_color, NULL);
    mvwprintw(value_list_win, 1, 0, "Maximum of 4 metrics per GPU");
    wstandend(value_list_win);

    for (enum plot_information i = plot_gpu_rate; i < plot_information_count; ++i) {
      if (interface->setup_win.options_selected[0] == all_gpu_row) {
        plot_info_to_draw draw_union = 0, draw_intersection = 0xffff;
        for (unsigned j = 0; j < devices_count; ++j) {
          draw_union |= interface->options.gpu_specific_opts[j].to_draw;
          draw_intersection = draw_intersection & interface->options.gpu_specific_opts[j].to_draw;
        }
        if (plot_isset_draw_info(i, draw_intersection)) {
          option_state = option_on;
        } else {
          if (plot_isset_draw_info(i, draw_union))
            option_state = option_partially_active;
          else
            option_state = option_off;
        }
      } else {
        option_state = plot_isset_draw_info(i, interface->options.gpu_specific_opts[selected_gpu].to_draw);
      }
      char description[64];
      format_metric_description(description, sizeof(description), i, unit);
      mvwprintw(value_list_win, i + 2, 0, "[%c] %s", option_state_char(option_state), description);
      if (interface->setup_win.indentation_level == 2 && interface->setup_win.options_selected[1] == i) {
        mvwchgat(value_list_win, i + 2, 0, 3, A_STANDOUT, cyan_color, NULL);
      }
    }
    wnoutrefresh(value_list_win);
  }
}

static void draw_setup_window_proc_list(struct nvtop_interface *interface) {
  WINDOW *option_list_win;
  if (interface->setup_win.options_selected[0] >= setup_proc_list_options_count)
    interface->setup_win.options_selected[0] = setup_proc_list_options_count - 1;
  if (interface->setup_win.options_selected[0] < setup_proc_list_sort_field) {
    option_list_win = interface->setup_win.single;
    if (interface->setup_win.indentation_level > 1)
      interface->setup_win.indentation_level = 1;
  } else {
    option_list_win = interface->setup_win.split[0];
    if (interface->setup_win.options_selected[0] == setup_proc_list_sort_field) {
      unsigned fields_count = process_field_displayed_count(interface->options.process_fields_displayed);
      if (!fields_count) {
        if (interface->setup_win.indentation_level > 1)
          interface->setup_win.indentation_level = 1;
      } else {
        if (interface->setup_win.options_selected[1] >= fields_count)
          interface->setup_win.options_selected[1] = fields_count - 1;
      }
    }
    if (interface->setup_win.options_selected[0] == setup_proc_list_columns) {
      if (interface->setup_win.options_selected[1] >= process_field_count)
        interface->setup_win.options_selected[1] = process_field_count - 1;
    }
  }

  werase(interface->setup_win.single);
  wnoutrefresh(interface->setup_win.single);
  touchwin(interface->setup_win.split[0]);
  touchwin(interface->setup_win.split[1]);

  wattr_set(option_list_win, A_STANDOUT, green_color, NULL);
  mvwprintw(option_list_win, 0, 0, "GPU Process List");
  wstandend(option_list_win);
  unsigned int cur_col, maxcols, tmp;
  (void)tmp;
  getmaxyx(option_list_win, tmp, maxcols);
  getyx(option_list_win, tmp, cur_col);
  mvwchgat(option_list_win, 0, cur_col, maxcols - cur_col, A_STANDOUT, green_color, NULL);

  enum option_state option_state;

  // Show process list (positive, backed by negative hide)
  option_state = !interface->options.hide_processes_list;
  mvwprintw(option_list_win, setup_proc_list_show_process_list + 1, 0, "[%c] %s", option_state_char(option_state),
            setup_proc_list_option_description[setup_proc_list_show_process_list]);
  if (interface->setup_win.indentation_level == 1 &&
      interface->setup_win.options_selected[0] == setup_proc_list_show_process_list) {
    mvwchgat(option_list_win, setup_proc_list_show_process_list + 1, 0, 3, A_STANDOUT, cyan_color, NULL);
  }

  // Hide HWDash process
  option_state = interface->options.filter_nvtop_pid;
  mvwprintw(option_list_win, setup_proc_list_hide_nvtop_process + 1, 0, "[%c] %s", option_state_char(option_state),
            setup_proc_list_option_description[setup_proc_list_hide_nvtop_process]);
  if (interface->setup_win.indentation_level == 1 &&
      interface->setup_win.options_selected[0] == setup_proc_list_hide_nvtop_process) {
    mvwchgat(option_list_win, setup_proc_list_hide_nvtop_process + 1, 0, 3, A_STANDOUT, cyan_color, NULL);
  }

  // Sort order
  option_state = !interface->options.sort_descending_order;
  mvwprintw(option_list_win, setup_proc_list_sort_order + 1, 0, "[%c] %s", option_state_char(option_state),
            setup_proc_list_option_description[setup_proc_list_sort_order]);
  if (interface->setup_win.indentation_level == 1 &&
      interface->setup_win.options_selected[0] == setup_proc_list_sort_order) {
    mvwchgat(option_list_win, setup_proc_list_sort_order + 1, 0, 3, A_STANDOUT, cyan_color, NULL);
  }

  // Sort field (opens right panel)
  if (interface->setup_win.options_selected[0] == setup_proc_list_sort_field) {
    if (interface->setup_win.indentation_level == 1)
      wattr_set(option_list_win, A_STANDOUT, cyan_color, NULL);
    if (interface->setup_win.indentation_level == 2)
      wattr_set(option_list_win, A_BOLD, cyan_color, NULL);
  }
  mvwaddch(option_list_win, setup_proc_list_sort_field + 1, 1, ACS_HLINE);
  waddch(option_list_win, '>');
  wstandend(option_list_win);
  wprintw(option_list_win, " %s", setup_proc_list_option_description[setup_proc_list_sort_field]);

  // Visible columns (opens right panel)
  if (interface->setup_win.options_selected[0] == setup_proc_list_columns) {
    if (interface->setup_win.indentation_level == 1)
      wattr_set(option_list_win, A_STANDOUT, cyan_color, NULL);
    if (interface->setup_win.indentation_level == 2)
      wattr_set(option_list_win, A_BOLD, cyan_color, NULL);
  }
  mvwaddch(option_list_win, setup_proc_list_columns + 1, 1, ACS_HLINE);
  waddch(option_list_win, '>');
  wstandend(option_list_win);
  wprintw(option_list_win, " %s", setup_proc_list_option_description[setup_proc_list_columns]);

  wnoutrefresh(option_list_win);

  // Right panel
  if (interface->setup_win.options_selected[0] >= setup_proc_list_sort_field) {
    WINDOW *value_list_win = interface->setup_win.split[1];
    if (interface->setup_win.options_selected[0] == setup_proc_list_sort_field) {
      wattr_set(value_list_win, A_STANDOUT, green_color, NULL);
      mvwprintw(value_list_win, 0, 0, "Processes are sorted by:");
      wstandend(value_list_win);
      wclrtoeol(value_list_win);
      getmaxyx(value_list_win, tmp, maxcols);
      getyx(value_list_win, tmp, cur_col);
      mvwchgat(value_list_win, 0, cur_col, maxcols - cur_col, A_STANDOUT, green_color, NULL);
      unsigned index = 0;
      for (enum process_field field = process_pid; field < process_field_count; ++field) {
        if (process_is_field_displayed(field, interface->options.process_fields_displayed)) {
          option_state = interface->options.sort_processes_by == field;
          mvwprintw(value_list_win, index + 1, 0, "[%c] %s", option_state_char(option_state),
                    setup_proc_list_value_descriptions[field]);
          wclrtoeol(value_list_win);
          if (interface->setup_win.indentation_level == 2 && interface->setup_win.options_selected[1] == index) {
            mvwchgat(value_list_win, index + 1, 0, 3, A_STANDOUT, cyan_color, NULL);
          }
          index++;
        }
      }
      if (!index) {
        wcolor_set(value_list_win, magenta_color, NULL);
        mvwprintw(value_list_win, 1, 0, "Nothing to sort: none of the process fields are displayed");
        wstandend(value_list_win);
      }
    }
    if (interface->setup_win.options_selected[0] == setup_proc_list_columns) {
      wattr_set(value_list_win, A_STANDOUT, green_color, NULL);
      mvwprintw(value_list_win, 0, 0, "Process Field Displayed:");
      wstandend(value_list_win);
      wclrtoeol(value_list_win);
      getmaxyx(value_list_win, tmp, maxcols);
      getyx(value_list_win, tmp, cur_col);
      mvwchgat(value_list_win, 0, cur_col, maxcols - cur_col, A_STANDOUT, green_color, NULL);
      for (enum process_field field = process_pid; field < process_field_count; ++field) {
        option_state = process_is_field_displayed(field, interface->options.process_fields_displayed);
        mvwprintw(value_list_win, field + 1, 0, "[%c] %s", option_state_char(option_state),
                  setup_proc_list_value_descriptions[field]);
        wclrtoeol(value_list_win);
        if (interface->setup_win.indentation_level == 2 && interface->setup_win.options_selected[1] == field) {
          mvwchgat(value_list_win, field + 1, 0, 3, A_STANDOUT, cyan_color, NULL);
        }
      }
    }
    wclrtobot(value_list_win);
    wnoutrefresh(value_list_win);
  }
}

static const char *setup_window_shortcuts[] = {"Enter", "ESC", "Arrow keys", "+/-", "F4"};

static const char *setup_window_shortcut_description[] = {"Toggle", "Exit", "Navigate Menu",
                                                          "Increment/Decrement Values", "Save Config"};

void draw_setup_window_shortcuts(struct nvtop_interface *interface) {
  WINDOW *window = interface->shortcut_window;

  wmove(window, 0, 0);
  for (size_t i = 0; i < ARRAY_SIZE(setup_window_shortcuts); ++i) {
    wprintw(window, "%s", setup_window_shortcuts[i]);
    wattr_set(window, A_STANDOUT, cyan_color, NULL);
    wprintw(window, "%s ", setup_window_shortcut_description[i]);
    wstandend(window);
  }
  wclrtoeol(window);
  unsigned int cur_col, tmp;
  (void)tmp;
  getyx(window, tmp, cur_col);
  mvwchgat(window, 0, cur_col, -1, A_STANDOUT, cyan_color, NULL);
  wnoutrefresh(window);
}

void draw_setup_window(unsigned devices_count, struct list_head *devices, struct nvtop_interface *interface) {
  // Device headers are refreshed before the setup screen on every frame. Clear
  // the full setup area first so they cannot show through unused menu rows or
  // the one-column gaps between the setup windows.
  werase(interface->setup_win.clean_space);
  wnoutrefresh(interface->setup_win.clean_space);

  draw_setup_window_setup(interface);
  switch (interface->setup_win.selected_section) {
  case setup_general_selected:
    draw_setup_window_general(interface);
    break;
  case setup_devices_selected:
    draw_setup_window_devices(interface->total_dev_count, interface);
    break;
  case setup_gpu_display_selected:
    draw_setup_window_gpu_display(devices_count, devices, interface);
    break;
  case setup_process_list_selected:
    draw_setup_window_proc_list(interface);
    break;
  default:
    break;
  }
}

// Navigate up/down within the GPU Display section, skipping the heading row.
static void navigate_gpu_display_up(struct nvtop_interface *interface) {
  unsigned slot_count = active_plot_slot_count(interface);
  unsigned heading_row = setup_gpu_display_color_start + slot_count;

  if (interface->setup_win.options_selected[0] == 0)
    return;
  interface->setup_win.options_selected[0]--;
  if (interface->setup_win.options_selected[0] == heading_row)
    interface->setup_win.options_selected[0] = heading_row - 1;
}

static void navigate_gpu_display_down(struct nvtop_interface *interface, unsigned devices_count) {
  unsigned slot_count = active_plot_slot_count(interface);
  unsigned heading_row = setup_gpu_display_color_start + slot_count;
  unsigned last_row = heading_row + 2 + devices_count - 1;

  if (interface->setup_win.options_selected[0] >= last_row)
    return;
  interface->setup_win.options_selected[0]++;
  if (interface->setup_win.options_selected[0] == heading_row)
    interface->setup_win.options_selected[0] = heading_row + 1;
}

void handle_setup_win_keypress(int keyId, struct nvtop_interface *interface) {
  if (interface->setup_win.visible) {
    switch (keyId) {

    case 'l':
    case KEY_RIGHT:
      if (interface->setup_win.indentation_level < 2)
        interface->setup_win.indentation_level++;
      break;

    case 'h':
    case KEY_LEFT:
      if (interface->setup_win.indentation_level > 0)
        interface->setup_win.indentation_level--;
      break;

    case 'k':
    case KEY_UP:
      if (interface->setup_win.indentation_level == 0) {
        if (interface->setup_win.selected_section != setup_general_selected) {
          interface->setup_win.selected_section--;
          interface->setup_win.options_selected[0] = 0;
          interface->setup_win.options_selected[1] = 0;
          werase(interface->setup_win.single);
          werase(interface->setup_win.split[0]);
          werase(interface->setup_win.split[1]);
          wnoutrefresh(interface->setup_win.single);
        }
      } else if (interface->setup_win.selected_section == setup_gpu_display_selected &&
                 interface->setup_win.indentation_level == 1) {
        navigate_gpu_display_up(interface);
      } else {
        if (interface->setup_win.indentation_level == 1)
          interface->setup_win.options_selected[1] = 0;
        if (interface->setup_win.options_selected[interface->setup_win.indentation_level - 1] != 0)
          interface->setup_win.options_selected[interface->setup_win.indentation_level - 1]--;
      }
      break;

    case 'j':
    case KEY_DOWN:
      if (interface->setup_win.indentation_level == 0) {
        if (interface->setup_win.selected_section + 1 != setup_window_selection_count) {
          interface->setup_win.selected_section++;
          interface->setup_win.options_selected[0] = 0;
          interface->setup_win.options_selected[1] = 0;
          werase(interface->setup_win.single);
          werase(interface->setup_win.split[0]);
          werase(interface->setup_win.split[1]);
          wnoutrefresh(interface->setup_win.single);
        }
      } else if (interface->setup_win.selected_section == setup_gpu_display_selected &&
                 interface->setup_win.indentation_level == 1) {
        navigate_gpu_display_down(interface, interface->monitored_dev_count);
      } else {
        if (interface->setup_win.indentation_level == 1)
          interface->setup_win.options_selected[1] = 0;
        interface->setup_win.options_selected[interface->setup_win.indentation_level - 1]++;
      }
      break;

    case '+':
      if (interface->setup_win.selected_section == setup_general_selected) {
        if (interface->setup_win.options_selected[0] == setup_general_update_interval) {
          if (interface->options.update_interval <= 99800)
            interface->options.update_interval += 100;
        }
      }
      if (interface->setup_win.selected_section == setup_gpu_display_selected) {
        if (interface->setup_win.indentation_level == 1) {
          if (interface->setup_win.options_selected[0] == setup_gpu_display_enc_dec_timer) {
            interface->options.encode_decode_hiding_timer += 5.;
          }
        }
      }
      break;
    case '-':
      if (interface->setup_win.selected_section == setup_general_selected) {
        if (interface->setup_win.options_selected[0] == setup_general_update_interval) {
          if (interface->options.update_interval >= 200)
            interface->options.update_interval -= 100;
        }
      }
      if (interface->setup_win.selected_section == setup_gpu_display_selected) {
        if (interface->setup_win.indentation_level == 1) {
          if (interface->setup_win.options_selected[0] == setup_gpu_display_enc_dec_timer) {
            interface->options.encode_decode_hiding_timer -= 5.;
            if (interface->options.encode_decode_hiding_timer < 0.) {
              interface->options.encode_decode_hiding_timer = 0.;
            }
          }
        }
      }
      break;

    case '\n':
    case KEY_ENTER:
      if (interface->setup_win.indentation_level == 0) {
        handle_setup_win_keypress(KEY_RIGHT, interface);
        return;
      }
      // General Display Settings
      if (interface->setup_win.selected_section == setup_general_selected) {
        if (interface->setup_win.options_selected[0] == setup_general_color) {
          interface->options.use_color = !interface->options.use_color;
        }
        if (interface->setup_win.options_selected[0] == setup_general_fahrenheit) {
          interface->options.temperature_in_fahrenheit = !interface->options.temperature_in_fahrenheit;
        }
        if (interface->setup_win.options_selected[0] == setup_general_reverse_chart) {
          interface->options.plot_left_to_right = !interface->options.plot_left_to_right;
        }
      }
      // Device Selection
      if (interface->setup_win.selected_section == setup_devices_selected) {
        if (interface->setup_win.indentation_level == 1) {
          if (interface->setup_win.options_selected[0] == setup_device_host_toggle) {
            bool new_val = !(interface->options.show_host_cpu_usage || interface->options.show_host_mem_usage);
            interface->options.show_host_cpu_usage = new_val;
            interface->options.show_host_mem_usage = new_val;
          } else if (interface->setup_win.options_selected[0] >= setup_device_gpu_start) {
            unsigned dev_idx = interface->setup_win.options_selected[0] - setup_device_gpu_start;
            interface->options.gpu_specific_opts[dev_idx].doNotMonitor =
                !interface->options.gpu_specific_opts[dev_idx].doNotMonitor;
            interface->options.has_monitored_set_changed = true;
          }
        }
      }
      // GPU Display Settings
      if (interface->setup_win.selected_section == setup_gpu_display_selected) {
        unsigned slot_count_kp = active_plot_slot_count(interface);
        unsigned heading_kp = setup_gpu_display_color_start + slot_count_kp;
        unsigned all_gpu_kp = heading_kp + 1;
        unsigned gpu_list_kp = heading_kp + 2;

        if (interface->setup_win.indentation_level == 1) {
          if (interface->setup_win.options_selected[0] == setup_gpu_display_enc_dec_timer) {
            if (interface->options.encode_decode_hiding_timer > 0.) {
              interface->options.encode_decode_hiding_timer = 0.;
            } else {
              interface->options.encode_decode_hiding_timer = 30.;
            }
          }
          if (interface->setup_win.options_selected[0] == setup_gpu_display_info_bar) {
            interface->options.has_gpu_info_bar = !interface->options.has_gpu_info_bar;
          }
          // Color rows
          unsigned sel = interface->setup_win.options_selected[0];
          if (sel >= setup_gpu_display_color_start && sel < heading_kp) {
            unsigned slot = sel - setup_gpu_display_color_start;
            interface->options.gpu_plot_color_idx[slot] =
                (interface->options.gpu_plot_color_idx[slot] + 1) % chart_color_names_count;
            apply_plot_colors(interface->options.gpu_plot_color_idx);
          }
          if (interface->setup_win.options_selected[0] >= all_gpu_kp) {
            handle_setup_win_keypress(KEY_RIGHT, interface);
          }
        } else if (interface->setup_win.indentation_level == 2) {
          bool selected_all_gpus = interface->setup_win.options_selected[0] == all_gpu_kp;
          unsigned selected_gpu_offset =
              interface->setup_win.options_selected[0] > all_gpu_kp
                  ? interface->setup_win.options_selected[0] - gpu_list_kp
                  : 0;

          if (selected_all_gpus) {
            plot_info_to_draw draw_intersection = 0xffff;
            for (unsigned j = 0; j < interface->monitored_dev_count; ++j) {
              draw_intersection = draw_intersection & interface->options.gpu_specific_opts[j].to_draw;
            }
            if (plot_isset_draw_info(interface->setup_win.options_selected[1], draw_intersection)) {
              for (unsigned i = 0; i < interface->monitored_dev_count; ++i) {
                interface->options.gpu_specific_opts[i].to_draw = plot_remove_draw_info(
                    interface->setup_win.options_selected[1], interface->options.gpu_specific_opts[i].to_draw);
                interface_ring_buffer_empty(&interface->saved_data_ring, i);
              }
            } else {
              for (unsigned i = 0; i < interface->monitored_dev_count; ++i) {
                interface->options.gpu_specific_opts[i].to_draw = plot_add_draw_info(
                    interface->setup_win.options_selected[1], interface->options.gpu_specific_opts[i].to_draw);
                interface_ring_buffer_empty(&interface->saved_data_ring, i);
              }
            }
          }
          if (interface->setup_win.options_selected[0] > all_gpu_kp) {
            unsigned selected_gpu = interface->setup_win.options_selected[0] - gpu_list_kp;
            if (plot_isset_draw_info(interface->setup_win.options_selected[1],
                                     interface->options.gpu_specific_opts[selected_gpu].to_draw))
              interface->options.gpu_specific_opts[selected_gpu].to_draw = plot_remove_draw_info(
                  interface->setup_win.options_selected[1], interface->options.gpu_specific_opts[selected_gpu].to_draw);
            else
              interface->options.gpu_specific_opts[selected_gpu].to_draw = plot_add_draw_info(
                  interface->setup_win.options_selected[1], interface->options.gpu_specific_opts[selected_gpu].to_draw);
            interface_ring_buffer_empty(&interface->saved_data_ring, selected_gpu);
          }
          // Re-anchor selection after color row count changes
          if (selected_all_gpus || interface->setup_win.options_selected[0] > all_gpu_kp) {
            unsigned heading_after = setup_gpu_display_color_start + active_plot_slot_count(interface);
            unsigned all_gpu_after = heading_after + 1;
            if (selected_all_gpus)
              interface->setup_win.options_selected[0] = all_gpu_after;
            else
              interface->setup_win.options_selected[0] = all_gpu_after + 1 + selected_gpu_offset;
          }
        }
      }
      // Process List Options
      if (interface->setup_win.selected_section == setup_process_list_selected) {
        if (interface->setup_win.indentation_level == 1) {
          if (interface->setup_win.options_selected[0] == setup_proc_list_sort_order) {
            interface->options.sort_descending_order = !interface->options.sort_descending_order;
          } else if (interface->setup_win.options_selected[0] == setup_proc_list_hide_nvtop_process) {
            interface->options.filter_nvtop_pid = !interface->options.filter_nvtop_pid;
          } else if (interface->setup_win.options_selected[0] == setup_proc_list_show_process_list) {
            interface->options.hide_processes_list = !interface->options.hide_processes_list;
          } else if (interface->setup_win.options_selected[0] == setup_proc_list_sort_field) {
            handle_setup_win_keypress(KEY_RIGHT, interface);
          } else if (interface->setup_win.options_selected[0] == setup_proc_list_columns) {
            handle_setup_win_keypress(KEY_RIGHT, interface);
          }
        } else if (interface->setup_win.indentation_level == 2) {
          if (interface->setup_win.options_selected[0] == setup_proc_list_sort_field) {
            unsigned index = 0;
            for (enum process_field field = process_pid; field < process_field_count; ++field) {
              if (process_is_field_displayed(field, interface->options.process_fields_displayed)) {
                if (index == interface->setup_win.options_selected[1])
                  interface->options.sort_processes_by = field;
                index++;
              }
            }
          }
          if (interface->setup_win.options_selected[0] == setup_proc_list_columns) {
            if (process_is_field_displayed(interface->setup_win.options_selected[1],
                                           interface->options.process_fields_displayed)) {
              interface->options.process_fields_displayed = process_remove_field_to_display(
                  interface->setup_win.options_selected[1], interface->options.process_fields_displayed);
            } else {
              interface->options.process_fields_displayed = process_add_field_to_display(
                  interface->setup_win.options_selected[1], interface->options.process_fields_displayed);
            }
            if (!process_is_field_displayed(interface->options.sort_processes_by,
                                            interface->options.process_fields_displayed)) {
              interface->options.sort_processes_by =
                  process_default_sort_by_from(interface->options.process_fields_displayed);
            }
          }
        }
      }
      break;
    case KEY_F(2):
    case 27:
      interface->setup_win.visible = false;
      update_window_size_to_terminal_size(interface);
      break;
    case KEY_F(4):
      save_interface_options_to_config_file(interface->total_dev_count, &interface->options);
      break;
    default:
      break;
    }
  }
}
