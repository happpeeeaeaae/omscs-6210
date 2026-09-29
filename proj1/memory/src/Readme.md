# Memory coordinator

Build with `make` in this directory and run with a positive interval, for
example `./memory_coordinator 2`. `main` opens `qemu:///system` and calls
`MemoryScheduler()` once per interval. A live build needs libvirt development
headers and libraries. Policy amounts below are MiB; libvirt balloon sizes
are KiB, and host free memory is converted from bytes to KiB.

## One interval: measurement, decision, action

1. `fetch_vm_states` lists active VMs and reads `ACTUAL_BALLOON`, `UNUSED`,
   optional `LAST_UPDATE`, each VM's configured maximum, and host free
   memory. `read_memory_statistics` requests periodic balloon statistics.
   State is reused only when a VM's ID **and UUID** match the prior pass.
2. `plan_target_memory` builds targets without calling libvirt. A VM with
   more than **300 MiB unused** can donate up to **100 MiB** per pass;
   `plan_reclamations` also limits the amount so the measured unused memory
   remains at least **100 MiB**. Donors are considered from most unused down.
3. `plan_grants` considers VMs with less than **200 MiB unused**, from least
   unused up. Each may receive at most **100 MiB**, stopping at its configured
   maximum or **2048 MiB**. The shared budget is host free memory minus the
   **200 MiB host reserve** and outstanding grants. Each planned grant spends
   that budget; reclaimed memory becomes spendable only after a later host
   reading reports it free. Targets are rounded down to 4 KiB pages.
4. `apply_memory_targets` sends changed targets through
   `virDomainSetMemory`. An accepted target remains pending until a valid
   sample reports the target allocation, preventing a different request
   while the balloon responds. `save_vm_history` carries measurements and
   pending requests into the next interval.

At 512 MiB allocated with 350 MiB unused, a VM can donate 100 MiB and
receive a 412 MiB target. At 512 MiB allocated with 150 MiB unused, a VM can
receive up to 100 MiB and reach 612 MiB when the host budget permits.

## Safety, retries, and limits

- The 200/300 MiB thresholds provide hysteresis; the 100 MiB step makes
  changes gradual. A donor above 300 MiB should retain more than 200 MiB
  unused after one full step **at sampling time**. The explicit 100 MiB bound
  is a second check. Guest demand can rise after sampling, so no policy
  based on these statistics can guarantee future unused memory.
- Missing or invalid balloon values, a zero timestamp, and a regressed
  timestamp are ignored. A newer timestamp alone does not make unchanged
  values fresh; only changed valid sizes update stored measurements.
  When `LAST_UPDATE` is absent, changed valid readings remain usable.
- Failed requests on unchanged readings become eligible to retry after three
  passes; changed readings can permit an earlier retry when a timestamp is
  available. A pending target is reissued after three passes without observed allocation
  progress when statistics are valid. Failed libvirt calls are logged, and
  statistics may still be read if enabling their period fails.
- The 200 MiB host reserve gates **new** grants. A stalled growth retry
  resends its existing target without a new host-budget check; changes in
  host demand while a target is pending therefore remain a limitation.

## Evidence

`make -C tests check` and `make -C tests sanitize` exercise the coordinator
with mocked libvirt calls. Cases cover thresholds, reserves, partial grants,
the 2048 MiB cap, stale and missing statistics, delayed balloon responses,
failed calls, retries, and three 100-round demand patterns. The recorded
[test 2](../test/memory_coordinator2.log) and
[test 3](../test/memory_coordinator3.log) show guests reaching 2048 MiB and
later releasing memory. Those logs are earlier VM runs; they do not validate
changes made afterward. The mocks verify decision rules under a simplified
balloon model. See the [live test procedure](../test/HowToDoTest.md) to repeat
the runs with the current source.
