#include <stdio.h>
#include <stdlib.h>
#include <libvirt/libvirt.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <time.h>

int is_exit = 0; // DO NOT MODIFY THIS VARIABLE

void CPUScheduler(virConnectPtr connection, int interval);

/*
DO NOT CHANGE THE FOLLOWING FUNCTION
*/
void signal_callback_handler()
{
	printf("Caught Signal");
	is_exit = 1;
}

/*
DO NOT CHANGE THE FOLLOWING FUNCTION
*/
int main(int argc, char *argv[])
{
	virConnectPtr conn;

	if (argc != 2)
	{
		printf("Incorrect number of arguments\n");
		return 0;
	}

	// Gets the interval passes as a command line argument and sets it as the STATS_PERIOD for collection of balloon memory statistics of the domains
	int interval = atoi(argv[1]);

	conn = virConnectOpen("qemu:///system");
	if (conn == NULL)
	{
		fprintf(stderr, "Failed to open connection\n");
		return 1;
	}

	// Get the total number of pCpus in the host
	signal(SIGINT, signal_callback_handler);

	while (!is_exit)
	// Run the CpuScheduler function that checks the CPU Usage and sets the pin at an interval of "interval" seconds
	{
		CPUScheduler(conn, interval);
		sleep(interval);
	}

	// Closing the connection
	virConnectClose(conn);
	return 0;
}

/* Only sampled data enters the planner. Libvirt handles stay in CpuCycle. */
typedef struct {
	unsigned int number;
	unsigned long long cpu_time_ns;
	int current_cpu;
	bool has_single_online_pin;
} CpuVcpuState;

typedef struct {
	unsigned int domain_id;
	char uuid[VIR_UUID_STRING_BUFLEN];
	CpuVcpuState *vcpus;
	size_t vcpu_count;
	int domain_index;
} CpuVmState;

typedef struct {
	CpuVmState *vms;
	size_t vm_count;
	int *online_cpus;
	size_t online_cpu_count;
	int host_cpu_count;
	struct timespec sampled_at;
} CpuSnapshot;

typedef struct {
	CpuSnapshot snapshot;
	virDomainPtr *domains;
	size_t domain_count;
} CpuCycle;

typedef struct {
	unsigned int domain_id;
	char uuid[VIR_UUID_STRING_BUFLEN];
	unsigned int vcpu_number;
	int target_cpu;
} CpuAssignment;

typedef struct {
	CpuAssignment *targets;
	size_t target_count;
	CpuAssignment *changes;
	size_t change_count;
} CpuPlan;

typedef struct {
	const CpuVmState *vm;
	const CpuVcpuState *vcpu;
	double usage_percent;
	int target_cpu;
} CpuWork;

typedef struct {
	CpuWork *work_items;
	double *current_loads;
	double *planned_loads;
	size_t *planned_counts;
	size_t work_count;
} CpuPlanWorkspace;

typedef struct {
	CpuSnapshot previous;
	bool initialized;
} SchedulerState;

static SchedulerState scheduler_state;

static void release_snapshot(CpuSnapshot *snapshot)
{
	if (snapshot->vms != NULL)
		for (size_t vm_index = 0; vm_index < snapshot->vm_count; vm_index++)
			free(snapshot->vms[vm_index].vcpus);
	free(snapshot->vms);
	free(snapshot->online_cpus);
	memset(snapshot, 0, sizeof(*snapshot));
}

static void release_cycle(CpuCycle *cycle)
{
	for (size_t domain_index = 0; domain_index < cycle->domain_count; domain_index++)
		virDomainFree(cycle->domains[domain_index]);
	free(cycle->domains);
	release_snapshot(&cycle->snapshot);
	memset(cycle, 0, sizeof(*cycle));
}

static void release_scheduler_state(void)
{
	release_snapshot(&scheduler_state.previous);
}

static int initialize_scheduler_state(void)
{
	if (scheduler_state.initialized)
		return 0;
	if (atexit(release_scheduler_state) != 0)
		return -1;
	scheduler_state.initialized = true;
	return 0;
}

static int online_cpu_index(const CpuSnapshot *snapshot, int cpu_number)
{
	for (size_t cpu_index = 0; cpu_index < snapshot->online_cpu_count; cpu_index++)
		if (snapshot->online_cpus[cpu_index] == cpu_number)
			return (int)cpu_index;
	return -1;
}

