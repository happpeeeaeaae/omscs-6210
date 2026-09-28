# Memory Coordinator

Build with `make` in this directory. Run `./memory_coordinator 2` to sample every
two seconds.

## General idea

The coordinator checks each guest's unused memory and the host's free memory.
It plans small reductions for guests with plenty of unused memory, then plans
growth for the hungriest guests within the available host budget. The gap
between the growth and reduction thresholds avoids rapid back-and-forth
changes. Balloon changes take effect asynchronously.

## Where the stages are

| Stage | Code | Purpose |
| --- | --- | --- |
| Read | [read_memory_snapshot](./memory_coordinator.c#L134) | Fetch guest balloon statistics, limits, and host free memory from libvirt. |
| Decide | [plan_memory_actions](./memory_coordinator.c#L231) | Choose ordered guest targets within the reserves and shared budget. |
| Apply | [apply_memory_actions](./memory_coordinator.c#L351) | Send targets to libvirt and record successful requests. |

[MemoryScheduler](./memory_coordinator.c#L375) runs the stages and saves
history for the next call.

## Edge cases

- Missing or unchanged statistics do not trigger an adjustment.
- A pending balloon request blocks another change for that guest until a
  fresh sample reports the requested allocation.
- A failed request can be retried on a later fresh sample. Grant targets stay
  fixed for the current cycle, so a failed grant's budget is reconsidered next
  cycle. Planned reductions are not spendable until the host reports the
  memory free.

## Settings to adjust

The [memory constants](./memory_coordinator.c#L63) set the step size, growth
and reduction thresholds, guest and host reserves, and the guest allocation
cap. They use KiB; 1024 KiB is 1 MiB. The planner also rounds growth down to
[4 KiB pages](./memory_coordinator.c#L331). The sampling interval is the
command-line argument. Update the related assertions in
[memory tests](../../tests/memory_test.c) if these values change.

Run the mock tests with `make -C ../../tests check`. For diagnostic output, use
`MEMORY_DEBUG=1 ./memory_coordinator 2`.
