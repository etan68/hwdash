# Upstream maintenance

hwdash is an independent GPL-3.0-or-later project based on
[nvtop](https://github.com/Syllo/nvtop). It keeps nvtop's Git history and
copyright notices so backend changes remain attributable and mergeable.

## Boundary

The nvtop-derived accelerator collection layer remains the upstream-facing
part of the tree:

- `src/collectors/gpu/extract_gpuinfo*.c`, `extract_gcuinfo*.c` and
  `extract_npuinfo*.c` implement vendor backends.
- `include/nvtop/extract_gpuinfo_common.h` is their common data contract.
- `src/collectors/gpu/extract_gpuinfo.c` registers and refreshes those backends.
- `src/collectors/gpu/device_discovery_linux.c` contains shared Linux device discovery.

Each compiled vendor source registers a `struct gpu_vendor` constructor with
the common collector. A newly merged backend therefore enters hwdash through
the same device list without a hwdash-specific vendor fork. New source files
still have to be included by the merged CMake configuration, and a new common
metric may require a small interface update before hwdash can display it.

hwdash-specific code should stay outside vendor backends whenever the metric
is not supplied by that accelerator:

- `src/collectors/host/host_metrics.c` and `include/nvtop/host_metrics.h` collect
  CPU, RAM, load, frequency and package power through ordinary kernel APIs.
- `src/collectors/vendor/` contains unprivileged machine-vendor adapters. The
  Lenovo adapter validates and consumes a value; it never accesses the EC.
- `src/helper/hardware_helper.c` owns the privileged process lifecycle and
  atomic publication. Hardware-specific privileged access is implemented by
  providers under `src/helper/providers/`; Lenovo EC is the first provider.
- `src/interface*.c`, `src/plot*.c` and the layout helpers own hwdash's device
  sections and presentation.
- `src/collectors/gpu/pci_name_lookup.c` and `include/hwdash/pci_name_lookup.h`
  resolve a GPU display name from the udev property, then the PCI name database
  of `lspci` through the optional libpci dependency, then the sysfs identifiers.
  The Intel backend calls it from `gpuinfo_intel_populate_static_info` and keeps
  its own bracket normalization; `src/CMakeLists.txt` looks for libpci only
  inside the `INTEL_SUPPORT` block. Merging an upstream change to that Intel
  function should keep both calls.

Lenovo CPU Fan is a host CPU sensor. It must not be added to the NVIDIA, Intel,
AMD or other accelerator structs. The privileged provider publishes a narrow
value, the unprivileged Lenovo adapter validates it, and the host collector feeds
it to the CPU section. Future vendor-specific metrics follow the same split;
providers that need no extra privilege stay entirely under `collectors/vendor`.

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
