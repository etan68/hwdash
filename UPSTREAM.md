# Upstream maintenance

hwdash is an independent GPL-3.0-or-later project based on
[nvtop](https://github.com/Syllo/nvtop). It keeps nvtop's Git history and
copyright notices so backend changes remain attributable and mergeable.

## Boundary

The nvtop-derived accelerator collection layer remains the upstream-facing
part of the tree:

- `src/extract_gpuinfo*.c`, `src/extract_gcuinfo*.c` and
  `src/extract_npuinfo*.c` implement vendor backends.
- `include/nvtop/extract_gpuinfo_common.h` is their common data contract.
- `src/extract_gpuinfo.c` registers and refreshes those backends.
- `src/device_discovery_linux.c` contains shared Linux device discovery.

Each compiled vendor source registers a `struct gpu_vendor` constructor with
the common collector. A newly merged backend therefore enters hwdash through
the same device list without a hwdash-specific vendor fork. New source files
still have to be included by the merged CMake configuration, and a new common
metric may require a small interface update before hwdash can display it.

hwdash-specific code should stay outside vendor backends whenever the metric
is not supplied by that accelerator:

- `src/host_metrics.c` and `include/nvtop/host_metrics.h` collect and normalize
  host CPU, RAM, package power and host sensors.
- `src/lenovo_cpu_fan_helper.c` is the privileged Lenovo EC adapter. It publishes
  one checked RPM value; the main program remains unprivileged.
- `src/interface*.c`, `src/plot*.c` and the layout helpers own hwdash's device
  sections and presentation.

Lenovo CPU Fan is a host CPU sensor. It must not be added to the NVIDIA, Intel,
AMD or other accelerator structs. Its adapter feeds the host metrics state,
which the CPU device section consumes. This preserves the upstream GPU data
contract and lets new upstream GPU backends arrive without knowing about the
Lenovo extension.

## Remotes

Use the hwdash repository as `origin` and nvtop as `upstream`:

```bash
git remote set-url origin https://github.com/etan68/hwdash.git
git remote add upstream https://github.com/Syllo/nvtop.git
git fetch --all --prune
```

If `upstream` already exists, update it with `git remote set-url upstream ...`.

## Synchronizing

Start a temporary integration branch from the current hwdash main branch:

```bash
git switch main
git pull --ff-only origin main
git switch -c sync/nvtop-<tag-or-date>
git fetch upstream
git merge --no-ff upstream/master
```

Resolve conflicts without renaming nvtop's internal C symbols, include directory
or CMake target. Build and test all enabled backends available in CI, then merge
the reviewed integration branch into `main`.

Record the upstream tag or commit in the merge message and release notes. Do not
copy backend files manually: merging preserves authorship, deletions, new files
and build-system changes that a file copy can miss.

## Expected conflict area

Most new hardware support should merge through the vendor backend files with no
hwdash changes. The recurring review points are the target source list in
`src/CMakeLists.txt`, initialization and refresh calls in `src/nvtop.c`, common
metric struct changes, and interface changes caused by a new metric or processing
unit. The installed product name remains `hwdash`; the internal upstream names
remain intentionally unchanged.
