#!/usr/bin/env python

from __future__ import print_function
from vm import VMManager
import subprocess

VM_PREFIX = "aos"

if __name__ == '__main__':
    manager = VMManager()
    vms = manager.getFilteredVms(VM_PREFIX)

    for vmname in vms:
        manager.startVM(vmname)

    # Wait for all VMs in parallel; uvt-kvm wait blocks until cloud-init
    # and the SSH daemon are ready.
    waits = [subprocess.Popen(['uvt-kvm', 'wait', vmname]) for vmname in vms]
    for w in waits:
        w.wait()
