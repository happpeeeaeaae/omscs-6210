# vCPU scheduler

Build with `make`, then run `./vcpu_scheduler 2` to check the VMs every two
seconds. `read_cpu_snapshot` records each vCPU's CPU time and current pin.
`collect_cpu_work` compares CPU time with the previous reading to measure
recent activity. When a VM does more or less work, its measured CPU use rises
or falls on the next check. The first reading provides a starting point.

`plan_cpu_pins` adds the vCPU loads on each physical CPU. If those loads are
already close (a standard deviation of at most 5 percentage points), it
keeps valid current pins. Otherwise, it considers the busiest vCPUs first and
places each on the least loaded physical CPU. It chooses the new arrangement
only when the predicted balance improves by at least 1 percentage point.

`apply_pin_changes` sends only needed pin changes to libvirt. It also repairs
broad or invalid pins. If activity later moves to different VMs and the
physical CPUs become uneven again, the next readings can trigger another
adjustment. Stable workloads keep their pins, avoiding unnecessary moves.
