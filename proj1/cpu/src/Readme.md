# vCPU scheduler

Build with `make` in this directory and run `./vcpu_scheduler <positive interval>`.
The required scaffold opens `qemu:///system`, calls `CPUScheduler` at that interval,
and closes the connection after SIGINT.

## Greedy scheduling rule

The scheduler uses [Graham's Longest Processing Time first (LPT) rule](https://fanchung.ucsd.edu/ron/papers/69_02_multiprocessing.pdf): order jobs by decreasing size, then assign each job to the least loaded processor. Each vCPU is a job, each online pCPU is a processor, and the vCPU's recent CPU use is its estimated size. UUID and vCPU number break job-order ties. For equal planned pCPU loads, the current pCPU is preferred, followed by the pCPU with fewer assigned vCPUs and then the lowest pCPU ID.

Classical LPT assumes fixed job sizes and has no migration cost. This scheduler samples again every interval and changes pins only when the new placement is predicted to reduce the standard deviation of pCPU loads by at least one percentage point. It leaves an already balanced placement alone when the deviation is at most five percentage points, except that it fixes an affinity mask that does not pin a vCPU to one online pCPU. These are the changes needed for periodic pinning and the assignment's stability goal. LPT's mathematical bound concerns completion time for fixed jobs; it does not guarantee the assignment's five point deviation for changing VM demand.

## Program flow

1. `initialize_scheduler_state` registers cleanup for the saved sample. The scaffold owns the libvirt connection.
2. `read_cpu_snapshot` obtains the online pCPU IDs, active domains, each vCPU's cumulative CPU time, and its affinity mask. VM records are sorted by running domain ID for lookup. Each record also stores its UUID so a reused or changed ID cannot silently match a prior sample.
3. `plan_cpu_pins` compares the complete snapshot with the preceding complete snapshot. It calculates each vCPU's utilization as `100 × CPU-time delta / monotonic elapsed time`, chooses a target for every vCPU, and returns the changed pins. This function makes no libvirt calls and does not mutate either input snapshot.
4. `apply_pin_changes` calls `virDomainPinVcpu` only for changed or invalid pins. A failed request is reported; the next cycle reads actual affinity again before deciding what to do.
5. `release_cycle` frees libvirt domain handles and all temporary data. The current complete snapshot becomes the next baseline; the registered cleanup frees that baseline on exit.

The first sample is only a baseline. A new or restarted VM, a counter reset, or nonpositive elapsed time also requires a fresh baseline before redistribution. Incomplete statistics abort the cycle without replacing the previous complete sample. An empty domain list clears the baseline. Counts and affinity mask sizes come from libvirt, so the code handles multiple vCPUs, sparse online pCPU IDs, and masks longer than one byte.

## Tests and limits

Run `make -C ../../tests check` and `make -C ../../tests sanitize` from this directory for mocked libvirt tests. Follow `../test/HowToDoTest.md` for live VM tests. Measured vCPU use can understate demand when several busy vCPUs contend for one pCPU, so the greedy result may take several intervals to settle. This scheduler does not model NUMA, host processes, or cache migration cost.

Libvirt API references: [domain and vCPU statistics and pinning](https://libvirt.org/html/libvirt-libvirt-domain.html), [online pCPU map](https://libvirt.org/html/libvirt-libvirt-host.html#virNodeGetCPUMap).
