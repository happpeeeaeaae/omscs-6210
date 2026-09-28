# Memory coordinator

Build with `make` in this directory, then run `./memory_coordinator 2` to
schedule every two seconds. The program connects to `qemu:///system` once.

## Program flow

Each scheduler pass follows these functions:

1. `fetch_vm_states` lists active VMs and reads their current balloon size,
   unused memory, configured limit, and statistics timestamp. A small
   `VmMap` maps each libvirt domain ID to a `VmRecord`; its UUID check
   prevents a reused ID from inheriting an old VM's state. The
   `VmMemoryState` substructure holds the measurements and pending request.
2. `plan_target_memory` receives the complete snapshot and host free memory.
   It calculates ordered target sizes without calling libvirt or changing the
   input. Its result is a `MemoryPlan`.
3. `apply_memory_targets` sends each target through `virDomainSetMemory`.
   Accepted requests remain pending until a newer sample reports that target.
4. `save_vm_history` retains the state needed for the next pass.
   `cleanup_memory_plan`, `cleanup_vm_map`, and
   `cleanup_memory_scheduler` release allocated memory and libvirt handles.

`initialize_memory_scheduler` sets the initial state, while
`open_hypervisor_connection` contains the one-time libvirt connection.

## Greedy policy

The planner first chooses donors in descending order of unused memory. A VM
with more than 300 MiB unused gives back at most 100 MiB, leaving at least
100 MiB unused. It then chooses recipients in ascending order of unused
memory. A VM with less than 200 MiB unused can gain at most 100 MiB, bounded
by its configured maximum, the 2048 MiB project cap, and the shared host
budget. The budget is measured host free memory minus the 200 MiB host
reserve and any outstanding grants. Grants are rounded down to 4 KiB.
Reclaimed memory enters the budget only when the host reports it free on a
later pass.

The 200/300 MiB gap avoids switching a VM back and forth when its unused
memory fluctuates near a threshold. Actual and unused memory must both be
valid. When `LAST_UPDATE` is available, it must advance before another
decision; when libvirt omits it, valid balloon readings are still used.
A failed request on that fallback path is retried no more than once every
three passes. A still-pending request blocks another request to the same
VM, and a target equal to the observed allocation is never sent.

Set `MEMORY_DEBUG=1` to see sampled sizes, host grant budget, and reasons
that a VM cannot grow. Statistics are read even if enabling their collection
period fails, since another process may already have enabled collection.
Libvirt call failures are always written to stderr with the call name, VM UUID
when known, and libvirt's error message. Empty memory statistics and a zero
host-free-memory result are also reported.

This is a deliberately small greedy adaptation of the ideas in Carl
Waldspurger's [*Memory Resource Management in VMware ESX Server*](https://usenix.org/legacy/events/osdi02/tech/waldspurger/waldspurger_html/esx-mem-html.html)
(OSDI 2002). Its [idle-memory reclamation section](https://usenix.org/legacy/events/osdi02/tech/waldspurger/waldspurger_html/node14.html)
describes preferentially reclaiming memory from idle VMs; its
[dynamic reallocation section](https://usenix.org/legacy/events/osdi02/tech/waldspurger/waldspurger_html/node20.html)
describes periodic rebalancing and hysteresis. For this assignment, the
policy replaces ESX's shares, idle-memory tax, and multiple host-pressure
states with unused-memory ordering, fixed thresholds, one-step changes,
and assignment-specific safety limits. The
assignment's [memory requirements](../../README.md#key-considerations)
set the 100 MiB guest reserve, 200 MiB host reserve, and gradual changes.

Libvirt's [domain API](https://libvirt.org/html/libvirt-libvirt-domain.html)
defines the balloon statistics and target-memory calls. Its
[host API](https://libvirt.org/html/libvirt-libvirt-host.html#virNodeGetFreeMemory)
reports free host memory in bytes, so the coordinator converts it to KiB.

## Verification

`make -C ../../tests .build/memory_test && ../../tests/.build/memory_test`
runs the mock scheduler scenarios. Live use also needs libvirt development
headers, a running hypervisor, and guests with working balloon statistics.