static int read_online_cpus(virConnectPtr connection, CpuSnapshot *snapshot)
{
	unsigned char *online_map = NULL;
	unsigned int reported_online_count = 0;
	int host_cpu_count = virNodeGetCPUMap(connection, &online_map,
						    &reported_online_count, 0);
	if (host_cpu_count <= 0 || reported_online_count == 0) {
		free(online_map);
		return -1;
	}

	snapshot->online_cpus = calloc((size_t)host_cpu_count, sizeof(*snapshot->online_cpus));
	if (snapshot->online_cpus == NULL) {
		free(online_map);
		return -1;
	}
	snapshot->host_cpu_count = host_cpu_count;
	for (int cpu_number = 0; cpu_number < host_cpu_count; cpu_number++)
		if (VIR_CPU_USED(online_map, cpu_number))
			snapshot->online_cpus[snapshot->online_cpu_count++] = cpu_number;
	free(online_map);
	return snapshot->online_cpu_count == reported_online_count ? 0 : -1;
}

static void record_vcpu_state(CpuVcpuState *state, const virVcpuInfo *info,
			      const unsigned char *affinity, const CpuSnapshot *snapshot)
{
	int pinned_cpu = -1;
	int allowed_count = 0;
	for (int cpu_number = 0; cpu_number < snapshot->host_cpu_count; cpu_number++) {
		if (!VIR_CPU_USED(affinity, cpu_number))
			continue;
		pinned_cpu = cpu_number;
		allowed_count++;
	}
	state->number = info->number;
	state->cpu_time_ns = info->cpuTime;
	state->has_single_online_pin = allowed_count == 1 &&
					      online_cpu_index(snapshot, pinned_cpu) >= 0;
	state->current_cpu = state->has_single_online_pin ? pinned_cpu : info->cpu;
	if (online_cpu_index(snapshot, state->current_cpu) < 0)
		state->current_cpu = snapshot->online_cpus[0];
}

static int read_vm_state(virDomainPtr domain, int domain_index,
			 const CpuSnapshot *snapshot, CpuVmState *vm)
{
	virDomainInfo domain_info;
	vm->domain_id = virDomainGetID(domain);
	vm->domain_index = domain_index;
	if (vm->domain_id == UINT_MAX ||
	    virDomainGetUUIDString(domain, vm->uuid) < 0 ||
	    virDomainGetInfo(domain, &domain_info) < 0 || domain_info.nrVirtCpu == 0)
		return -1;

	vm->vcpu_count = domain_info.nrVirtCpu;
	int map_length = VIR_CPU_MAPLEN(snapshot->host_cpu_count);
	virVcpuInfo *vcpu_info = calloc(vm->vcpu_count, sizeof(*vcpu_info));
	unsigned char *affinities = calloc(vm->vcpu_count, (size_t)map_length);
	vm->vcpus = calloc(vm->vcpu_count, sizeof(*vm->vcpus));
	if (vcpu_info == NULL || affinities == NULL || vm->vcpus == NULL) {
		free(vcpu_info);
		free(affinities);
		return -1;
	}

	int read_count = virDomainGetVcpus(domain, vcpu_info, (int)vm->vcpu_count,
					    affinities, map_length);
	if (read_count == (int)vm->vcpu_count)
		for (size_t vcpu_index = 0; vcpu_index < vm->vcpu_count; vcpu_index++)
			record_vcpu_state(&vm->vcpus[vcpu_index], &vcpu_info[vcpu_index],
					  affinities + vcpu_index * (size_t)map_length, snapshot);
	free(vcpu_info);
	free(affinities);
	return read_count == (int)vm->vcpu_count ? 0 : -1;
}

static int compare_vm_ids(const void *left, const void *right)
{
	const CpuVmState *left_vm = left;
	const CpuVmState *right_vm = right;
	return (left_vm->domain_id > right_vm->domain_id) -
	       (left_vm->domain_id < right_vm->domain_id);
}

static int sort_and_validate_vm_ids(CpuSnapshot *snapshot)
{
	if (snapshot->vm_count > 1)
		qsort(snapshot->vms, snapshot->vm_count, sizeof(*snapshot->vms), compare_vm_ids);
	for (size_t vm_index = 1; vm_index < snapshot->vm_count; vm_index++)
		if (snapshot->vms[vm_index - 1].domain_id == snapshot->vms[vm_index].domain_id)
			return -1;
	return 0;
}

