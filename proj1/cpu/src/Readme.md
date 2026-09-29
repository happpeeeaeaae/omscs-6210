# vCPU scheduler

Build with `make` in this directory and run with a positive interval, for
example `./vcpu_scheduler 2`. `main` opens `qemu:///system`, calls
`CPUScheduler()` each interval, and closes the connection on Ctrl-C. A live
build needs libvirt development headers and libraries.

## One interval: measurement, decision, action

1. `read_cpu_snapshot` obtains online pCPU IDs with `virNodeGetCPUMap`, lists
   active VMs, and reads each vCPU's cumulative CPU time and affinity with
   `virDomainGetVcpus`. A single online pCPU in the affinity mask is a valid
   pin; a broad or invalid mask is marked for repair.
2. `plan_cpu_pins` matches VMs by ID **and UUID** and vCPUs by number against
   the prior snapshot. `collect_cpu_work` computes utilization as
   `100 × (CPU-time delta in ns) / (monotonic elapsed time in ns)`. It sums the
   percentages assigned to each online pCPU to estimate its load.
3. `load_deviation` computes the population standard deviation of pCPU loads.
   At **≤5 percentage points**, valid pins stay put. Above 5, `assign_greedily`
   places vCPUs in descending utilization order on the least loaded online
   pCPU. Ties prefer the current pCPU, then fewer assigned vCPUs, then the
   lowest pCPU ID. The new mapping is accepted only if its predicted deviation
   improves by **at least 1 point**.
4. `record_plan` includes only changed pins or invalid affinity masks.
   `apply_pin_changes` calls `virDomainPinVcpu` with a full host-sized mask
   containing just the chosen pCPU. `save_baseline` keeps the sampled state
   for the next interval; a failed pin is read again rather than presumed
   successful.

For example, four equally busy vCPUs concentrated on one of four pCPUs are
spread across the four pCPUs. Once their measured loads are balanced, the
scheduler stops moving them. This avoids migration overhead from small
measurement fluctuations.

## Robustness and evidence

- The first sample, a new or restarted VM, a reset CPU-time counter, or
  nonpositive elapsed time establishes a baseline without rebalancing. An
  incomplete libvirt snapshot does not replace the last complete baseline.
  Empty VM sets clear it. Counts and CPU IDs come from libvirt, so the policy
  works with more or fewer vCPUs than pCPUs, multiple vCPUs per VM, and sparse
  online pCPU IDs.
- A saturated pCPU can make measured utilization understate demand, so a
  greedy placement may need multiple intervals and cannot guarantee a
  5-point deviation for every workload. The scheduler avoids unnecessary
  repinning but does not model NUMA or host-process load.
- `make -C tests check` and `make -C tests sanitize` run the scheduler against
  mocked libvirt calls. They cover balancing, stability after balance, changing
  demand, unusual CPU layouts, restarts, incomplete samples, and failed pins.
  The recorded [test 2](../test/vcpu_scheduler2.log) shows pCPU readings
  `102, 0, 0, 0` at iteration 8 and `25, 25, 25, 25` at iteration 9;
  [test 3](../test/vcpu_scheduler3.log) shows balance after mixed demand.
  These are earlier VM runs; the mocks check the submitted decision logic.
  See the [live test procedure](../test/HowToDoTest.md) to repeat the runs.
