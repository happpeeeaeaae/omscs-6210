#!/usr/bin/python
from __future__ import print_function
import subprocess
import time

from vm import VMManager

VM_PREFIX = "aos"

if __name__ == '__main__':
    manager = VMManager()
    vms = manager.getFilteredVms(VM_PREFIX)

    for vm in vms:
        print("Destroying:", vm)
        subprocess.call(['uvt-kvm', 'destroy', vm])

    time.sleep(15)
    remaining = manager.getFilteredVms(VM_PREFIX)
    if remaining:
        print("WARNING: Some VMs were not destroyed:", ", ".join(remaining))
    else:
        print("All VMs destroyed.")