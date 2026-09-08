#!/usr/bin/env python

from __future__ import print_function
from vm import VMManager
import subprocess
import time

VM_PREFIX = "aos"

if __name__ == '__main__':
    manager = VMManager() 
    vms=manager.getRunningVMNames(filterPrefix=VM_PREFIX)
    
    for vmname in vms:
        subprocess.call(['virsh', 'shutdown', vmname])
        time.sleep(15)