/* The cycle owns domain handles; the snapshot owns only copied observations. */
static int read_cpu_snapshot(virConnectPtr connection, CpuCycle *cycle)
{
	CpuSnapshot *snapshot = &cycle->snapshot;
	if (read_online_cpus(connection, snapshot) < 0)
		return -1;
	int domain_count = virConnectListAllDomains(connection, &cycle->domains,
						    VIR_CONNECT_LIST_DOMAINS_ACTIVE);
	if (domain_count < 0)
		return -1;
	cycle->domain_count = (size_t)domain_count;
	snapshot->vm_count = cycle->domain_count;
	if (snapshot->vm_count > 0) {
		snapshot->vms = calloc(snapshot->vm_count, sizeof(*snapshot->vms));
		if (snapshot->vms == NULL)
			return -1;
	}
	for (size_t domain_index = 0; domain_index < cycle->domain_count; domain_index++)
		if (read_vm_state(cycle->domains[domain_index], (int)domain_index,
				  snapshot, &snapshot->vms[domain_index]) < 0)
			return -1;
	if (sort_and_validate_vm_ids(snapshot) < 0)
		return -1;
	return clock_gettime(CLOCK_MONOTONIC, &snapshot->sampled_at) == 0 ? 0 : -1;
}

static const CpuVmState *find_vm(const CpuSnapshot *snapshot, unsigned int domain_id)
{
	size_t first = 0;
	size_t last = snapshot->vm_count;
	while (first < last) {
		size_t middle = first + (last - first) / 2;
		if (snapshot->vms[middle].domain_id < domain_id)
			first = middle + 1;
		else
			last = middle;
	}
	return first < snapshot->vm_count &&
	       snapshot->vms[first].domain_id == domain_id ? &snapshot->vms[first] : NULL;
}

static const CpuVcpuState *find_vcpu(const CpuVmState *vm, unsigned int vcpu_number)
{
	for (size_t vcpu_index = 0; vcpu_index < vm->vcpu_count; vcpu_index++)
		if (vm->vcpus[vcpu_index].number == vcpu_number)
			return &vm->vcpus[vcpu_index];
	return NULL;
}

static double elapsed_seconds(const CpuSnapshot *current, const CpuSnapshot *previous)
{
	return (double)(current->sampled_at.tv_sec - previous->sampled_at.tv_sec) +
	       (double)(current->sampled_at.tv_nsec - previous->sampled_at.tv_nsec) / 1e9;
}

static size_t count_vcpus(const CpuSnapshot *snapshot)
{
	size_t total = 0;
	for (size_t vm_index = 0; vm_index < snapshot->vm_count; vm_index++)
		total += snapshot->vms[vm_index].vcpu_count;
	return total;
}

/* Return false when a first sample, restart, or counter reset needs a new baseline. */
static bool collect_cpu_work(const CpuSnapshot *current, const CpuSnapshot *previous,
			     CpuWork *work, double *current_loads)
{
	double elapsed = elapsed_seconds(current, previous);
	if (elapsed <= 0.0)
		return false;
	size_t work_index = 0;
	for (size_t vm_index = 0; vm_index < current->vm_count; vm_index++) {
		const CpuVmState *vm = &current->vms[vm_index];
		const CpuVmState *old_vm = find_vm(previous, vm->domain_id);
		if (old_vm == NULL || strcmp(vm->uuid, old_vm->uuid) != 0)
			return false;
		for (size_t vcpu_index = 0; vcpu_index < vm->vcpu_count; vcpu_index++) {
			const CpuVcpuState *vcpu = &vm->vcpus[vcpu_index];
			const CpuVcpuState *old_vcpu = find_vcpu(old_vm, vcpu->number);
			if (old_vcpu == NULL || vcpu->cpu_time_ns < old_vcpu->cpu_time_ns)
				return false;
			CpuWork *entry = &work[work_index++];
			entry->vm = vm;
			entry->vcpu = vcpu;
			entry->usage_percent = (double)(vcpu->cpu_time_ns - old_vcpu->cpu_time_ns) /
					       (elapsed * 1e9) * 100.0;
			entry->target_cpu = vcpu->current_cpu;
			int cpu_index = online_cpu_index(current, vcpu->current_cpu);
			current_loads[cpu_index] += entry->usage_percent;
		}
	}
	return true;
}

