# Modified work notice

hwdash is a modified work based on nvtop. The hwdash project began modifying
the upstream work in September 2026.

The principal modifications as of 2026-09-27 are:

- an independent `hwdash` product, executable, configuration and packaging identity;
- a host CPU and RAM device with details, history and chart rendering;
- host CPU identity, frequency, load, swap and package-power collection;
- section-based terminal layout and chart legend, edge and live-readout changes;
- a Lenovo CPU fan RPM adapter consisting of an unprivileged vendor collector
  and a provider in the generic privileged hardware helper; and
- build, service, test and documentation changes needed by those features.

Inherited source files retain their original copyright and license notices. New
hwdash files and modifications are distributed under GPL-3.0-or-later. Git
history records the individual changes, authors and dates.
