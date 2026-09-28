#include <stdio.h>
#include <stdlib.h>
#include <libvirt/libvirt.h>
#include <math.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
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

typedef struct
{
	int stats_ok;
	int have_actual;
	int have_unused;
	int have_updated;
	unsigned long long actual;
	unsigned long long unused;
	unsigned long long updated;
	unsigned long long maximum;
} MemoryReading;

typedef struct
{
	virDomainPtr *domains;
	int ndomains;
	MemorySample *samples;
	MemoryReading *readings;
	unsigned long long host_free_kib;
	int debug;
} MemorySnapshot;

typedef struct
{
	int sample_index;
	unsigned long target;
	int grow;
} MemoryAction;

static void memory_debug(int enabled, const char *format, ...)
{
	if (!enabled)
		return;
	va_list args;
	va_start(args, format);
	fprintf(stderr, "[memory] ");
	vfprintf(stderr, format, args);
	fputc('\n', stderr);
	va_end(args);
}

static int compare_unused_memory(const void *left, const void *right)
{
	const MemorySample *a = left, *b = right;
	if (a->unused != b->unused)
		return a->unused < b->unused ? -1 : 1;
	return strcmp(a->uuid, b->uuid);
}

/* Collect one host and guest snapshot, retaining history only for polling setup. */
static int read_memory_snapshot(virConnectPtr conn, int interval,
				const MemorySample *previous, int previous_count,
				MemorySnapshot *snapshot)
{
	snapshot->ndomains = virConnectListAllDomains(conn, &snapshot->domains,
						      VIR_CONNECT_LIST_DOMAINS_ACTIVE);
	if (snapshot->ndomains < 0)
	{
		memory_debug(snapshot->debug, "cannot list active domains: result=%d",
			     snapshot->ndomains);
		return -1;
	}
	if (snapshot->ndomains == 0)
	{
		memory_debug(snapshot->debug, "no active domains");
		return 0;
	}
	snapshot->samples = calloc(snapshot->ndomains, sizeof(*snapshot->samples));
	snapshot->readings = calloc(snapshot->ndomains, sizeof(*snapshot->readings));
	if (!snapshot->samples || !snapshot->readings)
		return -1;

	for (int d = 0; d < snapshot->ndomains; d++)
	{
		MemorySample *sample = &snapshot->samples[d];
		MemoryReading *reading = &snapshot->readings[d];
		char uuid[VIR_UUID_STRING_BUFLEN];
		unsigned int id = virDomainGetID(snapshot->domains[d]);
		if (id == UINT_MAX || virDomainGetUUIDString(snapshot->domains[d], uuid) < 0)
		{
			// Keep old history: this guest may still have a grant in flight.
			memory_debug(snapshot->debug,
				     "cannot identify domain index=%d; skipping pass and keeping history", d);
			return -1;
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
			int result = virDomainSetMemoryStatsPeriod(snapshot->domains[d],
								  interval, VIR_DOMAIN_AFFECT_LIVE);
			memory_debug(snapshot->debug, "%s: stats period=%d result=%d",
				     sample->uuid, interval, result);
			if (result < 0)
				continue;
			sample->stats_period = interval;
		}
		virDomainInfo info;
		virDomainMemoryStatStruct stats[VIR_DOMAIN_MEMORY_STAT_NR];
		if (virDomainGetInfo(snapshot->domains[d], &info) < 0)
		{
			memory_debug(snapshot->debug, "%s: cannot read domain info", sample->uuid);
			continue;
		}
		int nstats = virDomainMemoryStats(snapshot->domains[d], stats,
						  VIR_DOMAIN_MEMORY_STAT_NR, 0);
		if (nstats < 0)
		{
			memory_debug(snapshot->debug, "%s: cannot read memory stats: result=%d",
				     sample->uuid, nstats);
			continue;
		}
		reading->stats_ok = 1;
		reading->maximum = info.maxMem;
		for (int s = 0; s < nstats; s++)
		{
			if (stats[s].tag == VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON)
			{
				reading->actual = stats[s].val;
				reading->have_actual = 1;
			}
			else if (stats[s].tag == VIR_DOMAIN_MEMORY_STAT_UNUSED)
			{
				reading->unused = stats[s].val;
				reading->have_unused = 1;
			}
			else if (stats[s].tag == VIR_DOMAIN_MEMORY_STAT_LAST_UPDATE)
			{
				reading->updated = stats[s].val;
				reading->have_updated = 1;
			}
		}
	}
	snapshot->host_free_kib = virNodeGetFreeMemory(conn) / 1024;
	return 1;
}

