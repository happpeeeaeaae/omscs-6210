#include <stdio.h>
#include <stdlib.h>
#include <libvirt/libvirt.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#define MIN(a, b) ((a) < (b) ? a : b)
#define MAX(a, b) ((a) > (b) ? a : b)

int is_exit = 0; // DO NOT MODIFY THE VARIABLE

void MemoryScheduler(virConnectPtr conn, int interval);

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

	signal(SIGINT, signal_callback_handler);

	while (!is_exit)
	{
		// Calls the MemoryScheduler function after every 'interval' seconds
		MemoryScheduler(conn, interval);
		sleep(interval);
	}

	// Close the connection
	virConnectClose(conn);
	return 0;
}

#define MEMORY_STEP_KIB (100UL * 1024)
#define GUEST_MIN_UNUSED_KIB (100UL * 1024)
#define GROW_BELOW_KIB (200UL * 1024)
#define RECLAIM_ABOVE_KIB (300UL * 1024)
#define HOST_RESERVE_KIB (200UL * 1024)
#define GUEST_MAX_KIB (2048UL * 1024)

typedef struct
{
	char uuid[VIR_UUID_STRING_BUFLEN];
	unsigned int domain_id;
	int domain_index;
	int stats_period;
	int usable;
	unsigned long long last_update;
	unsigned long long actual;
	unsigned long long unused;
	unsigned long long maximum;
	unsigned long long pending_target;
} MemorySample;

static int compare_unused_memory(const void *left, const void *right)
{
	const MemorySample *a = left, *b = right;
	if (a->unused != b->unused)
		return a->unused < b->unused ? -1 : 1;
	return strcmp(a->uuid, b->uuid);
}

/* Reclaim spare memory first, then give small increments to the hungriest VMs. */
void MemoryScheduler(virConnectPtr conn, int interval)
{
	static MemorySample *previous = NULL;
	static int previous_count = 0;
	virDomainPtr *domains = NULL;
	MemorySample *samples = NULL;

	if (interval <= 0)
		return;
	int ndomains = virConnectListAllDomains(conn, &domains, VIR_CONNECT_LIST_DOMAINS_ACTIVE);
	if (ndomains < 0)
		return;
	if (ndomains == 0)
	{
		free(previous);
		previous = NULL;
		previous_count = 0;
		free(domains);
		return;
	}
	samples = calloc(ndomains, sizeof(*samples));
	if (!samples)
		goto cleanup;

	for (int d = 0; d < ndomains; d++)
	{
		MemorySample *sample = &samples[d];
		char uuid[VIR_UUID_STRING_BUFLEN];
		unsigned int id = virDomainGetID(domains[d]);
		if (id == UINT_MAX || virDomainGetUUIDString(domains[d], uuid) < 0)
		{
			// Keep the old history: this guest may still have a grant in flight.
			goto cleanup;
		}
		for (int old = 0; old < previous_count; old++)
			if (previous[old].domain_id == id && !strcmp(previous[old].uuid, uuid))
			{
				*sample = previous[old];
				break;
			}
		strcpy(sample->uuid, uuid);
		sample->domain_id = id;
		sample->domain_index = d;
		sample->usable = 0;

		if (sample->stats_period != interval)
		{
			if (virDomainSetMemoryStatsPeriod(domains[d], interval, VIR_DOMAIN_AFFECT_LIVE) < 0)
				continue;
			sample->stats_period = interval;
		}
		virDomainInfo info;
		virDomainMemoryStatStruct stats[VIR_DOMAIN_MEMORY_STAT_NR];
		if (virDomainGetInfo(domains[d], &info) < 0)
			continue;
		int nstats = virDomainMemoryStats(domains[d], stats, VIR_DOMAIN_MEMORY_STAT_NR, 0);
		if (nstats < 0)
			continue;

		unsigned long long actual = 0, unused = 0, updated = 0;
		int have_actual = 0, have_unused = 0;
		for (int s = 0; s < nstats; s++)
		{
			if (stats[s].tag == VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON)
			{
				actual = stats[s].val;
				have_actual = 1;
			}
			else if (stats[s].tag == VIR_DOMAIN_MEMORY_STAT_UNUSED)
			{
				unused = stats[s].val;
				have_unused = 1;
			}
			else if (stats[s].tag == VIR_DOMAIN_MEMORY_STAT_LAST_UPDATE)
				updated = stats[s].val;
		}
		if (have_actual)
			sample->actual = actual;
		if (!have_actual || !have_unused || actual == 0 || unused > actual ||
		    updated == 0 || updated <= sample->last_update || info.maxMem == 0)
			continue;
		sample->last_update = updated;
		sample->unused = unused;
		sample->maximum = MIN(info.maxMem, GUEST_MAX_KIB);

		// Balloon changes are asynchronous; don't stack requests on old samples.
		if (sample->pending_target != 0)
		{
			if (actual != sample->pending_target)
				continue;
			sample->pending_target = 0;
		}
		sample->usable = 1;
	}

	qsort(samples, ndomains, sizeof(*samples), compare_unused_memory);
	for (int d = 0; d < ndomains; d++)
	{
		MemorySample *sample = &samples[d];
		if (!sample->usable || sample->unused <= RECLAIM_ABOVE_KIB)
			continue;
		unsigned long long amount = MIN(MEMORY_STEP_KIB, sample->unused - GUEST_MIN_UNUSED_KIB);
		unsigned long target = sample->actual - amount;
		if (virDomainSetMemory(domains[sample->domain_index], target) == 0)
			sample->pending_target = target;
		else
			fprintf(stderr, "Could not reclaim memory from %s\n", sample->uuid);
	}

	// Only measured host memory is spendable, not the amount just requested back.
	unsigned long long free_kib = virNodeGetFreeMemory(conn) / 1024;
	unsigned long long budget = free_kib > HOST_RESERVE_KIB ?
		free_kib - HOST_RESERVE_KIB : 0;
	for (int d = 0; d < ndomains; d++)
		if (samples[d].pending_target > samples[d].actual)
			budget -= MIN(budget, samples[d].pending_target - samples[d].actual);

	for (int d = 0; d < ndomains && budget > 0; d++)
	{
		MemorySample *sample = &samples[d];
		if (!sample->usable || sample->pending_target != 0 ||
		    sample->unused >= GROW_BELOW_KIB || sample->actual >= sample->maximum)
			continue;
		unsigned long long amount = MIN(MEMORY_STEP_KIB, sample->maximum - sample->actual);
		amount = MIN(amount, budget);
		// Round down to a 4 KiB page so an asynchronous target can be observed exactly.
		amount -= amount % 4;
		if (amount == 0)
			continue;
		unsigned long target = sample->actual + amount;
		if (virDomainSetMemory(domains[sample->domain_index], target) == 0)
		{
			sample->pending_target = target;
			budget -= amount;
		}
		else
			fprintf(stderr, "Could not give memory to %s\n", sample->uuid);
	}

	free(previous);
	previous = samples;
	previous_count = ndomains;
	samples = NULL;

cleanup:
	free(samples);
	for (int d = 0; d < ndomains; d++)
		virDomainFree(domains[d]);
	free(domains);
}
