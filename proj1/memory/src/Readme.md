# Memory coordinator

`fetch_vm_states` reads each VM's allocation, unused memory, and
maximum size, along with the host's free memory. As a VM uses more memory,
its unused amount falls; when it frees memory, the unused amount rises. These
readings show when a VM needs more room or has memory it can give back.

`plan_target_memory` decides whether to change each VM's balloon size.
At 150 MiB or less unused, a VM can grow by up to 100 MiB. At more than
300 MiB unused, it can shrink by up to 100 MiB while keeping at least
100 MiB unused. `plan_grants` gives memory to the neediest VMs first,
subject to each VM's limit and a 200 MiB reserve on the host. Between
those thresholds, the allocation stays put. For example, an idle 512 MiB
VM with about 190 MiB unused stays at 512 MiB.

`apply_memory_targets` asks libvirt to make the planned balloon changes.
The coordinator waits for a VM to report its new size before requesting
another change, so memory grows or shrinks in steps as demand changes.