static double load_deviation(const double *loads, size_t cpu_count)
{
	double mean = 0.0;
	double variance = 0.0;
	for (size_t cpu_index = 0; cpu_index < cpu_count; cpu_index++)
		mean += loads[cpu_index];
	mean /= (double)cpu_count;
	for (size_t cpu_index = 0; cpu_index < cpu_count; cpu_index++)
		variance += (loads[cpu_index] - mean) * (loads[cpu_index] - mean);
	return sqrt(variance / (double)cpu_count);
}

static int compare_workload(const void *left, const void *right)
{
	const CpuWork *left_work = left;
	const CpuWork *right_work = right;
	if (left_work->usage_percent != right_work->usage_percent)
		return left_work->usage_percent < right_work->usage_percent ? 1 : -1;
	int uuid_order = strcmp(left_work->vm->uuid, right_work->vm->uuid);
	if (uuid_order != 0)
		return uuid_order;
	return (left_work->vcpu->number > right_work->vcpu->number) -
	       (left_work->vcpu->number < right_work->vcpu->number);
}

static size_t least_loaded_cpu(const CpuSnapshot *snapshot, const CpuWork *work,
			       const double *planned_loads, const size_t *planned_counts)
{
	size_t best = 0;
	for (size_t candidate = 1; candidate < snapshot->online_cpu_count; candidate++) {
		if (planned_loads[candidate] < planned_loads[best]) {
			best = candidate;
			continue;
		}
		if (planned_loads[candidate] != planned_loads[best])
			continue;
		bool candidate_is_current = snapshot->online_cpus[candidate] == work->vcpu->current_cpu;
		bool best_is_current = snapshot->online_cpus[best] == work->vcpu->current_cpu;
		if ((candidate_is_current && !best_is_current) ||
		    (candidate_is_current == best_is_current &&
		     planned_counts[candidate] < planned_counts[best]))
			best = candidate;
	}
	return best;
}

static void assign_greedily(const CpuSnapshot *snapshot, CpuWork *work, size_t work_count,
			    double *planned_loads, size_t *planned_counts)
{
	if (work_count > 1)
		qsort(work, work_count, sizeof(*work), compare_workload);
	for (size_t work_index = 0; work_index < work_count; work_index++) {
		size_t cpu_index = least_loaded_cpu(snapshot, &work[work_index],
						 planned_loads, planned_counts);
		work[work_index].target_cpu = snapshot->online_cpus[cpu_index];
		planned_loads[cpu_index] += work[work_index].usage_percent;
		planned_counts[cpu_index]++;
	}
}

static CpuAssignment make_assignment(const CpuWork *work, int target_cpu)
{
	CpuAssignment assignment = {0};
	assignment.domain_id = work->vm->domain_id;
	strcpy(assignment.uuid, work->vm->uuid);
	assignment.vcpu_number = work->vcpu->number;
	assignment.target_cpu = target_cpu;
	return assignment;
}

static void record_plan(const CpuWork *work, size_t work_count, bool use_greedy,
			CpuPlan *plan)
{
	for (size_t work_index = 0; work_index < work_count; work_index++) {
		const CpuWork *entry = &work[work_index];
		int target_cpu = use_greedy ? entry->target_cpu : entry->vcpu->current_cpu;
		CpuAssignment assignment = make_assignment(entry, target_cpu);
		plan->targets[plan->target_count++] = assignment;
		if (target_cpu != entry->vcpu->current_cpu ||
		    !entry->vcpu->has_single_online_pin)
			plan->changes[plan->change_count++] = assignment;
	}
}

static void release_plan_workspace(CpuPlanWorkspace *workspace)
{
	free(workspace->work_items);
	free(workspace->current_loads);
	free(workspace->planned_loads);
	free(workspace->planned_counts);
	memset(workspace, 0, sizeof(*workspace));
}

