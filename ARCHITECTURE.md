# HWDash architecture

Hardware collection is separated from presentation and from privileged access.

## Collection layers

- `src/collectors/gpu/` contains the nvtop-derived GPU, NPU and accelerator
  backends, device discovery and accelerator process accounting.
- `src/collectors/host/` contains generic whole-host metrics such as CPU, RAM,
  load, frequency and package power.
- `src/collectors/vendor/` contains unprivileged adapters for machine-vendor
  features. These adapters detect support, validate samples and expose them to
  the host collector.

The interface consumes normalized collector state. It does not open hardware
devices and does not know which privileged mechanism produced a sample.

## Privileged helper

`src/helper/hardware_helper.c` is a generic, separately installed helper. It
owns polling, process lifecycle and atomic publication. A privileged hardware
implementation registers a provider through `src/helper/helper_provider.h` and
lives under `src/helper/providers/`.

The Lenovo EC provider is the first implementation. It publishes one sanitized
RPM value; `src/collectors/vendor/lenovo_cpu_fan.c` consumes that value without
privilege. A future vendor integration can use the same helper provider boundary,
or remain entirely unprivileged when ordinary kernel interfaces are sufficient.

Provider outputs are deliberately narrow data files under `/run/hwdash`. They
are treated as short-lived samples: the consumer rejects malformed or stale
values, and the helper removes output after an incoherent read or clean exit.

## Upstream GPU updates

The GPU collector files keep their nvtop names and `include/nvtop` data contract.
When nvtop adds a backend, merge the upstream history, move the new collector
source into `src/collectors/gpu/`, update `src/CMakeLists.txt`, and run the full
build and test suite. See `UPSTREAM.md` for the synchronization workflow.
