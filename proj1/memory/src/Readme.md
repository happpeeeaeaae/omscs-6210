# Memory coordinator

The coordinator adjusts active guests' balloon targets once per interval to
serve guests with low unused memory and reclaim from guests with ample unused
memory. Build in this directory with `make`, then run, for example,
`./memory_coordinator 2`. `main` opens one `qemu:///system` connection, calls
`MemoryScheduler()` each interval, and closes it after Ctrl-C. All policy
amounts below are MiB. Balloon statistics and targets use KiB; host free
memory arrives in bytes and is converted to KiB. Building for a live host
requires libvirt development headers and libraries.

## One interval: where the decisions live

1. `fetch_vm_states` lists active domains and obtains each domain's ID and
   UUID. `read_memory_statistics` asks libvirt to collect balloon statistics
   every `interval` seconds, reads `ACTUAL_BALLOON` (current allocation),
   `UNUSED` (guest-reported unused memory), and `LAST_UPDATE` when available,
   and gets the configured maximum from `virDomainGetInfo`. It also reads host
   free memory with `virNodeGetFreeMemory`. The small `VmMap` carries each
   guest's measurements and any outstanding target between passes. A matching
   UUID is required before reusing history for a domain ID.
2. `plan_target_memory` builds a plan from the snapshot without calling
   libvirt. `plan_reclamations` considers guests in descending order of
   unused memory. A guest is a donor only above **300 MiB unused**. One
   request reclaims at most **100 MiB** and is also bounded so that the
   measured unused memory would remain at least **100 MiB**.
3. `plan_grants` considers guests in ascending order of unused memory. A
   guest below **200 MiB unused** may receive at most **100 MiB** in one
   request. Its target cannot exceed its configured maximum or the project's
   **2048 MiB** ceiling. A single shared grant budget is host free memory
   minus a **200 MiB** host reserve and any grants still pending from earlier
   passes. Each planned grant reduces that budget. Targets are rounded down
   to **4 KiB** increments; a budget smaller than one page permits no grant.
4. `apply_memory_targets` calls `virDomainSetMemory` only when the proposed
   target differs from the observed allocation and no earlier request for
   that guest is pending. After an accepted request, the target stays pending
   until a valid sample reports that allocation. A stalled target is reissued
   after three passes without allocation progress. `save_vm_history`
   retains the measurements and pending requests for the next pass; cleanup
   functions free temporary plans, maps, and domain handles.

For example, a guest at 512 MiB allocated with 350 MiB unused is eligible to
donate 100 MiB, producing a 412 MiB target. A different guest at 512 MiB
allocated with 150 MiB unused is eligible to receive 100 MiB, producing a
612 MiB target if the host budget permits. The reclaimed 100 MiB is **not**
counted toward that grant in the same pass. It becomes available only after a
later host-free-memory reading reports it.

## Why the guardrails matter

- The 200/300 MiB gap is hysteresis: small changes near one threshold do not
  immediately reverse the previous decision. The 100 MiB step also makes
  growth and reclamation gradual. Because a donor must start above 300 MiB,
  one 100 MiB request is expected to leave more than 200 MiB unused at the
  measurement time; the explicit 100 MiB floor is an additional bound. Guest
  demand can change after sampling, so these are safeguards based on the
  available statistics, not a guarantee about future free memory.
- Missing or invalid `ACTUAL_BALLOON` or `UNUSED` data makes that guest
  ineligible for an action. A zero or regressed `LAST_UPDATE` is ignored, and
  a newer timestamp alone does not make unchanged measurements fresh. If the
  timestamp is absent, changed valid readings can still be used. Failed
  requests on unchanged readings are retried after three passes; changed
  readings can prompt an earlier retry when a timestamp is available. A
  pending request prevents a different target while the balloon responds.
- If host free memory is at or below 200 MiB, no new grants are planned.
  Reclamation can still proceed. The planner subtracts outstanding grants
  before sharing the remaining host budget, so several hungry guests cannot
  each spend the same free memory. A failed libvirt request is logged and
  reconsidered on a later eligible sample.
- Failure to enable the statistics period is logged, but the coordinator
  still attempts to read statistics that another process may have enabled.
  `MEMORY_DEBUG=1` prints sampled sizes, the host grant budget, and reasons
  growth is blocked. Libvirt failures go to stderr with the call name, UUID
  when known, and available error detail. Empty memory statistics and a zero
  host-free-memory result are also reported.

The policy takes the general idea of reclaiming idle guest memory and
rebalancing it over time from [*Memory Resource Management in VMware ESX
Server*](https://usenix.org/legacy/events/osdi02/tech/waldspurger/waldspurger_html/esx-mem-html.html).
The thresholds, step size, and reserves here are the concrete rules used by
this implementation.

## Evidence and how to reproduce it

From the project root, `make -C tests check` runs the coordinator source
against mocked libvirt calls; `make -C tests sanitize` adds AddressSanitizer
and UndefinedBehaviorSanitizer. Both commands passed locally. The mock checks
cover thresholds, reserves, partial grants, configured and project limits,
stale or missing statistics, delayed balloon responses, failed requests,
pending-grant accounting, and three 100-round demand patterns. The
simulations check gradual changes, conservation of memory, and eventual
reclamation; they use a simplified model of balloon behavior.

In the recorded [memory test 3 log](../test/memory_coordinator3.log), one
guest's allocation rises from 512 MiB at iteration 9 to 612 MiB at iteration
10 and 1212 MiB at iteration 25, then falls to 448 MiB by iteration 75 after
demand subsides. The [memory test 2 log](../test/memory_coordinator2.log)
shows one guest at the 2048 MiB ceiling at iteration 50 and all four at
448 MiB by iteration 75. These logs show observed VM behavior; the mock
checks isolate the safety and retry rules. See the
[live test instructions](../test/HowToDoTest.md) for reproducing the runs.
