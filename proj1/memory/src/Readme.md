# Memory Coordinator

Build with `make` and run `./memory_coordinator 2` to sample every two seconds.
The interval must be positive.

## Established approach: threshold control and ballooning

A hypervisor can change a VM's available memory through a
[balloon driver](https://usenix.org/legacy/events/osdi02/tech/waldspurger/waldspurger_html/node6.html).
The [dynamic reallocation policy in *Memory Resource Management in VMware ESX Server*](https://www.usenix.org/legacy/publications/library/proceedings/osdi02/tech/waldspurger/waldspurger_html/node20.html)
reacts to memory-pressure thresholds and uses hysteresis so allocations do not
oscillate around a single boundary. Those are the established ideas behind this
coordinator. Its exact allocation rule is specific to this assignment: it uses
guest unused-memory statistics, fixed thresholds, and a greedy priority order,
not the paper's share-based working-set policy.

## Modifications for this assignment

The [project requirements](../../README.md#memory-coordinator) require gradual
changes and reserves for both guests and the host. This implementation uses the
following values; all calculations are in KiB (1 MiB = 1024 KiB).

| Rule | Value |
| --- | ---: |
| Maximum change per guest per call | 100 MiB |
| Grow when guest unused memory is below | 200 MiB |
| Reclaim when guest unused memory is above | 300 MiB |
| Minimum unused memory after a requested reclaim | 100 MiB |
| Host free memory reserved from grants | 200 MiB |
| Guest allocation ceiling | Smaller of configured maximum and 2048 MiB |

The 200/300 MiB gap is the hysteresis band: a guest that grows from 190 to
290 MiB unused will not immediately be shrunk. The 100 MiB step bounds each
adjustment; the host and guest reserves protect available memory.

## Scheduling cycle

1. **Read fresh state.** List active domains, enable balloon statistics at the
   supplied interval, and read actual allocation, unused memory, and the last
   update time. Skip a guest with missing, invalid, or unchanged statistics.
   Match history by domain UUID and running domain ID so restarted guests start
   with fresh state.
2. **Plan reclamation.** For each usable guest above 300 MiB unused, choose a
   reduction of at most 100 MiB while leaving at least 100 MiB unused.
3. **Compute a safe shared budget.** Use the host free memory fetched with the
   guest statistics, convert bytes to KiB, and subtract the 200 MiB host
   reserve and any outstanding growth requests. Planned reclamation becomes
   spendable only after the host reports it free. A failed host query leaves
   no grant budget.
4. **Plan growth for the hungriest guests first.** Sort guests by increasing
   unused memory, breaking ties by UUID. For each usable guest below 200 MiB
   unused, choose up to 100 MiB, subject to its allocation ceiling and the
   remaining shared budget. Round grants down to a 4 KiB page and reserve
   every planned grant from the budget.
5. **Wait for balloon completion.** Do not send another adjustment to a guest
   until a fresh sample reports its pending target. Reserve pending growth on
   later calls so the same host memory cannot be promised twice.

`read_memory_snapshot` fetches guest statistics and host free memory from
libvirt. `plan_memory_actions` uses those readings and saved history to return
ordered reclamation and growth targets without calling libvirt.
`apply_memory_actions` sends the planned targets and records a pending target
only when libvirt accepts a request. `MemoryScheduler` owns the per-call data
and releases all domain handles.

Targets are fixed for each cycle. If a grant fails, its budget is reconsidered
on the next fresh sample; it is not reassigned to another guest during the
same cycle. Failed guests can still retry on later fresh samples.

Immediately before each memory-change API call, an explicit guard skips a target
equal to the latest observed allocation or a guest with a pending request. Only
a successful request records a pending target. Completed changes do not prevent
reuse of an earlier target (for example, 512 → 612 → 512 → 612 MiB).

History is refreshed each call and departed domains are removed. If a guest
cannot be identified, the whole pass is skipped and the previous history is
kept so an outstanding grant is not forgotten. Failed balloon requests are
reported. All active domains are managed regardless of name.

## Optional diagnostics

Enable diagnostics with `MEMORY_DEBUG=1`; the interval argument is unchanged:

```sh
MEMORY_DEBUG=1 ./memory_coordinator 2 2>memory-debug.log
```

Diagnostics go to stderr and report observed statistics and update timestamps,
missing or invalid statistics, stale samples, pending targets versus observed
allocations, requested growth/reclamation and API results, and the host grant
budget after reserves and after planning grants. Memory values in the log are
in KiB. Without `MEMORY_DEBUG=1`, execution stays quiet except for existing
error messages.

If adjustments stop, check for a missing, zero, or unchanged `last_update`, or a
pending target that never matches the observed allocation on a fresh sample.
Diagnostics do not change the statistics polling or pending-request policy.

## Scope and testing

This fixed-step policy does not predict demand or guarantee fair allocation
under sustained host pressure. It may initially grow an idle guest whose
unused memory is below the growth threshold. Reserve checks use sampled data:
a workload can consume memory between calls, and a guest at its maximum cannot
always be helped. Use a short interval, such as one or two seconds, for the
supplied workloads.

A stalled balloon request remains pending rather than being issued again.
Restart the coordinator if an external tool changes a pending target. The
policy assumes the project's x86 KVM guests with 4 KiB pages and a working
balloon driver. VM creation, balloon support, and maximum-memory configuration
remain part of the supplied environment setup.

Follow `../test/HowToDoTest.md` for the three KVM scenarios: one hungry guest,
all guests hungry, and two guests whose workloads finish at different times.
Also check insufficient host memory, guests at their maximum, stale statistics,
and failed or delayed balloon requests.

For tests that need no VMs or libvirt installation, run
`make -C ../../tests check` from this directory. The
[mock test guide](../../tests/README.md) describes the simulations and their
limits.

## API references

- [libvirt memory statistics](https://libvirt.org/html/libvirt-libvirt-domain.html#virDomainMemoryStats)
- [libvirt balloon targets](https://libvirt.org/html/libvirt-libvirt-domain.html#virDomainSetMemory)
- [libvirt host free memory and units](https://libvirt.org/html/libvirt-libvirt-host.html#virNodeGetFreeMemory)
