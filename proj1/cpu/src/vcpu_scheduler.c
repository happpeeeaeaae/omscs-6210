#include <stdio.h>
#include <stdlib.h>
#include <libvirt/libvirt.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <time.h>
#define MIN(a, b) ((a) < (b) ? a : b)
#define MAX(a, b) ((a) > (b) ? a : b)

int is_exit = 0; // DO NOT MODIFY THIS VARIABLE

void CPUScheduler(virConnectPtr conn, int interval);

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

#define BALANCED_DEVIATION 5.0
#define MIN_IMPROVEMENT 1.0

typedef struct
{
	char uuid[VIR_UUID_STRING_BUFLEN];
	unsigned int domain_id;
	unsigned int vcpu;
	unsigned long long cpu_time;
	int domain_index;
	int current_cpu;
	int target_cpu;
	int needs_pin;
	double usage;
} CpuSample;

typedef struct
{
	virDomainPtr *domains;
	int ndomains;
	unsigned char *online;
	unsigned int nonline;
	int ncpus;
	double sample_time;
	CpuSample *samples;
	int count;
} CpuSnapshot;

typedef struct
{
	int sample_index;
	int target_cpu;
} CpuAction;

static double load_deviation(const double *loads, const unsigned char *online,
			     int ncpus, unsigned int nonline)
{
	double mean = 0.0, variance = 0.0;
	for (int cpu = 0; cpu < ncpus; cpu++)
		if (VIR_CPU_USED(online, cpu))
			mean += loads[cpu];
	mean /= nonline;
	for (int cpu = 0; cpu < ncpus; cpu++)
		if (VIR_CPU_USED(online, cpu))
			variance += (loads[cpu] - mean) * (loads[cpu] - mean);
	return sqrt(variance / nonline);
}

static int compare_cpu_usage(const void *left, const void *right)
{
	const CpuSample *a = left, *b = right;
	if (a->usage != b->usage)
		return a->usage < b->usage ? 1 : -1;
	int order = strcmp(a->uuid, b->uuid);
	if (order != 0)
		return order;
	return (a->vcpu > b->vcpu) - (a->vcpu < b->vcpu);
}

