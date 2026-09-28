# vCPU Scheduler

Build with `make` and run `./vcpu_scheduler 2` to sample every two seconds. The
interval must be positive.

## Established algorithm: Longest Processing Time first

[Longest Processing Time first (LPT)](https://arxiv.org/abs/1801.05489) is a
greedy scheduling rule for jobs on identical processors: sort jobs from largest
to smallest, then assign each job to the processor with the lowest load so far.
It originates with [Graham's multiprocessor scheduling work](https://fanchung.ucsd.edu/ron/papers/69_02_multiprocessing.pdf).
Here, each vCPU is a job, each online pCPU is a processor, and recent vCPU
utilization is the estimated job size.

## Modifications for periodic vCPU pinning

Classical LPT builds a schedule from known, fixed job sizes. This scheduler
repeats the placement decision as VM demand changes. Dynamic rebalancing also
has a relocation cost, as described in [The Load Rebalancing Problem](https://research.google/pubs/the-load-rebalancing-problem/).
The following rules adapt LPT to the [assignment's balance and stability goals](../../README.md):

1. **Estimate current load.** List active domains and online pCPUs. Read every
   vCPU's cumulative CPU time and affinity mask. Match it to the previous sample
   by domain UUID, running domain ID, and vCPU number. Compute utilization as
   `100 * delta_cpu_nanoseconds / elapsed_nanoseconds`, using monotonic elapsed
   time. Sum vCPU utilization on each pCPU. For a vCPU without a single-pCPU
   affinity, use its last reported pCPU as the initial placement estimate.
2. **Keep a balanced placement.** If the standard deviation of online pCPU loads
   is at most 5 percentage points, keep existing pins. If a vCPU lacks a single
   online-pCPU affinity, pin it to its reported current pCPU without moving
   already pinned vCPUs. This avoids migrations when the measured load is
   already balanced.
3. **Run LPT when loads are unbalanced.** Sort vCPUs by decreasing utilization
   and assign each to the online pCPU with the lowest planned load. When planned
   loads tie, prefer the vCPU's current pCPU, then the lowest pCPU number.
   Equal-usage vCPUs are ordered by UUID and vCPU number for repeatability.
4. **Limit repinning.** Apply an unbalanced plan only when its predicted standard
   deviation improves by at least one percentage point, unless any vCPU needs
   an explicit pin. In that case, apply the plan regardless of improvement.
   Issue pin requests only where the chosen pCPU differs from the current
   affinity, or the current affinity is not a single online pCPU.
5. **Wait for valid samples.** The first call records a baseline. A new or
   restarted vCPU, or a decreasing CPU-time counter, requires another sample
   before any redistribution. Incomplete statistics abort that iteration's
   redistribution. Pin failures are reported and reconsidered using observed
   affinities on the next call.

The main loop controls the interval; the scheduler does not sleep internally.
CPU topology and domains are refreshed on every call. Departed domains are
removed from the saved samples, and all active domains are managed regardless
of name. CPU counts and affinity masks are allocated dynamically.

Each call has three stages. `read_cpu_snapshot` fetches the host topology,
domain and vCPU statistics, affinities, and sample time from libvirt.
`plan_cpu_actions` compares the snapshot with saved CPU-time counters and
returns the vCPU-to-pCPU pins selected by the rules above; it makes no libvirt
calls. `apply_cpu_actions` sends only those pins to libvirt. `CPUScheduler`
owns the per-call data, saves a complete snapshot for the next call, and
releases all domain handles.

## Scope and testing

LPT's results for fixed jobs and makespan do not guarantee the assignment's
standard-deviation target for changing vCPU workloads. Recent utilization is
only an estimate of future demand. This scheduler does not account for NUMA,
host processes, or cache migration costs beyond limiting pin changes.

The supplied tests cover balanced pins, all guests initially sharing one pCPU,
and mixed heavy/light workloads. Follow `../test/HowToDoTest.md` in a configured
KVM environment. Also check VM restarts, multiple vCPUs per guest, offline
pCPUs, and affinity masks spanning more than one byte.

For tests that need no VMs or libvirt installation, run
`make -C ../../tests check` from this directory. The
[mock test guide](../../tests/README.md) describes the simulations and their
limits.

## API references

- [libvirt domain APIs](https://libvirt.org/html/libvirt-libvirt-domain.html):
  `virConnectListAllDomains`, `virDomainGetInfo`, `virDomainGetVcpus`, and
  `virDomainPinVcpu`.
- [libvirt host API](https://libvirt.org/html/libvirt-libvirt-host.html#virNodeGetCPUMap):
  online pCPU discovery and affinity mask sizing.
