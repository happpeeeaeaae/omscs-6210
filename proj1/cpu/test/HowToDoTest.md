# CPU Scheduler

This directory contains files for evaluating your CPU Scheduler. There are 3 test cases in total, each of which introduces a different workload.

## Understanding CPU Scheduler Testing  
While designing your CPU scheduler, there are a few things to consider:  
1. What are the kinds of workloads/situations that the scheduler needs to handle?  
2. What is the average case and what are the edge cases?
3. What statistics would help in making a scheduling decision?  

### Test Cases
We provide three test cases as a starting point. Each test is described below, along with the rationale behind the tests. The point we're trying to illustrate is the expected behavior, so we'll not focus on the actual percentages for usage. The setup for each test is the same — there are 8 VMs running on 4 CPU cores.   

#### **Test Case 1**  
- There are 8 VMs with 1 CPU each, running similar workloads. The vCPUs are pinned such that there are 2 vCPUs pinned to each pCPU.   
- Since pinning is already balanced, your scheduler should not be doing any additional work.  
- This is the base case. The scheduler should be able to determine that the CPU utilization is balanced. 
- Note that the pinnings do not change across iterations. In summary, the scheduler does not take any action.  

**Sample output**:
```
Iteration 1:
0 - usage: 100.0 | mapping ['aos_vm3', 'aos_vm2']
1 - usage: 99.0 | mapping ['aos_vm4', 'aos_vm5']
2 - usage: 99.0 | mapping ['aos_vm8', 'aos_vm7']
3 - usage: 100.0 | mapping ['aos_vm1', 'aos_vm6']
Iteration 2:
0 - usage: 100.0 | mapping ['aos_vm3', 'aos_vm2']
1 - usage: 99.0 | mapping ['aos_vm4', 'aos_vm5']
2 - usage: 99.0 | mapping ['aos_vm8', 'aos_vm7']
3 - usage: 99.0 | mapping ['aos_vm1', 'aos_vm6']
Iteration 3:
0 - usage: 100.0 | mapping ['aos_vm3', 'aos_vm2']
1 - usage: 100.0 | mapping ['aos_vm4', 'aos_vm5']
2 - usage: 100.0 | mapping ['aos_vm8', 'aos_vm7']
3 - usage: 100.0 | mapping ['aos_vm1', 'aos_vm6']
```


#### **Test Case 2**  
- There are 8 VMs with 1 CPU each, running similar workloads. All the vCPUs are pinned to pCPU0.   
- Pins should be changed such that there are 2 vCPUs pinned to each available pCPU.  
- The scheduler may take a few iterations to arrive at a stable pinning. Once the workloads on each pCPU is balanced, the scheduler should not make unnecessary changes to the pins.
- This is the second part of the base case. The scheduler should be able to just change pinnings to balance the utilization.  

**Sample output**:
```
Iteration 1:
0 - usage: 100.0 | mapping ['aos_vm3', 'aos_vm2', 'aos_vm1', 'aos_vm4', 'aos_vm6', 'aos_vm5', 'aos_vm7', 'aos_vm8']
1 - usage: 0.0 | mapping []
2 - usage: 0.0 | mapping []
3 - usage: 0.0 | mapping []
Iteration 2:
0 - usage: 150.0 | mapping ['aos_vm3', 'aos_vm2', 'aos_vm7']
1 - usage: 99.0 | mapping ['aos_vm4', 'aos_vm5']
2 - usage: 50.0 | mapping ['aos_vm8']
3 - usage: 99.0 | mapping ['aos_vm1', 'aos_vm6']
Iteration 3:
0 - usage: 100.0 | mapping ['aos_vm3', 'aos_vm2']
1 - usage: 100.0 | mapping ['aos_vm4', 'aos_vm5']
2 - usage: 100.0 | mapping ['aos_vm8', 'aos_vm7']
3 - usage: 100.0 | mapping ['aos_vm1', 'aos_vm6']
Iteration 4:
0 - usage: 100.0 | mapping ['aos_vm3', 'aos_vm2']
1 - usage: 100.0 | mapping ['aos_vm4', 'aos_vm5']
2 - usage: 100.0 | mapping ['aos_vm8', 'aos_vm7']
3 - usage: 100.0 | mapping ['aos_vm1', 'aos_vm6']
```


#### **Test Case 3**  
- There are 8 VMs with 1 CPU each but 4 VMs run a heavy workload and 4 VMs run a light load. The vCPUs are equally likely to run on any pCPU. In other words, the vCPUs are randomly pinned initially.
- The scheduler should use some statistic to decide the optimal pinning which will balance the workload across the pCPUs.
- The VMs are randomly pinned initially. The scheduler then pins one VM with heavy load and one VM with light load to each pCPU.
- This is a rudimentary test for the scheduler which builds on the two previous tests. The scheduler needs to balance the utilization and change the pinnings to do so.

