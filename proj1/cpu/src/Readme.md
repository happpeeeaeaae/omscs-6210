# VCPU Scheduler

This is a simple largest-load-first greedy scheduler. Build with `make` and run
`./vcpu_scheduler 2` to sample every two seconds. The interval must be positive.

## Algorithm

1. List active domains, discover online host CPUs, and read every vCPU's cumulative
   CPU time and affinity mask. CPU counts and masks are allocated dynamically.
2. Match each vCPU with its previous sample using the domain UUID, running domain
   ID, and vCPU number. The first call only records a baseline. New or restarted
   vCPUs, including counters that decrease, also require a baseline before the
   scheduler changes any pins.
3. Compute utilization as `100 * delta_cpu_nanoseconds / elapsed_nanoseconds`,
   using elapsed monotonic time between calls. Sum the utilization on each pCPU.
   For an unpinned vCPU, use its last reported CPU as an initial estimate.
4. Keep the existing pins when the standard deviation across online pCPUs is at
   most 5 percentage points and each vCPU is already pinned to one online CPU.
5. Otherwise, sort vCPUs by decreasing utilization and put each on the CPU with the
   lowest planned load. Ties prefer its current CPU, then the lowest CPU number.
   Equal-usage vCPUs are ordered by UUID and vCPU number for repeatability.
6. Apply the plan only if the predicted standard deviation improves by at least
   one percentage point. Unpinned vCPUs bypass this check to establish explicit
   pins. Only issue pin requests where the current affinity differs.

No extra sleep occurs inside the scheduler. The existing main loop controls the
interval. Domain and CPU information is refreshed on every call; departed domains
are removed from the saved samples. Incomplete statistics abort that iteration's
redistribution, and pin failures are reported and reconsidered on the next call.

## Limitations and testing

This baseline uses recent utilization as an estimate of future work; it does not
predict demand or account for NUMA, host processes, or cache migration costs. A
greedy assignment is not guaranteed to reach a standard deviation of 5 for every
possible workload. Small improvements are deliberately ignored to reduce churn.
All active domains on this project host are managed, regardless of their names.

The supplied tests cover balanced pins, all guests initially sharing one CPU, and
mixed heavy/light workloads. Follow `../test/HowToDoTest.md` in a configured KVM
environment. Additional useful checks are VM restarts, multiple vCPUs per guest,
offline CPUs, and hosts whose affinity masks require more than one byte.

For tests that need no VMs or libvirt installation, run `make -C ../../tests check`
from this directory. The [mock test guide](../../tests/README.md) describes the
simulations and their limitations.

## References

- [Project requirements](../../README.md)
- [libvirt domain APIs](https://libvirt.org/html/libvirt-libvirt-domain.html):
  `virConnectListAllDomains`, `virDomainGetInfo`, `virDomainGetVcpus`, and
  `virDomainPinVcpu`.
- [libvirt host APIs](https://libvirt.org/html/libvirt-libvirt-host.html#virNodeGetCPUMap):
  online CPU discovery and affinity mask sizing.
