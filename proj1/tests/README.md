# Scheduler tests with mocked libvirt

From the project root, run:

```sh
make -C tests check
make -C tests sanitize
```

These tests need only a C compiler and Make. They do not connect to a hypervisor,
install packages, start VMs, or sleep. Build outputs stay in the ignored `.build/`
directory. An assertion failure makes the command fail with a nonzero exit code.

`sanitize` also enables AddressSanitizer and UndefinedBehaviorSanitizer. Leak
scanning defaults to off because the development sandbox uses a ptrace wrapper;
outside that environment, enable it with `ASAN_OPTIONS=detect_leaks=1 make -C tests
sanitize`. The tests use GNU C11, which accepts the assignment's unchanged signal
handler declaration, unlike newer compilers' default C23 mode.

## What is mocked

Each test includes the real scheduler source with its `main` renamed. A minimal
header in `mock_libvirt/libvirt/libvirt.h` and the functions in `mock.h` replace
libvirt calls. `cpu_test.c` also replaces the monotonic clock with simulated time.
The production source and its Makefiles do not need a special test mode.

The mock supplies active domains, UUIDs, running IDs, vCPU counts, cumulative CPU
times, affinity masks, balloon statistics, guest memory limits, and host free
memory. Pin and balloon requests are recorded and checked. Domain handle releases
are counted, and selected API calls can return errors or incomplete statistics.
Focused tests also call the decision functions with constructed snapshots to
check their action lists without making libvirt calls.

## CPU checks

- First-sample warmup and preservation of already balanced pins.
- Equal workloads initially concentrated on one CPU, and mixed heavy/light loads.
- Stable pinning after balance is achieved and adaptation when demand changes.
- Multiple vCPUs per guest, sparse CPU IDs, and affinity masks spanning bytes.
- Fewer vCPUs than pCPUs, one pCPU, no domains, restarts, decreasing counters,
  reordered domain listings, departed domains, incomplete statistics, and failed pins.

The multi-round simulations make subsequent CPU-time increments depend on the
latest pinning. Each pCPU supplies at most 100% utilization; contention is modeled
by dividing capacity in proportion to each resident vCPU's demand. Tests require
balanced demand within 15 rounds and no further pin changes over the next 10.
Reported demand can exceed 100% even though measured utilization is capped.

## Memory checks

- Growth and gradual reclamation, including exact threshold boundaries.
- Guest and host reserves, a shared allocation budget, and partial grants.
- Configured guest limits and the 2048 MiB project ceiling.
- Missing/stale statistics, delayed balloon responses, and failed memory requests.
- Missing `LAST_UPDATE` with valid balloon readings, failed statistics-period
  setup, and rate-limited retries after failed requests without timestamps.
- No redundant requests for fresh unchanged statistics in the stable band,
  guests at either allocation ceiling, or host budgets smaller than one page.
- Pending growth and reclamation across repeated and partially completed
  observations, failed-request retries on fresh samples, and reuse of a previous
  target after completed grow–shrink–grow changes.
- Fixed grant targets: budget reserved for a failed grant is reconsidered on
  the next fresh sample, rather than reassigned later in the same cycle.
- Reservations for pending grants, restarts, and a failed UUID query followed by
  recovery without duplicating a grant.

The memory mock rejects every request equal to the guest's current allocation.

Three simulations run 100 rounds each: one consumer, four consumers, and two
consumers where one finishes early. Consumers add 40 MiB of used memory per round
and later release it. Balloon requests settle between samples, updating both
guest allocation and host free memory. Assertions check gradual adjustments,
conservation of memory, reserves, reaching the maximum, and eventual reclamation.
Output reports each guest's peak and final allocation in MiB.

Some `Could not pin`, `Could not give memory`, and `Could not reclaim memory`
messages are expected: those cases intentionally inject API failures and then
check recovery.

## Limits

These are deterministic tests of the actual C scheduling code against a simplified
model. They do not validate libvirt ABI compatibility, QEMU behavior, real guest
balloon drivers, kernel scheduling fairness, or the assignment's live VM tests.
Run the existing CPU and memory VM test instructions in the configured project
environment before relying on those behaviors.