/* Read one complete CPU snapshot; an incomplete domain aborts the cycle. */
static int read_cpu_snapshot(virConnectPtr conn, CpuSnapshot *snapshot)
{
	struct timespec now;
	snapshot->ncpus = virNodeGetCPUMap(conn, &snapshot->online, &snapshot->nonline, 0);
	if (snapshot->ncpus <= 0 || snapshot->nonline == 0 ||
	    clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return -1;
	snapshot->sample_time = now.tv_sec + now.tv_nsec / 1e9;
	snapshot->ndomains = virConnectListAllDomains(conn, &snapshot->domains,
						     VIR_CONNECT_LIST_DOMAINS_ACTIVE);
	if (snapshot->ndomains <= 0)
		return snapshot->ndomains;

	int maplen = VIR_CPU_MAPLEN(snapshot->ncpus);
	for (int d = 0; d < snapshot->ndomains; d++)
	{
		virDomainInfo info;
		char uuid[VIR_UUID_STRING_BUFLEN];
		unsigned int id = virDomainGetID(snapshot->domains[d]);
		if (id == UINT_MAX || virDomainGetUUIDString(snapshot->domains[d], uuid) < 0 ||
		    virDomainGetInfo(snapshot->domains[d], &info) < 0 || info.nrVirtCpu == 0)
			return -1;

		CpuSample *grown = realloc(snapshot->samples,
					   (snapshot->count + info.nrVirtCpu) * sizeof(*grown));
		if (!grown)
			return -1;
		snapshot->samples = grown;
		virVcpuInfoPtr vcpus = calloc(info.nrVirtCpu, sizeof(*vcpus));
		unsigned char *maps = calloc(info.nrVirtCpu, maplen);
		if (!vcpus || !maps)
		{
			free(vcpus);
			free(maps);
			return -1;
		}
		int nread = virDomainGetVcpus(snapshot->domains[d], vcpus,
					     info.nrVirtCpu, maps, maplen);
		if (nread != info.nrVirtCpu)
		{
			free(vcpus);
			free(maps);
			return -1;
		}

		for (int v = 0; v < nread; v++)
		{
			CpuSample *sample = &snapshot->samples[snapshot->count++];
			memset(sample, 0, sizeof(*sample));
			strcpy(sample->uuid, uuid);
			sample->domain_id = id;
			sample->domain_index = d;
			sample->vcpu = vcpus[v].number;
			sample->cpu_time = vcpus[v].cpuTime;
			int allowed = 0, pinned = -1;
			for (int cpu = 0; cpu < snapshot->ncpus; cpu++)
				if (VIR_CPU_USED(maps + v * maplen, cpu))
				{
					allowed++;
					pinned = cpu;
				}
			sample->needs_pin = allowed != 1 || pinned < 0 ||
					    !VIR_CPU_USED(snapshot->online, pinned);
			sample->current_cpu = sample->needs_pin ? vcpus[v].cpu : pinned;
			if (sample->current_cpu < 0 || sample->current_cpu >= snapshot->ncpus ||
			    !VIR_CPU_USED(snapshot->online, sample->current_cpu))
			{
				sample->needs_pin = 1;
				for (int cpu = 0; cpu < snapshot->ncpus; cpu++)
					if (VIR_CPU_USED(snapshot->online, cpu))
					{
						sample->current_cpu = cpu;
						break;
					}
			}
		}
		free(vcpus);
		free(maps);
	}
	return 1;
}

/* Compare this snapshot with the previous one and choose pins without libvirt calls. */
static int plan_cpu_actions(CpuSnapshot *snapshot, const CpuSample *previous,
			    int previous_count, double previous_time,
			    CpuAction **actions, int *action_count)
{
	double *loads = calloc(snapshot->ncpus, sizeof(*loads));
	double *planned = calloc(snapshot->ncpus, sizeof(*planned));
	int ready = 1, must_pin = 0;
	double elapsed = snapshot->sample_time - previous_time;
	if (!loads || !planned)
	{
		free(loads);
		free(planned);
		return -1;
	}

	for (int v = 0; v < snapshot->count; v++)
	{
		CpuSample *sample = &snapshot->samples[v];
		int found = 0;
		must_pin |= sample->needs_pin;
		for (int old = 0; old < previous_count; old++)
			if (previous[old].vcpu == sample->vcpu &&
			    previous[old].domain_id == sample->domain_id &&
			    !strcmp(previous[old].uuid, sample->uuid))
			{
				if (sample->cpu_time >= previous[old].cpu_time && elapsed > 0)
				{
					sample->usage = (sample->cpu_time - previous[old].cpu_time) /
							(elapsed * 1e9) * 100.0;
					found = 1;
				}
				break;
			}
		if (!found)
			ready = 0;
		loads[sample->current_cpu] += sample->usage;
	}

	if (ready)
	{
		double current_deviation = load_deviation(loads, snapshot->online,
							  snapshot->ncpus, snapshot->nonline);
		if (must_pin || current_deviation > BALANCED_DEVIATION)
		{
			double improvement = 0.0;
			if (current_deviation <= BALANCED_DEVIATION)
			{
				for (int v = 0; v < snapshot->count; v++)
					snapshot->samples[v].target_cpu = snapshot->samples[v].current_cpu;
			}
			else
			{
				qsort(snapshot->samples, snapshot->count, sizeof(*snapshot->samples),
				      compare_cpu_usage);
				for (int v = 0; v < snapshot->count; v++)
				{
					int best = -1;
					for (int cpu = 0; cpu < snapshot->ncpus; cpu++)
						if (VIR_CPU_USED(snapshot->online, cpu) &&
						    (best < 0 || planned[cpu] < planned[best] ||
						     (planned[cpu] == planned[best] &&
						      cpu == snapshot->samples[v].current_cpu)))
							best = cpu;
					snapshot->samples[v].target_cpu = best;
					planned[best] += snapshot->samples[v].usage;
				}
				improvement = current_deviation - load_deviation(planned,
									 snapshot->online,
									 snapshot->ncpus,
									 snapshot->nonline);
			}
			if (must_pin || improvement >= MIN_IMPROVEMENT)
			{
				*actions = calloc(snapshot->count, sizeof(**actions));
				if (!*actions)
				{
					free(loads);
					free(planned);
					return -1;
				}
				for (int v = 0; v < snapshot->count; v++)
					if (snapshot->samples[v].needs_pin ||
					    snapshot->samples[v].target_cpu != snapshot->samples[v].current_cpu)
					{
						(*actions)[*action_count].sample_index = v;
						(*actions)[*action_count].target_cpu = snapshot->samples[v].target_cpu;
						(*action_count)++;
					}
			}
		}
	}
	free(loads);
	free(planned);
	return 0;
}

/* Apply only the decisions already made; failed pins are retried from fresh state. */
static int apply_cpu_actions(const CpuSnapshot *snapshot, const CpuAction *actions,
			     int action_count)
{
	if (action_count == 0)
		return 0;
	int maplen = VIR_CPU_MAPLEN(snapshot->ncpus);
	unsigned char *pinmap = calloc(maplen, 1);
	if (!pinmap)
		return -1;
	for (int a = 0; a < action_count; a++)
	{
		const CpuSample *sample = &snapshot->samples[actions[a].sample_index];
		memset(pinmap, 0, maplen);
		VIR_USE_CPU(pinmap, actions[a].target_cpu);
		if (virDomainPinVcpu(snapshot->domains[sample->domain_index], sample->vcpu,
				     pinmap, maplen) < 0)
			fprintf(stderr, "Could not pin vCPU %u of %s\n", sample->vcpu, sample->uuid);
	}
	free(pinmap);
	return 0;
}

/* Largest-load-first scheduling, using one sample from the preceding call. */
void CPUScheduler(virConnectPtr conn, int interval)
{
	static CpuSample *previous = NULL;
	static int previous_count = 0;
	static double previous_time = 0.0;
	CpuSnapshot snapshot = {0};
	CpuAction *actions = NULL;
	int action_count = 0;

	if (interval <= 0)
		return;
	int status = read_cpu_snapshot(conn, &snapshot);
	if (status == 0)
	{
		free(previous);
		previous = NULL;
		previous_count = 0;
	}
	if (status > 0 &&
	    plan_cpu_actions(&snapshot, previous, previous_count, previous_time,
			     &actions, &action_count) == 0 &&
	    apply_cpu_actions(&snapshot, actions, action_count) == 0)
	{
		free(previous);
		previous = snapshot.samples;
		previous_count = snapshot.count;
		previous_time = snapshot.sample_time;
		snapshot.samples = NULL;
	}

	free(actions);
	free(snapshot.samples);
	free(snapshot.online);
	for (int d = 0; d < snapshot.ndomains; d++)
		virDomainFree(snapshot.domains[d]);
	free(snapshot.domains);
}