/* Select complete targets from measured memory, without making libvirt calls. */
static int plan_memory_actions(MemorySnapshot *snapshot,
			       MemoryAction **actions, int *action_count)
{
	*actions = calloc(snapshot->ndomains, sizeof(**actions));
	if (!*actions)
		return -1;
	for (int d = 0; d < snapshot->ndomains; d++)
	{
		MemorySample *sample = &snapshot->samples[d];
		const MemoryReading *reading = &snapshot->readings[d];
		if (!reading->stats_ok)
			continue;
		if (reading->have_actual)
			sample->actual = reading->actual;
		memory_debug(snapshot->debug,
			     "%s: stats actual_kib=%llu unused_kib=%llu last_update=%llu "
			     "previous_update=%llu pending_target_kib=%llu",
			     sample->uuid, reading->actual, reading->unused, reading->updated,
			     sample->last_update, sample->pending_target);
		if (!reading->have_actual || !reading->have_unused || reading->actual == 0 ||
		    reading->unused > reading->actual || reading->updated == 0 ||
		    reading->maximum == 0)
		{
			memory_debug(snapshot->debug,
				     "%s: skipping missing/invalid stats: has_actual=%d has_unused=%d "
				     "has_last_update=%d max_kib=%llu", sample->uuid,
				     reading->have_actual, reading->have_unused,
				     reading->have_updated, reading->maximum);
			continue;
		}
		if (reading->updated <= sample->last_update)
		{
			memory_debug(snapshot->debug, "%s: skipping stale stats", sample->uuid);
			continue;
		}
		sample->last_update = reading->updated;
		sample->unused = reading->unused;
		sample->maximum = MIN(reading->maximum, GUEST_MAX_KIB);

		// Balloon changes are asynchronous; don't stack requests on old samples.
		if (sample->pending_target != 0)
		{
			if (sample->actual != sample->pending_target)
			{
				memory_debug(snapshot->debug,
					     "%s: waiting for pending target_kib=%llu; observed actual_kib=%llu",
					     sample->uuid, sample->pending_target, sample->actual);
				continue;
			}
			memory_debug(snapshot->debug, "%s: completed pending target_kib=%llu",
				     sample->uuid, sample->pending_target);
			sample->pending_target = 0;
		}
		sample->usable = 1;
	}

	qsort(snapshot->samples, snapshot->ndomains, sizeof(*snapshot->samples),
	      compare_unused_memory);
	for (int d = 0; d < snapshot->ndomains; d++)
	{
		MemorySample *sample = &snapshot->samples[d];
		if (!sample->usable || sample->unused <= RECLAIM_ABOVE_KIB)
			continue;
		unsigned long long amount = MIN(MEMORY_STEP_KIB,
						sample->unused - GUEST_MIN_UNUSED_KIB);
		unsigned long target = sample->actual - amount;
		if (target == sample->actual || sample->pending_target != 0)
			continue;
		(*actions)[*action_count] = (MemoryAction){d, target, 0};
		(*action_count)++;
	}

	// Only measured host memory is spendable; planned reclamation adds no credit.
	unsigned long long budget = snapshot->host_free_kib > HOST_RESERVE_KIB ?
		snapshot->host_free_kib - HOST_RESERVE_KIB : 0;
	for (int d = 0; d < snapshot->ndomains; d++)
		if (snapshot->samples[d].pending_target > snapshot->samples[d].actual)
			budget -= MIN(budget, snapshot->samples[d].pending_target -
					    snapshot->samples[d].actual);
	memory_debug(snapshot->debug, "host free_kib=%llu reserve_kib=%lu grant_budget_kib=%llu "
		     "after pending growth reservations",
		     snapshot->host_free_kib, HOST_RESERVE_KIB, budget);

	for (int d = 0; d < snapshot->ndomains && budget > 0; d++)
	{
		MemorySample *sample = &snapshot->samples[d];
		if (!sample->usable || sample->pending_target != 0 ||
		    sample->unused >= GROW_BELOW_KIB || sample->actual >= sample->maximum)
		{
			if (sample->usable && sample->unused < GROW_BELOW_KIB &&
			    sample->actual >= sample->maximum)
				memory_debug(snapshot->debug,
					     "%s: at allocation ceiling actual_kib=%llu maximum_kib=%llu",
					     sample->uuid, sample->actual, sample->maximum);
			continue;
		}
		unsigned long long amount = MIN(MEMORY_STEP_KIB,
						sample->maximum - sample->actual);
		amount = MIN(amount, budget);
		// Round down to a 4 KiB page so an asynchronous target can be observed exactly.
		amount -= amount % 4;
		if (amount == 0)
		{
			memory_debug(snapshot->debug,
				     "%s: grant below one page; remaining_budget_kib=%llu",
				     sample->uuid, budget);
			continue;
		}
		unsigned long target = sample->actual + amount;
		if (target == sample->actual || sample->pending_target != 0)
			continue;
		(*actions)[*action_count] = (MemoryAction){d, target, 1};
		(*action_count)++;
		budget -= amount;
	}
	memory_debug(snapshot->debug, "host planned_remaining_budget_kib=%llu", budget);
	return 0;
}

