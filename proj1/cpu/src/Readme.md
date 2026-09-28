# vCPU scheduler

The scheduler samples active guests every interval and pins their vCPUs to online
host pCPUs. Its goal is to balance measured CPU use while keeping an acceptable
mapping stable. Build in this directory with `make`, then run, for example,
`./vcpu_scheduler 2`. The supplied `main` opens `qemu:///system`, calls
`CPUScheduler()` repeatedly, and closes the connection after Ctrl-C. Building
for a live host requires libvirt development headers and libraries.

## One interval: where the decisions live

1. `read_cpu_snapshot` obtains the online pCPU IDs with `virNodeGetCPUMap` and
   lists active domains. For each vCPU, `read_vm_state` copies its cumulative
   CPU time and affinity mask from `virDomainGetVcpus`. A mask containing exactly
   one online pCPU is a valid pin. Otherwise, the scheduler uses the vCPU's
   current running pCPU (or the first online pCPU) as a temporary placement and
   marks its affinity for repair.
2. `plan_cpu_pins` compares this complete snapshot with the previous one. For
   each matching domain UUID and vCPU number, `collect_cpu_work` computes
   `usage % = 100 × (CPU time now − CPU time before) / elapsed nanoseconds`.
   CPU time and elapsed time are both in nanoseconds; elapsed time comes from a
   monotonic clock. Summing the vCPU percentages assigned to each pCPU gives
   the estimated current pCPU loads.
3. `plan_cpu_pins` measures balance with the population standard deviation of
   those loads. If it is at most **5 percentage points**, the scheduler keeps
   the current placement. Otherwise, `assign_greedily` sorts vCPUs by decreasing
   measured use and assigns each to the least loaded online pCPU. Equal vCPU
   uses are ordered by UUID and vCPU number. For equal planned pCPU loads, the
   current pCPU wins, then the pCPU with fewer assigned vCPUs, then the lowest
   pCPU ID. This is a periodic adaptation of
   [Graham's longest processing time first rule](https://fanchung.ucsd.edu/ron/papers/69_02_multiprocessing.pdf).
4. The proposed mapping is used only if its predicted standard deviation is at
   least **1 percentage point** lower than the current value. `record_plan`
   includes only pins that would change or whose existing affinity is not a
   single online pCPU. Thus an already balanced mapping is left alone, while
   an invalid or broad affinity mask can still be repaired.
5. `apply_pin_changes` calls `virDomainPinVcpu` for those changes. It builds a
   full host-sized affinity mask with just the target pCPU set. A failed call is
   reported; the next interval reads the actual affinity again instead of
   assuming that the request succeeded. `save_baseline` retains the current
   sample, and `release_cycle` frees temporary domain handles and arrays.

For example, if four equally busy vCPUs are all pinned to pCPU 0, their
estimated loads are concentrated on one pCPU. The greedy plan places one on
each of four online pCPUs. Once the measured loads are within the 5-point
balance threshold, later intervals preserve those pins.

## Edge cases and limits

- The first sample is a baseline, so it makes no balancing decision. A new VM,
  a restart that changes its identity or resets its CPU counter, or nonpositive
  elapsed time also starts a new baseline before redistributing. An incomplete
  libvirt snapshot is discarded without replacing the last complete baseline.
  No active VMs clears the baseline.
- The scheduler uses counts and CPU IDs returned by libvirt. It handles more
  vCPUs than pCPUs, fewer vCPUs than pCPUs, multiple vCPUs in one guest,
  sparse online pCPU IDs, and affinity masks longer than one byte. Domain ID
  lookup also checks UUID so a reused ID cannot inherit another guest's CPU
  history.
- A vCPU sharing a saturated pCPU may receive less CPU time than it wants.
  The measured use can therefore understate demand, and rebalancing may need
  several intervals. The greedy plan estimates balance; it does not guarantee
  a 5-point result for every workload. It does not model host tasks, NUMA, or
  migration costs beyond avoiding unnecessary pin changes.

## Evidence and how to reproduce it

From the project root, `make -C tests check` runs the real scheduler source
against mocked libvirt calls. `make -C tests sanitize` runs the same cases with
AddressSanitizer and UndefinedBehaviorSanitizer. Both commands passed locally.
The cases cover the first sample, balanced pins, concentrated and changing
workloads, failed pins, incomplete statistics, VM restarts, unusual pCPU
counts, and multi-byte masks. In the mock simulations, the placement becomes
balanced and then stops changing when demand is steady.

The recorded [CPU test 2 log](../test/vcpu_scheduler2.log) shows a concentrated
sample at iteration 8 (pCPU readings `102, 0, 0, 0`) and balanced readings at
iteration 9 (`25, 25, 25, 25`); at iteration 25 the readings are
`35, 35, 34, 34`. The [CPU test 3 log](../test/vcpu_scheduler3.log) shows a
mixed workload changing from `49, 86, 87, 127` at iteration 7 to
`41, 40, 40, 42` at iteration 8. These monitor readings illustrate the live
behavior; the deterministic mock checks establish the specific decision rules.
See [live test instructions](../test/HowToDoTest.md) for reproducing the VM
runs.
