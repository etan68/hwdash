# HWDash

What is hwdash?
----------------

hwdash is a terminal hardware monitor for host CPU, RAM, GPUs, accelerators and
their processes. It is based on [nvtop](https://github.com/Syllo/nvtop) and keeps
nvtop's multi-vendor hardware collection backends while developing an independent
device-section interface and host monitoring layer.

This is an independent project and is not an official nvtop release. Existing
nvtop copyright and license notices are retained in the inherited source files.

Currently supported vendors are AMD (Linux amdgpu driver), Apple (limited M1 &
M2 support), Huawei (Ascend), Intel (Linux i915/Xe drivers), NVIDIA (Linux
proprietary divers), Qualcomm Adreno (Linux MSM driver), Broadcom VideoCore (Linux v3d driver),
Rockchip, MetaX (MXSML driver), Enflame (Linux EFML driver), Tenstorrent (Linux tt-kmd driver).
Rockchip, MetaX (MXSML driver), Enflame (Linux EFML driver), Iluvatar CoreX (ixML / libixml).

Because a picture is worth a thousand words:

![hwdash interface](/screenshot/NVTOP_ex1.png)

Table of Contents
-----------------

- [hwdash Options and Interactive Commands](#hwdash-options-and-interactive-commands)
  - [Interactive Setup Window](#interactive-setup-window)
  - [Saving Preferences](#saving-preferences)
  - [hwdash Manual and Command line Options](#hwdash-manual-and-command-line-options)
- [Host CPU and Memory Monitoring](#host-cpu-and-memory-monitoring)
  - [Screen layout](#screen-layout)
  - [The CPU detail block](#the-cpu-detail-block)
  - [Platform support](#platform-support)
  - [Metric semantics](#metric-semantics)
  - [Configuration keys](#configuration-keys)
- [GPU Support](#gpu-support)
  - [AMD](#amd)
  - [Intel](#intel)
  - [NVIDIA](#nvidia)
  - [Adreno](#adreno)
  - [Apple](#apple)
  - [Ascend](#ascend) (only tested on 910B)
  - [Iluvatar CoreX](#iluvatar-corex)
  - [VideoCore](#videocore)
  - [Rockchip](#rockchip)
  - [MetaX](#metax)
  - [Enflame](#enflame)
  - [Tenstorrent](#tenstorrent)
- [Build](#build)
- [Installation and build](#installation-and-build)
- [Upstream backend synchronization](#upstream-backend-synchronization)
- [Troubleshoot](#troubleshoot)
- [License](#license)

hwdash Options and Interactive Commands
----------------------------------------
### Interactive Setup Window

hwdash has a builtin setup utility that provides a way to specialize the interface to your needs.
Simply press ``F2`` and select the options that are the best for you.

In the ``Chart`` section you can choose which metrics are plotted, including GPU and memory
utilization, temperature, power, clocks, and the **PCIe RX / TX load** (the receive and transmit
throughput as a percentage of the maximum link bandwidth).

In the ``General`` section you can enable or disable, independently, the **host CPU usage** and the
**host memory usage** lines of the combined CPU chart (see
[Host CPU and Memory Monitoring](#host-cpu-and-memory-monitoring)). Both are enabled by default on
Linux and disabled elsewhere. The layout is rebuilt as soon as you leave the setup window.

![hwdash Setup Window](/screenshot/Nvtop-config.png)

### Saving Preferences

You can save the preferences set in the setup window by pressing ``F12``.
The preferences will be loaded the next time you run ``hwdash``.

### hwdash Manual and Command line Options

hwdash comes with a manpage!
```bash
man hwdash
```
For quick command line arguments help
```bash
hwdash -h
hwdash --help
```

Host CPU and Memory Monitoring
------------------------------

On Linux, ``hwdash`` reports the whole host next to the GPU metrics. The whole host CPU
utilization and the whole host memory utilization share a single **CPU chart**, the way the GPU
utilization and the GPU memory share a GPU chart: one percentage history line per enabled metric,
drawn by the same plot renderer, with the same ``0/25/50/75/100`` scale, the same border and the
same time axis. The whole host CPU is treated as a device of its own, the **CPU device**, listed
before the GPU devices.

The legend of a chart is the key of a curve, not a readout of it: the entries of a chart are all
drawn on a single row, horizontally, in plot line order, and every entry carries a short
``---`` swatch so that it visibly belongs to a line. The text and the swatch of an entry use the
color of the plot line they stand for, so a GPU chart reads ``GPU0 % ---    GPU0 mem% ---`` and the
CPU chart ``CPU % ---    RAM % ---``. The current value of a metric is not part of its legend: the
detail block right above each chart already shows the values.

The current value of every line of a chart is instead read out in a **gutter** at the right edge of
the chart. The frame extends to the column immediately before the fixed-position value, leaving only
the separation the two need, while the section keeps the width the layout gave it. Values stay outside
the frame, curves and legend, each in the color of its line: ``0.4%``, ``92.6%``, ``100.0%``. A value
sits on the row its own line ends on, so the number is level with the end of its curve; when two values
would land on the same row, or too close for both to be read, they take separate adjacent rows, the
higher value above the lower one and, when two values are equal, the lower plot line above the higher
one - ``CPU %`` above ``RAM %``, ``GPU0 %`` above ``GPU0 mem%``. A readout always stays inside the data
region, below the legend and above the time axis, so neither a 0% nor a 100% ever escapes the chart,
and no value repeats the name of its metric, which is what the legend is for. A line whose newest
sample is not available shows no readout at all, never an invented 0%.

The oldest edge of a chart is kept clean: every curve begins against the Y axis as one connected
segment, and the oldest sample leaves together with its transition, so neither a detached short mark
nor a vertical remnant hangs at the edge. The same connected edge applies when the time axis is
reversed and the left edge is the newest one.

### Screen layout

The monitoring screen is a stack of sections, from the top of the terminal to the bottom:

- the **CPU device**: its detail block, then its CPU chart;
- the **GPU devices**, in their index order: **one section per device**, its own detail block
  directly above its own chart;
- the **process list**;
- the unused space, if the charts and the process list do not fill the terminal;
- the keyboard shortcut line, always the last row of the terminal.

One blank row separates two sections. It never separates a device detail block from its own chart,
so a GPU header only ever introduces the chart right below it.

The first row of a detail block is the title row of its device - ``Device CPU [...]``, ``Device 0
[...]`` - and it stays at the column of the section. Every row below it is indented to the column the
vertical Y axis of the chart of that same device is drawn at, so the clocks, the temperatures, the
powers and the GPU / GPU memory bars all start lined up with the axis of the chart right below them.
A narrow terminal truncates those rows; it never wraps them into the section that follows.

- The CPU chart is an ordinary chart: it gets the same outer dimensions, the same row height, the
  same full chart row width, and the same resize and narrow terminal behaviour as the GPU charts. No
  chart ever shares a row with another one and none takes a column away from another: every visible
  device owns a full width chart of its own, and all of the visible charts share the available
  vertical space equally, within the existing minimum chart height and process-list behaviour.
- Enabling one metric draws one line in that chart, enabling both draws two lines, and disabling
  both removes the CPU device entirely, its detail block and its blank row with it: no space is
  reserved for it.
- On a small terminal whole device sections are left out, in a fixed order: the **CPU device
  first**, then the **GPU device sections from the highest index to the lowest**, until every
  visible chart has at least the minimum chart height. A GPU section that is left out has neither a
  detail block nor a chart, so the visible GPUs stay an ordered prefix of the monitored ones and no
  header is ever left without the chart below it. Everything comes back as soon as the terminal is
  tall enough again. Nothing overlaps, nothing leaves the terminal, and everything is re-laid out
  whenever the terminal is resized or an option changes. The hidden GPUs are still monitored, they
  still keep their history and they are still listed in the setup screen.
- The history curves are kept when the window is resized: only the drawing surface is recomputed.

### The CPU detail block

Above its chart, the CPU device shows a three row detail block, as dense as a GPU one:

```
Device CPU [<model>]  CORES <physical>C/<logical>T
CPU  <util>%   FREQ <average GHz>   LOAD <1m> / <5m> / <15m>   POWER <package watts>W   Lenovo CPU Fan <rpm> RPM
RAM  <used>/<total> GiB  <percent>%   AVAIL <available GiB>   SWAP <used>/<total> GiB
```

The CPU utilization and the memory usage are exactly the values drawn by the CPU chart. The CPU
model and the core counts come from ``/proc/cpuinfo`` (with ``/sys/devices/system/cpu/`` for the
logical threads), the average frequency from the ``cpuinfo_cur_freq`` / ``scaling_cur_freq`` sysfs
entries, the load averages from ``/proc/loadavg``, and the swap from ``/proc/meminfo``; everything
refreshes with the normal update interval. Missing or malformed fields show ``N/A``. When the
terminal is too narrow, the block shortens in that order: swap, then the 15m and 5m load averages,
then the available memory, then the fan speed, package power, frequency and the rest of the static
CPU information; the CPU utilization and the memory used/total/percentage stay as long as the block
itself fits, and an overlong CPU model is truncated rather than overflowing the terminal. The field
titles of the block - ``Device CPU``, ``CORES``, ``CPU``, ``FREQ``, ``LOAD``, ``POWER``, ``Lenovo CPU Fan``,
``RAM``, ``AVAIL``, ``SWAP`` - are highlighted with the same color as the titles of a GPU detail block, and
the values keep the normal terminal color; a title a narrow terminal dropped is not highlighted.
CPU temperature is not part of this block, and the power is the one of the whole CPU package: one
power for now, neither a per core breakdown nor a curve of the chart.

### Platform support

Whole host metrics are collected on **Linux only**, from the ``/proc`` filesystem:

- CPU utilization from the aggregate ``cpu`` line of ``/proc/stat``;
- memory usage from ``MemTotal`` and ``MemAvailable`` in ``/proc/meminfo``.

The package power of the CPU detail block does not come from ``/proc`` but from the standard power
interfaces of the kernel, and it is sampled once per refresh like the rest:

- the powercap (RAPL) energy counter of the package, looked up by the ``name`` its zone declares
  (``package``, ``pkg``, ``soc``, the AMD ``amd`` domain, with or without a ``-<index>`` suffix) and
  not by a fixed ``intel-rapl:0`` path; the watts are the delta of ``energy_uj`` over the elapsed
  monotonic time, and the counter wrapping at ``max_energy_range_uj`` is the normal way of counting,
  not an error;
- where no such zone exists, an hwmon ``power*_input`` sensor - but only one whose ``power*_label``
  says it is the package.

No raw MSR access and no external tool is used, so whatever the kernel does not expose to this
process is simply not reported: a first sample, a counter that reset without a wrap to explain it, a
wrap adding up to a power no package draws, a zone that disappears, a malformed file and a platform
with no such interface all show ``POWER N/A``, never a fabricated ``POWER 0.0W``. On a machine whose
kernel exposes no readable energy counter to a normal user, ``POWER N/A`` is therefore the expected
display.

On a machine whose DMI system vendor is Lenovo, the block also shows ``Lenovo CPU Fan``. HWDash
installs the generic ``hwdash-helper.service`` for privileged hardware collectors. Its first
provider reads the Lenovo EC register pair with a high-low-high consistency check and publishes
only the checked RPM value to ``/run/hwdash/lenovo-cpu-fan-rpm``. The HWDash interface remains
unprivileged. The provider refuses the Lenovo offsets on other computers, never enables EC writes,
and does not invent a fan percentage because this source contains RPM only. Non-Lenovo machines do
not show this field.

If the firmware exposes no active ACPI EC device to ``ec_sys``, the helper falls back to
``/dev/port`` and performs the same ``RD_EC`` command/address handshake as P3FanMonitor. The port
writes are part of selecting a register for reading; there is no ``WR_EC`` command and no EC RAM
write path in the helper.

Enable it once after installing hwdash:

```console
sudo systemctl daemon-reload
sudo systemctl enable --now hwdash-helper.service
systemctl status hwdash-helper.service
cat /run/hwdash/lenovo-cpu-fan-rpm
```

If the service stops or the EC sample is incoherent, its output disappears; hwdash also rejects a
stale output file and displays ``Lenovo CPU Fan N/A``.

Other operating systems keep building; they simply have no host metrics, and the lines default to
disabled there. If they are enabled by hand on such a platform, the detail block shows ``N/A``
instead of a fake value, and the chart draws no curve. When a sample cannot be read or is not usable
(missing or malformed ``/proc`` data, ``MemAvailable`` larger than ``MemTotal``, counter reset, no
elapsed tick between two refreshes), the detail block displays ``N/A`` and the curve leaves a hole:
unavailable data is never displayed as an idle 0% load and never plotted as a zero.

The metrics are sampled once per interface refresh, like the GPU metrics: no extra polling and no
busy loop are introduced, and ``-s``/``--sort-by`` style command line paths are unaffected.

### Metric semantics

- **CPU**: the difference between two successive samples of the aggregate tick counters.
  ``idle + iowait`` is counted as idle time, everything else as busy time. ``guest`` and
  ``guest_nice`` are *not* added to the total since the kernel already accounts them inside ``user``
  and ``nice``; adding them would double count virtualization time. The first sample only builds a
  reference, so a rate is displayed from the second refresh on.
- **Memory**: ``used = MemTotal - MemAvailable``, the definition used by ``free`` and by the kernel
  itself, so the reclaimable page cache and the reclaimable slab are *not* counted as used memory.
  It is deliberately not a sum of the per-process memory: shared libraries, page tables and kernel
  allocations would be missed or counted several times. The detail block shows used and total in
  GiB (powers of 1024), the usage as a percentage of ``MemTotal`` and the available memory; the
  legend of the line is its key, ``RAM %``, exactly like the legend of every other chart line.

### Configuration keys

The preferences saved with ``F12`` (``$XDG_CONFIG_HOME/hwdash/interface.ini``) store the two options
in the ``[GeneralOption]`` section:

```ini
[GeneralOption]
ShowHostCpuUsage = true
ShowHostMemUsage = true
```

Configuration files written by older versions of this fork do not contain these keys; they keep
working and fall back to the platform defaults above.

GPU Support
-----------

### AMD

hwdash inherits nvtop support for AMD GPUs using the `amdgpu` driver and the legacy `radeon` driver (legacy GPUs, limited support) through the exposed DRM and
sysfs interface.

The radeon provides limited metrics compared to amdgpu.

AMD introduced the fdinfo interface in kernel 5.14 ([browse kernel
source](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/drivers/gpu/drm/amd/amdgpu/amdgpu_fdinfo.c?h=linux-5.14.y)).
Hence, you will need a kernel with a version greater or equal to 5.14 to see the
processes using AMD GPUs.

Support for recent GPUs are regularly mainlined into the linux kernel, so please
use a recent-enough kernel for your GPU.

### Intel

hwdash inherits nvtop support for Intel GPUs using the `i915` or `xe` linux driver.

Intel introduced the fdinfo interface in kernel 5.19 ([browse kernel
source](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/drivers/gpu/drm/i915/i915_drm_client.c?h=linux-5.19.y)).
Hence, you will need a kernel with a version greater or equal to 5.19 to see the
processes using Intel GPUs.

Intel requires CAP_PERFMON or CAP_SYS_ADMIN capabilities to access the total memory usage, and an accurate GPU frequency.
you can run `sudo setcap cap_perfmon=ep $(which hwdash)` to grant the necessary permissions or run hwdash as root.

### NVIDIA

The *NVML library* does not support some of the queries for GPUs coming before the
Kepler microarchitecture. Anything starting at GeForce 600, GeForce 800M and
successor should work fine. For more information about supported GPUs please
take a look at the [NVML documentation](http://docs.nvidia.com/deploy/nvml-api/nvml-api-reference.html#nvml-api-reference).

### Adreno

hwdash inherits nvtop support for Adreno GPUs using the `msm` linux driver.

msm introduced the fdinfo interface in kernel 6.0 ([browse kernel
source](https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git/tree/drivers/gpu/drm/msm/msm_drv.c?h=linux-6.0.y)).
Hence, you will need a kernel with a version greater or equal to 6.0 to see the
processes using Adreno GPUs.

### Apple

hwdash inherits nvtop's initial support for Apple using Metal. This is only supported when building for Apple, and when building for Apple only this vendor is supported.

**APPLE SUPPORT STATUS**
- Apple support is still being worked on. Some bugs and limitations may apply.

### Ascend

hwdash inherits nvtop support for Ascend (testing on Altas 800 (910B)) by DCMI API (version 6.0.0).

Currently, the DCMI only supports limited APIs, missing PCIe generation, tx/rx throughput info, max power draw etc.

### Iluvatar CoreX

hwdash inherits nvtop support for Iluvatar CoreX GPUs through the ixML library.

The backend dynamically loads `libixml.so` from `/usr/local/corex/lib`,
`/usr/local/corex/lib64`, or the default dynamic loader search path. The ixML
runtime exposes an NVML-compatible API surface used by hwdash to query device,
power, PCIe, clock, temperature, memory, and process information.

### VideoCore

hwdash inherits nvtop support for VideoCore (testing on raspberrypi 4B).

Supports GPU frequency, temperature, utilization, per-process utilization, GPU memory usage, and H264 decoding utilization.

On non-raspberry pi os, you need to use the `linux-rpi 6.12.y` kernel and above, and ensure the presence of the `/dev/vcio` device.

### Rockchip

hwdash inherits nvtop support for Rockchip (testing on orangepi 5 plus).

Supports NPU frequency, temperature, utilization.

### MetaX

hwdash inherits nvtop support for MetaX (testing on MXC500) by MXSML LIBRARY.

For more information about GPUs please take a look at the [METAX documentation](https://developer.metax-tech.com/doc/index)

### Enflame

hwdash inherits nvtop support for Enflame GCUs (testing on Enflame S60, Enflame L300 and Enflame L600) by EFML LIBRARY

GCU, which refers to General Compute Unit, is a type of accelerator card that is used to perform general-purpose computing tasks just like GPGPU.

### Tenstorrent

hwdash inherits nvtop support for Tenstorrent AI accelerators (Blackhole, Wormhole, Grayskull) through the [tt-kmd](https://github.com/tenstorrent/tt-kmd) kernel driver.

Supports temperature, power draw, AI clock, fan RPM, PCIe link info, and process listing. No external libraries required -- all data is read from sysfs, hwmon, and procfs.

Build
-----

Several libraries are required for hwdash to use the inherited GPU backends:

* The *ncurses* library driving the user interface.
  * This makes the screen look beautiful.
* For NVIDIA: the *NVIDIA Management Library* (*NVML*) which comes with the GPU driver.
  * This queries the GPU for info.
* For AMD: the libdrm library used to query AMD GPUs through the kernel driver.
* For METAX: the *MetaX System Management Library* (*MXSML*) which comes with the GPU driver.
  * This queries the GPU for info.
* For Enflame: the *Enflame Management Library* (*EFML*) which comes with the GCU driver.
* For Iluvatar CoreX: the *ixML* runtime library (`libixml.so`) which comes with the driver.
  * This backend loads the library dynamically at runtime.

Installation and build
----------------------

The distribution packages named ``nvtop`` install the upstream project, not hwdash.
Until hwdash release packages are published, build this repository directly.

### Ubuntu / Debian

```bash
sudo apt install cmake libncurses-dev libdrm-dev libsystemd-dev libudev-dev git gcc g++
git clone https://github.com/etan68/hwdash.git
cd hwdash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DNVIDIA_SUPPORT=ON -DAMDGPU_SUPPORT=ON -DINTEL_SUPPORT=ON
cmake --build build -j
sudo cmake --install build
```

The installed user command is ``hwdash``. Linux installations also provide the
generic ``hwdash-helper.service``. It runs privileged provider modules separately
from the interface; currently the only provider is Lenovo CPU fan RPM.

For an installation under a user-selected prefix, add
``-DCMAKE_INSTALL_PREFIX=/path/to/prefix`` at configure time and omit ``sudo``
when that prefix is writable.

The build system supports ``Release``, ``RelWithDebInfo`` and ``Debug`` build
types. Debug builds enable the repository's configured diagnostics and
sanitizers.

Upstream backend synchronization
--------------------------------

hwdash intentionally retains nvtop's internal ``nvtop`` C symbols, include
namespace and CMake target. Its public binary, package, configuration, desktop
entry, man page, helper and systemd unit use the ``hwdash`` name. Keeping the
internal namespace stable avoids a large permanent rename diff in the files
where upstream develops hardware support.

The repository should use these remotes:

```text
origin    https://github.com/etan68/hwdash.git
upstream  https://github.com/Syllo/nvtop.git
```

For each upstream update, create a temporary branch from hwdash ``main``, merge
``upstream/master``, resolve and test it there, and merge the reviewed result
back into ``main``. See ``UPSTREAM.md`` for the code ownership boundary and the
repeatable synchronization procedure.

Troubleshoot
------------

- The plot looks bad:
  - Verify that you installed the wide character version of the ncurses library (libncurses**w**5-dev for Debian / Ubuntu), clean the build directory and restart the build process.
- If `hwdash` exits with `ncurses: cannot initialize terminal type ($TERM="unknown")`, ensure that `$TERM` is set to a valid terminal type such as `xterm-256color`.
- **Putty**: Tell putty not to lie about its capabilities (`$TERM`) by setting the field ``Terminal-type string`` to ``putty`` in the menu
  ``Connection > Data > Terminal Details``.
- `NO GPU to monitor.` for NVIDIA GPUs:
  - `hwdash` loads a shared library named `libnvml.so` (shipped with the NVIDIA
  drivers) to query device information. If the library is not present, hwdash
  will not be able to monitor your NVIDIA device.
  - On `WSL2`, the driver is exposed by Windows to the virtual machine. Follow
  NVIDIA's [CUDA on WSL guide](https://docs.nvidia.com/cuda/wsl-user-guide/index.html)
  rather than installing another Linux display driver inside WSL2; a mismatched
  driver can make the GPU unavailable to hwdash.

License
-------

hwdash is licensed under GPL version 3 or, at your option, any later version.
It is a modified work based on nvtop. Existing copyright and license notices
remain attached to inherited files, and the complete license is in ``COPYING``.
