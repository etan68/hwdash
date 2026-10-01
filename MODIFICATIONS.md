# Modified work notice

hwdash is a modified work based on nvtop. The hwdash project began modifying
the upstream work in September 2026.

The principal modifications as of 2026-09-30 are:

- an independent `hwdash` product, executable, configuration and packaging identity;
- a host CPU and RAM device with details, history and chart rendering;
- host CPU identity, frequency, load, swap and package-power collection on Linux,
  and native macOS CPU utilization, memory, CPU identity, load and swap collection;
- a CPU Display setup page, with `ShowCpuModel`, `ShowCpuFreq`, `ShowCpuLoad`,
  `ShowCpuPower`, `ShowCpuFan`, `ShowRamAvailable` and `ShowSwap` keys in a
  `[HostOption]` section, choosing which optional fields of the CPU/RAM detail
  block are drawn;
- section-based terminal layout and chart legend, edge and live-readout changes;
- a Lenovo CPU fan RPM adapter consisting of an unprivileged vendor collector
  and a provider in the generic privileged hardware helper;
- an Intel GPU display name that falls back to the PCI name database and to the
  PCI identifiers when the local udev database does not know the card; and
- build, service, test and documentation changes needed by those features.

Inherited source files retain their original copyright and license notices. New
hwdash files and modifications are distributed under GPL-3.0-or-later. Git
history records the individual changes, authors and dates.