static int allocate_plan_workspace(const CpuSnapshot *current, CpuPlanWorkspace *workspace)
{
	workspace->work_count = count_vcpus(current);
	workspace->work_items = calloc(workspace->work_count, sizeof(*workspace->work_items));
	workspace->current_loads = calloc(current->online_cpu_count, sizeof(*workspace->current_loads));
	workspace->planned_loads = calloc(current->online_cpu_count, sizeof(*workspace->planned_loads));
	workspace->planned_counts = calloc(current->online_cpu_count, sizeof(*workspace->planned_counts));
	if (workspace->work_items != NULL && workspace->current_loads != NULL &&
	    workspace->planned_loads != NULL && workspace->planned_counts != NULL)
		return 0;
	release_plan_workspace(workspace);
	return -1;
}

static int allocate_plan_results(size_t work_count, CpuPlan *plan)
{
	plan->targets = calloc(work_count, sizeof(*plan->targets));
	plan->changes = calloc(work_count, sizeof(*plan->changes));
	return plan->targets != NULL && plan->changes != NULL ? 0 : -1;
}

/* Pure decision stage: read two snapshots and return every target plus the changes. */
static int plan_cpu_pins(const CpuSnapshot *current, const CpuSnapshot *previous,
			 CpuPlan *plan)
{
	CpuPlanWorkspace workspace = {0};
	if (allocate_plan_workspace(current, &workspace) < 0)
		return -1;
	if (!collect_cpu_work(current, previous, workspace.work_items, workspace.current_loads)) {
		release_plan_workspace(&workspace);
		return 0;
	}
	if (allocate_plan_results(workspace.work_count, plan) < 0) {
		release_plan_workspace(&workspace);
		return -1;
	}
	double current_deviation = load_deviation(workspace.current_loads, current->online_cpu_count);
	bool use_greedy = current_deviation > 5.0;
	if (use_greedy) {
		assign_greedily(current, workspace.work_items, workspace.work_count,
				workspace.planned_loads, workspace.planned_counts);
		double planned_deviation = load_deviation(workspace.planned_loads,
						     current->online_cpu_count);
		use_greedy = current_deviation - planned_deviation >= 1.0;
	}
	record_plan(workspace.work_items, workspace.work_count, use_greedy, plan);
	release_plan_workspace(&workspace);
	return 1;
}

static void release_plan(CpuPlan *plan)
{
	free(plan->targets);
	free(plan->changes);
	memset(plan, 0, sizeof(*plan));
}

static int apply_pin_changes(const CpuCycle *cycle, const CpuPlan *plan)
{
	if (plan->change_count == 0)
		return 0;
	int map_length = VIR_CPU_MAPLEN(cycle->snapshot.host_cpu_count);
	unsigned char *pin_map = calloc((size_t)map_length, 1);
	if (pin_map == NULL)
		return -1;
	for (size_t change_index = 0; change_index < plan->change_count; change_index++) {
		const CpuAssignment *change = &plan->changes[change_index];
		const CpuVmState *vm = find_vm(&cycle->snapshot, change->domain_id);
		if (vm == NULL || strcmp(vm->uuid, change->uuid) != 0)
			continue;
		memset(pin_map, 0, (size_t)map_length);
		VIR_USE_CPU(pin_map, change->target_cpu);
		if (virDomainPinVcpu(cycle->domains[vm->domain_index], change->vcpu_number,
				     pin_map, map_length) < 0)
			fprintf(stderr, "Could not pin vCPU %u of %s\n",
				change->vcpu_number, change->uuid);
	}
	free(pin_map);
	return 0;
}

static void save_baseline(CpuCycle *cycle)
{
	release_snapshot(&scheduler_state.previous);
	scheduler_state.previous = cycle->snapshot;
	memset(&cycle->snapshot, 0, sizeof(cycle->snapshot));
}

void CPUScheduler(virConnectPtr connection, int interval)
{
	if (connection == NULL || interval <= 0 || initialize_scheduler_state() < 0)
		return;
	CpuCycle cycle = {0};
	CpuPlan plan = {0};
	if (read_cpu_snapshot(connection, &cycle) == 0) {
		int plan_status = cycle.snapshot.vm_count == 0 ? 0 :
			plan_cpu_pins(&cycle.snapshot, &scheduler_state.previous, &plan);
		if (plan_status >= 0 && (plan_status == 0 || apply_pin_changes(&cycle, &plan) == 0))
			save_baseline(&cycle);
	}
	release_plan(&plan);
	release_cycle(&cycle);
}