/* Record a pending target only when libvirt accepts the planned request. */
static void apply_memory_actions(MemorySnapshot *snapshot,
				  const MemoryAction *actions, int action_count)
{
	for (int a = 0; a < action_count; a++)
	{
		const MemoryAction *action = &actions[a];
		MemorySample *sample = &snapshot->samples[action->sample_index];
		if (action->target == sample->actual || sample->pending_target != 0)
			continue;
		int result = virDomainSetMemory(snapshot->domains[sample->domain_index],
						action->target);
		memory_debug(snapshot->debug, "%s: %s actual_kib=%llu target_kib=%lu result=%d",
			     sample->uuid, action->grow ? "grow" : "reclaim",
			     sample->actual, action->target, result);
		if (result == 0)
			sample->pending_target = action->target;
		else if (action->grow)
			fprintf(stderr, "Could not give memory to %s\n", sample->uuid);
		else
			fprintf(stderr, "Could not reclaim memory from %s\n", sample->uuid);
	}
}

/* Reclaim spare memory first, then give small increments to the hungriest VMs. */
void MemoryScheduler(virConnectPtr conn, int interval)
{
	static MemorySample *previous = NULL;
	static int previous_count = 0;
	const char *debug_env = getenv("MEMORY_DEBUG");
	MemorySnapshot snapshot = {.debug = debug_env != NULL && !strcmp(debug_env, "1")};
	MemoryAction *actions = NULL;
	int action_count = 0;

	if (interval <= 0)
		return;
	int status = read_memory_snapshot(conn, interval, previous, previous_count, &snapshot);
	if (status == 0)
	{
		free(previous);
		previous = NULL;
		previous_count = 0;
	}
	if (status > 0 && plan_memory_actions(&snapshot, &actions, &action_count) == 0)
	{
		apply_memory_actions(&snapshot, actions, action_count);
		free(previous);
		previous = snapshot.samples;
		previous_count = snapshot.ndomains;
		snapshot.samples = NULL;
	}

	free(actions);
	free(snapshot.readings);
	free(snapshot.samples);
	for (int d = 0; d < snapshot.ndomains; d++)
		virDomainFree(snapshot.domains[d]);
	free(snapshot.domains);
}