**Sample output**:
Assume that VMs 1, 2, 3 and 4 run heavy loads, and VMs 5, 6, 7 and 8 run light loads. We will use 50 to represent a light load and 100 to represent a heavy load. 
```
Iteration 1:
0 - usage: 150.0 | mapping ['aos_vm3', 'aos_vm2', 'aos_vm1']
1 - usage: 100.0 | mapping ['aos_vm5']
2 - usage: 100.0 | mapping ['aos_vm7']
3 - usage: 250.0 | mapping ['aos_vm6', 'aos_vm4', 'aos_vm8']
Iteration 2:
0 - usage: 150.0 | mapping ['aos_vm3', 'aos_vm2', 'aos_vm1']
1 - usage: 200.0 | mapping ['aos_vm8', 'aos_vm5']
2 - usage: 100.0 | mapping ['aos_vm7']
3 - usage: 150.0 | mapping ['aos_vm6', 'aos_vm4']
Iteration 3:
0 - usage: 150.0 | mapping ['aos_vm3', 'aos_vm8']
1 - usage: 150.0 | mapping ['aos_vm1', 'aos_vm5']
2 - usage: 150.0 | mapping ['aos_vm7', 'aos_vm2']
3 - usage: 150.0 | mapping ['aos_vm6', 'aos_vm4']
Iteration 4:
0 - usage: 150.0 | mapping ['aos_vm3', 'aos_vm8']
1 - usage: 150.0 | mapping ['aos_vm1', 'aos_vm5']
2 - usage: 150.0 | mapping ['aos_vm7', 'aos_vm2']
3 - usage: 150.0 | mapping ['aos_vm6', 'aos_vm4']
```


## Prerequisites for Testing
- Before starting these tests, ensure that you have created 8 virtual machines.
- The names of these VMs **MUST** start with *aos_* (e.g., aos_vm1 for the first VM, aos_vm2 for the second VM, etc.).
- Note that if you started with the Memory Coordinator portion of this assignment you should already have 4 VMs in place and as such will need to create 4 additional VMs. Otherwise if you are starting from this portion of the assignment you will be creating 8 VMs.
- If you need to create VMs, you can do so with the command: `uvt-kvm create aos_vm1 release=bionic --memory 512` (where 'aos_vm1' is the name of the VM you wish to create).
- Ensure the VMs are shutdown before starting. You can do this with the script `~/project1/shutdownallvm.py`.
- Compile the test programs for the evaluations with the following commands:

```bash
$ cd ~/project1/cpu/test/
$ ./makeall.sh
```

- Compile the CPU Scheduler program with the following: 

```bash
$ cd ~/project1/cpu/src/
$ make
```

## Running the Tests

Follow the procedure below for each test case (using Test Case 1 as an example). Before starting the steps below, run: `cd ~/project1/cpu/test`. Most scripts will be run from this directory. If you open a new terminal window, make sure to `cd` back into this directory again before running the test commands.

1. **Start all VMs**: You need to start all VMs before starting the test using `~/project1/startallvm.py` script. Run the script with `python3 ../../startallvm.py`.
2. **Copy test binaries to each VM**: Once the VMs are up and SSH is ready, run the `~/project1/cpu/test/assignall.sh` script to copy the required binaries. You will need to make the script executable with `chmod +x assignall.sh` and then run it with `./assignall.sh`.
3. **Set up logging**: Open a new terminal (or a tmux/screen session) and capture terminal output using the `script` command `script vcpu_scheduler1.log`.
4. **Start the monitoring tool**: In the same terminal, run the monitoring tool with `python3 monitor.py -t runtest1.py`:
   - The output will be logged in `vcpu_scheduler1.log` for submission.  
   - Use `python3 monitor.py --help` to view available flags.
5. **Run your CPU Scheduler**: Open a new terminal and go to `cd ../src/` and execute the `vcpu_scheduler` binary.
6. **Verify correct behavior**: Use the monitoring tool output to ensure your CPU Scheduler behaves as described in the readme `~/project1/README.md`.
7. **Stop logging**: After the test completes, type `exit` to stop the `script` command.  
   - The log will be saved as `vcpu_scheduler1.log`.
8. **Move the log file**: You need to move the log file to `~/project1/cpu/src/` directory. Use `mv vcpu_scheduler1.log ../src`.
9. **Plot the test results**: Generate a graph of the test run using the plotting script:  
   `python3 plot_graph_cpu.py -i <path to logfile> -o <path to save graph>`  
   - This graph is for self-review only and does not need to be submitted.  
   - A sample graph is available [here](../../res/sample-sol-1.pdf).
10. **Shut down the VMs**: Use the shutdown script `~/project1/shutdownallvm.py`. Run the script with `python3 ../../shutdownallvm.py`.
11. **Repeat for remaining test cases**: Repeat these steps for all test cases, updating the test case number as needed.


## Understanding Monitor Output
The *monitor.py* tool will output CPU utilization and mapping statistics in the following format:

```
Iteration Number 1:
0 - usage: 24.0 | mapping ['aos_vm3']
1 - usage: 23.0 | mapping ['aos_vm4']
2 - usage: 23.0 | mapping ['aos_vm8']
3 - usage: 23.0 | mapping ['aos_vm1']
4 - usage: 23.0 | mapping ['aos_vm7']
5 - usage: 24.0 | mapping ['aos_vm6']
6 - usage: 23.0 | mapping ['aos_vm5']
7 - usage: 24.0 | mapping ['aos_vm2']
```

The first line mentions the iteration number. Your vcpu_scheduler should be able to balance all the pCPUs in *25 iterations*. While running *~/project1/cpu/test/monitor.py -t runtest1.py*, the test case *runtest1.py* starts after 5 iterations. That is, first 5 iterations just show you the monitor output without running the test case. Then, the script executes the test (in this case *runtest1.py*). 

Where each line is of the form:

`<pCPU #> - usage: <pCPU % utilization> | mapping <VMs mapped to this pCPU>`

- The first field indicates the pCPU number.
- The second field indicates the % utilization of the given pCPU.
- The third field indicates a given pCPU's mapping to different virtual machines (vCPUs).