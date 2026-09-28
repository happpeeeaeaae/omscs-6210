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

/* Largest-load-first scheduling, using one sample from the preceding call. */
void CPUScheduler(virConnectPtr conn, int interval)
{
	static CpuSample *previous = NULL;
	static int previous_count = 0;
	static double previous_time = 0.0;
	virDomainPtr *domains = NULL;
	unsigned char *online = NULL, *pinmap = NULL;
	CpuSample *samples = NULL;
	double *loads = NULL, *planned = NULL;
	unsigned int nonline = 0;
	int count = 0, ready = 1, must_pin = 0;
	int ndomains = 0;
	struct timespec now;

	if (interval <= 0)
		return;
	int ncpus = virNodeGetCPUMap(conn, &online, &nonline, 0);
	if (ncpus <= 0 || nonline == 0 || clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		goto cleanup;
	double sample_time = now.tv_sec + now.tv_nsec / 1e9;
	double elapsed = sample_time - previous_time;
	int maplen = VIR_CPU_MAPLEN(ncpus);
	ndomains = virConnectListAllDomains(conn, &domains, VIR_CONNECT_LIST_DOMAINS_ACTIVE);
	if (ndomains < 0)
		goto cleanup;
	if (ndomains == 0)
	{
		free(previous);
		previous = NULL;
		previous_count = 0;
		goto cleanup;
	}

	loads = calloc(ncpus, sizeof(*loads));
	planned = calloc(ncpus, sizeof(*planned));
	pinmap = calloc(maplen, 1);
	if (!loads || !planned || !pinmap)
		goto cleanup;

	for (int d = 0; d < ndomains; d++)
	{
		virDomainInfo info;
		char uuid[VIR_UUID_STRING_BUFLEN];
		unsigned int id = virDomainGetID(domains[d]);
		if (id == UINT_MAX || virDomainGetUUIDString(domains[d], uuid) < 0 ||
		    virDomainGetInfo(domains[d], &info) < 0 || info.nrVirtCpu == 0)
			goto cleanup;

		CpuSample *grown = realloc(samples, (count + info.nrVirtCpu) * sizeof(*samples));
		if (!grown)
			goto cleanup;
		samples = grown;
		virVcpuInfoPtr vcpus = calloc(info.nrVirtCpu, sizeof(*vcpus));
		unsigned char *maps = calloc(info.nrVirtCpu, maplen);
		if (!vcpus || !maps)
		{
			free(vcpus);
			free(maps);
			goto cleanup;
		}
		int nread = virDomainGetVcpus(domains[d], vcpus, info.nrVirtCpu, maps, maplen);
		if (nread != info.nrVirtCpu)
		{
			free(vcpus);
			free(maps);
			goto cleanup;
		}

		for (int v = 0; v < nread; v++)
		{
			CpuSample *sample = &samples[count++];
			memset(sample, 0, sizeof(*sample));
			strcpy(sample->uuid, uuid);
			sample->domain_id = id;
			sample->domain_index = d;
			sample->vcpu = vcpus[v].number;
			sample->cpu_time = vcpus[v].cpuTime;
			int allowed = 0, pinned = -1;
			for (int cpu = 0; cpu < ncpus; cpu++)
				if (VIR_CPU_USED(maps + v * maplen, cpu))
				{
					allowed++;
					pinned = cpu;
				}
			sample->needs_pin = allowed != 1 || !VIR_CPU_USED(online, pinned);
			sample->current_cpu = sample->needs_pin ? vcpus[v].cpu : pinned;
			if (sample->current_cpu < 0 || sample->current_cpu >= ncpus ||
			    !VIR_CPU_USED(online, sample->current_cpu))
			{
				sample->needs_pin = 1;
				for (int cpu = 0; cpu < ncpus; cpu++)
					if (VIR_CPU_USED(online, cpu))
					{
						sample->current_cpu = cpu;
						break;
					}
			}
			must_pin |= sample->needs_pin;

			int found = 0;
			for (int old = 0; old < previous_count; old++)
				if (previous[old].vcpu == sample->vcpu &&
				    previous[old].domain_id == id && !strcmp(previous[old].uuid, uuid))
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
				ready = 0; // New/restarted vCPUs need another sample before scheduling.
			loads[sample->current_cpu] += sample->usage;
		}
		free(vcpus);
		free(maps);
	}

	if (ready)
	{
		double current_deviation = load_deviation(loads, online, ncpus, nonline);
		if (must_pin || current_deviation > BALANCED_DEVIATION)
		{
			qsort(samples, count, sizeof(*samples), compare_cpu_usage);
			for (int v = 0; v < count; v++)
			{
				int best = -1;
				for (int cpu = 0; cpu < ncpus; cpu++)
					if (VIR_CPU_USED(online, cpu) &&
					    (best < 0 || planned[cpu] < planned[best] ||
					     (planned[cpu] == planned[best] && cpu == samples[v].current_cpu)))
						best = cpu;
				samples[v].target_cpu = best;
				planned[best] += samples[v].usage;
			}
			double improvement = current_deviation - load_deviation(planned, online, ncpus, nonline);
			if (must_pin || improvement >= MIN_IMPROVEMENT)
				for (int v = 0; v < count; v++)
					if (samples[v].needs_pin || samples[v].target_cpu != samples[v].current_cpu)
					{
						memset(pinmap, 0, maplen);
						VIR_USE_CPU(pinmap, samples[v].target_cpu);
						if (virDomainPinVcpu(domains[samples[v].domain_index], samples[v].vcpu,
								     pinmap, maplen) < 0)
							fprintf(stderr, "Could not pin vCPU %u of %s\n", samples[v].vcpu, samples[v].uuid);
					}
		}
	}

	free(previous);
	previous = samples;
	previous_count = count;
	previous_time = sample_time;
	samples = NULL;

cleanup:
	free(samples);
	free(loads);
	free(planned);
	free(pinmap);
	free(online);
	for (int d = 0; d < ndomains; d++)
		virDomainFree(domains[d]);
	free(domains);
}
