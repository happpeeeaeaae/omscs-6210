# Memory Coordinator

This is a simple threshold-based coordinator. Build with `make` and run
`./memory_coordinator 2` to sample every two seconds. The interval must be positive.

## Policy

All memory calculations use KiB internally; the constants below are in MiB
(1 MiB = 1024 KiB).

| Setting | Value |
| --- | ---: |
| Maximum change per guest per call | 100 MiB |
| Give memory when unused memory is below | 200 MiB |
| Reclaim memory when unused memory is above | 300 MiB |
| Minimum unused memory allowed after a requested reclaim | 100 MiB |
| Host free memory reserved from allocation | 200 MiB |
| Guest allocation ceiling | Smaller of its configured maximum and 2048 MiB |

## Algorithm

1. List active domains and enable balloon statistics at the supplied interval.
   Read actual balloon allocation, unused guest memory, and the last-update
   timestamp. Missing, invalid, or unchanged samples do not trigger adjustments.
2. For guests above the reclamation threshold, request a reduction of up to
   100 MiB. Clamp it to preserve the guest's required unused-memory reserve.
3. Read host free memory and subtract the 200 MiB reserve. Host free memory is
   returned in bytes, so divide it by 1024 before comparing with guest statistics.
   A failed host query grants no memory. Requested reclamation is not added to
   this budget: it becomes spendable only when observed as host free memory.
4. Visit guests in increasing order of unused memory, breaking ties by UUID.
   Give a guest below 200 MiB unused up to 100 MiB, capped by its allocation
   ceiling and the remaining budget. Deduct only successful grants from the
   shared budget. Round grants down to a 4 KiB page.
5. Remember the requested target until a fresh sample reports that allocation.
   Do not issue another adjustment while a previous one is pending, and reserve
   any outstanding growth from the host budget on subsequent calls.

The gap between the two thresholds prevents an adjustment from immediately
triggering the opposite action. For example, a guest with 190 MiB unused receives
100 MiB; with no new workload it then has 290 MiB unused and stays unchanged.
This conservative policy generally leaves more than the required 100 MiB spare.

History is keyed by domain UUID and running domain ID and refreshed every call.
Departed domains are removed, and restarted guests begin with fresh state.
Unusable guest statistics are skipped; failed balloon requests are reported.
If a guest cannot be identified, the entire pass is skipped and previous history
is preserved so an outstanding grant cannot be forgotten.
All active domains on the project host are managed, regardless of their names.

## Limitations and testing

This baseline has no demand prediction or fairness guarantee under sustained
host pressure. It may initially grow an idle guest whose unused memory is below
the growth threshold. Its reserve checks use sampled data: a workload can consume
memory between calls, and a guest already at its maximum cannot always be helped.
Use a short interval such as one or two seconds for the supplied workloads.

A stalled balloon request is left pending rather than repeatedly issuing new
requests. Restart the coordinator if an external tool changes a pending target.
The sample assumes the project's x86 KVM guests with 4 KiB pages and a working
balloon driver. VM creation, balloon support, and maximum-memory configuration
remain part of the supplied environment setup.

Follow `../test/HowToDoTest.md` for the three KVM scenarios: one hungry guest, all
guests hungry, and two guests whose workloads finish at different times. Also
check insufficient host memory, guests at their maximum, stale statistics, and
failed or delayed balloon requests.

For tests that need no VMs or libvirt installation, run `make -C ../../tests check`
from this directory. The [mock test guide](../../tests/README.md) describes the
simulations and their limitations.

## References

- [Project requirements](../../README.md)
- [libvirt memory statistics](https://libvirt.org/html/libvirt-libvirt-domain.html#virDomainMemoryStats)
- [libvirt balloon targets](https://libvirt.org/html/libvirt-libvirt-domain.html#virDomainSetMemory)
- [libvirt host free memory and units](https://libvirt.org/html/libvirt-libvirt-host.html#virNodeGetFreeMemory)
