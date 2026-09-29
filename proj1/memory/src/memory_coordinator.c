#include <libvirt/libvirt.h>
#include <libvirt/virterror.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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

enum {
    MEMORY_PAGE_KIB = 4,
    MEMORY_STEP_KIB = 100 * 1024,
    GUEST_MIN_UNUSED_KIB = 100 * 1024,
    GROW_BELOW_KIB = 200 * 1024,
    RECLAIM_ABOVE_KIB = 300 * 1024,
    HOST_RESERVE_KIB = 200 * 1024,
    GUEST_MAX_KIB = 2048 * 1024,
    REQUEST_RETRY_PASSES = 3
};

typedef struct {
    unsigned long long actual_kib;
    unsigned long long unused_kib;
    unsigned long long maximum_kib;
    unsigned long long last_update;
    unsigned long long pending_target_kib;
    unsigned long long last_failed_pass;
    unsigned long long last_progress_pass;
    bool statistics_enabled;
    bool timestamp_available;
    bool has_failed_request;
    bool sample_valid;
    bool fresh;
} VmMemoryState;

typedef struct {
    unsigned int domain_id;
    char uuid[VIR_UUID_STRING_BUFLEN];
    virDomainPtr domain_handle;
    VmMemoryState memory;
} VmRecord;

typedef struct {
    VmRecord *records;
    size_t count;
    unsigned long long pass_number;
} VmMap;

typedef struct {
    size_t record_index;
    unsigned long target_kib;
    bool grow;
} MemoryTarget;

typedef struct {
    MemoryTarget *targets;
    size_t count;
} MemoryPlan;

typedef struct {
    VmMap history;
    unsigned long long pass_number;
    bool initialized;
} MemorySchedulerState;

typedef struct {
    unsigned long long actual_kib;
    unsigned long long unused_kib;
    unsigned long long update;
    bool has_actual;
    bool has_unused;
    bool has_update;
} MemoryReading;

static MemorySchedulerState scheduler_state;

void MemoryScheduler(virConnectPtr connection, int interval);

static void initialize_memory_scheduler(MemorySchedulerState *state)
{
    *state = (MemorySchedulerState) {
        .initialized = true
    };
}

static void log_libvirt_failure(const char *call_name, const char *vm_uuid,
                                unsigned long long requested_kib) {
    virErrorPtr error = virGetLastError();
    const char *error_message = error != NULL && error->message != NULL ?
                                error->message : "no libvirt error detail available";
    const char *vm_prefix = vm_uuid != NULL ? " for VM " : "";
    const char *vm_identifier = vm_uuid != NULL ? vm_uuid : "";

    if (requested_kib != 0) {
        fprintf(stderr, "[memory] %s failed%s%s (target=%llu KiB): %s\n",
                call_name, vm_prefix, vm_identifier, requested_kib, error_message);
    } else {
        fprintf(stderr, "[memory] %s failed%s%s: %s\n",
                call_name, vm_prefix, vm_identifier, error_message);
    }
}

static void cleanup_vm_map(VmMap *map) {
    for (size_t record_index = 0; record_index < map->count; ++record_index) {
        if (map->records[record_index].domain_handle != NULL) {
            if (virDomainFree(map->records[record_index].domain_handle) < 0) {
                const char *vm_uuid = map->records[record_index].uuid[0] != '\0' ?
                                      map->records[record_index].uuid : NULL;
                log_libvirt_failure("virDomainFree", vm_uuid, 0);
            }
        }
    }

    free(map->records);
    *map = (VmMap){0};
}

static void cleanup_memory_plan(MemoryPlan *plan)
{
    free(plan->targets);
    *plan = (MemoryPlan){0};
}

static const VmRecord *find_vm_by_id(const VmMap *map, unsigned int domain_id) {
    for (size_t record_index = 0; record_index < map->count; ++record_index) {
        if (map->records[record_index].domain_id == domain_id) {
            return &map->records[record_index];
        }
    }

    return NULL;
}

static bool identify_vm(VmRecord *record, virDomainPtr domain, const VmMap *history) {
    record->domain_id = virDomainGetID(domain);

    if (record->domain_id == UINT_MAX) {
        log_libvirt_failure("virDomainGetID", NULL, 0);
        return false;
    }

    if (virDomainGetUUIDString(domain, record->uuid) < 0) {
        log_libvirt_failure("virDomainGetUUIDString", NULL, 0);
        return false;
    }

    const VmRecord *previous = find_vm_by_id(history, record->domain_id);

    if (previous != NULL && strcmp(previous->uuid, record->uuid) == 0) {
        record->memory = previous->memory;
    }

    return true;
}

static bool enable_memory_statistics(VmRecord *record, int interval) {
    if (record->memory.statistics_enabled) {
        return true;
    }

    if (virDomainSetMemoryStatsPeriod(record->domain_handle, interval, VIR_DOMAIN_AFFECT_LIVE) < 0) {
        log_libvirt_failure("virDomainSetMemoryStatsPeriod", record->uuid, 0);
        return false;
    }

    record->memory.statistics_enabled = true;

    return true;
}

static MemoryReading parse_memory_statistics(const virDomainMemoryStatStruct *statistics, int statistic_count) {
    MemoryReading reading = {0};

    for (int statistic_index = 0; statistic_index < statistic_count; ++statistic_index) {
        switch (statistics[statistic_index].tag) {
            case VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON: {
                reading.actual_kib = statistics[statistic_index].val;
                reading.has_actual = true;
                break;
            }

            case VIR_DOMAIN_MEMORY_STAT_UNUSED: {
                reading.unused_kib = statistics[statistic_index].val;
                reading.has_unused = true;
                break;
            }

            case VIR_DOMAIN_MEMORY_STAT_LAST_UPDATE: {
                reading.update = statistics[statistic_index].val;
                reading.has_update = true;
                break;
            }
            default: {
                break;
            }
        }
    }

    return reading;
}

static void update_vm_memory(VmMemoryState *memory, const MemoryReading *reading,
                             unsigned long maximum_kib, unsigned long long pass_number) {
    bool not_ready = !reading->has_actual || !reading->has_unused ||
        reading->actual_kib == 0 ||
        reading->unused_kib > reading->actual_kib ||
        maximum_kib == 0 ||
        (reading->has_update && (reading->update == 0 ||
         (memory->timestamp_available && reading->update < memory->last_update)));

    if (not_ready) {
        return;
    }

    memory->sample_valid = true;
    unsigned long long effective_maximum = MIN(maximum_kib, GUEST_MAX_KIB);
    unsigned long long previous_actual = memory->actual_kib;
    bool actual_changed = reading->actual_kib != memory->actual_kib;
    bool unused_changed = reading->unused_kib != memory->unused_kib;
    bool maximum_changed = effective_maximum != memory->maximum_kib;
    memory->last_update = reading->has_update ? reading->update : 0;
    memory->timestamp_available = reading->has_update;
    if (!actual_changed && !unused_changed && !maximum_changed) {
        return;
    }

    if (actual_changed) memory->actual_kib = reading->actual_kib;
    if (unused_changed) memory->unused_kib = reading->unused_kib;
    if (maximum_changed) memory->maximum_kib = effective_maximum;

    if (memory->pending_target_kib != 0) {
        unsigned long long target = memory->pending_target_kib;
        if (memory->actual_kib == target) {
            memory->pending_target_kib = 0;
        } else if ((target > previous_actual && memory->actual_kib > previous_actual) ||
                   (target < previous_actual && memory->actual_kib < previous_actual)) {
            memory->last_progress_pass = pass_number;
        }
        if (memory->pending_target_kib != 0) return;
    }

    memory->fresh = true;
}

static void read_memory_statistics(VmRecord *record, unsigned long long pass_number) {
    virDomainInfo domain_info;
    virDomainMemoryStatStruct statistics[VIR_DOMAIN_MEMORY_STAT_NR];

    if (virDomainGetInfo(record->domain_handle, &domain_info) < 0) {
        log_libvirt_failure("virDomainGetInfo", record->uuid, 0);
        return;
    }

    int statistic_count = virDomainMemoryStats(record->domain_handle, statistics, VIR_DOMAIN_MEMORY_STAT_NR, 0);

    if (statistic_count < 0) {
        log_libvirt_failure("virDomainMemoryStats", record->uuid, 0);
        return;
    }

    if (statistic_count == 0) {
        fprintf(stderr, "[memory] virDomainMemoryStats returned no statistics for VM %s\n",
                record->uuid);
    }

    MemoryReading reading = parse_memory_statistics(statistics, statistic_count);
    update_vm_memory(&record->memory, &reading, domain_info.maxMem, pass_number);
}

static bool allocate_vm_map(VmMap *snapshot, virDomainPtr **domains, virConnectPtr connection) {
    int domain_count = virConnectListAllDomains(connection, domains, VIR_CONNECT_LIST_DOMAINS_ACTIVE);
    if (domain_count < 0) {
        log_libvirt_failure("virConnectListAllDomains", NULL, 0);
        return false;
    }

    snapshot->count = (size_t)domain_count;
    if (snapshot->count == 0) {
        return true;
    }

    snapshot->records = calloc(snapshot->count, sizeof(*snapshot->records));
    if (snapshot->records != NULL) {
        return true;
    }

    for (int domain_index = 0; domain_index < domain_count; ++domain_index) {
        if (virDomainFree((*domains)[domain_index]) < 0) {
            log_libvirt_failure("virDomainFree", NULL, 0);
        }
    }

    free(*domains);
    *domains = NULL;
    snapshot->count = 0;

    return false;
}

/* Collect one complete snapshot for the current state. */
static bool read_vm_records(VmMap *snapshot, virDomainPtr *domains, const VmMap *history, int interval) {
    bool success = true;
    for (size_t record_index = 0; record_index < snapshot->count; ++record_index) {
        VmRecord *record = &snapshot->records[record_index];
        record->domain_handle = domains[record_index];
        domains[record_index] = NULL;

        if (!identify_vm(record, record->domain_handle, history)) {
            success = false;
            break;
        }

        record->memory.fresh = false;
        record->memory.sample_valid = false;

        enable_memory_statistics(record, interval);
        read_memory_statistics(record, snapshot->pass_number);
    }

    for (size_t record_index = 0; record_index < snapshot->count; ++record_index) {
        if (domains[record_index] != NULL && virDomainFree(domains[record_index]) < 0) {
            log_libvirt_failure("virDomainFree", NULL, 0);
        }
    }

    return success;
}

static bool fetch_vm_states(virConnectPtr connection, int interval, const VmMap *history, VmMap *snapshot,
                            unsigned long long *host_free_kib) {
    virDomainPtr *domains = NULL;

    if (!allocate_vm_map(snapshot, &domains, connection)) {
        return false;
    }

    bool success = read_vm_records(snapshot, domains, history, interval);
    free(domains);

    if (success) {
        virResetLastError();
        unsigned long long host_free_bytes = virNodeGetFreeMemory(connection);
        if (host_free_bytes == 0) {
            if (virGetLastError() != NULL) {
                log_libvirt_failure("virNodeGetFreeMemory", NULL, 0);
            } else {
                fputs("[memory] virNodeGetFreeMemory returned 0 bytes of free host memory\n", stderr);
            }
        }
        *host_free_kib = host_free_bytes / 1024;
    }

    return success;
}

static bool target_exists(const MemoryPlan *plan, size_t record_index) {
    for (size_t target_index = 0; target_index < plan->count; ++target_index) {
        if (plan->targets[target_index].record_index == record_index) {
            return true;
        }
    }

    return false;
}

static bool vm_can_change(const VmRecord *record, bool donor, unsigned long long pass_number) {
    const VmMemoryState *memory = &record->memory;
    if (!memory->sample_valid || memory->pending_target_kib != 0) {
        return false;
    }

    bool retry_due = memory->has_failed_request &&
                     pass_number >= memory->last_failed_pass &&
                     pass_number - memory->last_failed_pass >= REQUEST_RETRY_PASSES;
    if (memory->has_failed_request && !memory->fresh && !retry_due) {
        return false;
    }

    /* Without a timestamp, a failed request must not be retried every pass. */
    if (memory->has_failed_request && !memory->timestamp_available && !retry_due) {
        return false;
    }

    if (donor) {
        return memory->unused_kib > RECLAIM_ABOVE_KIB;
    }

    return memory->unused_kib < GROW_BELOW_KIB && memory->actual_kib < memory->maximum_kib;
}

/* Repeated selection is clear and cheap for the small VM set. */
static size_t select_next_vm(const VmMap *map, const MemoryPlan *plan, bool donor) {
    size_t selected = map->count;
    for (size_t record_index = 0; record_index < map->count; ++record_index) {
        const VmRecord *candidate = &map->records[record_index];
        if (!vm_can_change(candidate, donor, map->pass_number) ||target_exists(plan, record_index)) {
                continue;
            }

        unsigned long long unused_kib = candidate->memory.unused_kib;


        if (selected == map->count ||(donor ? unused_kib > map->records[selected].memory.unused_kib : unused_kib < map->records[selected].memory.unused_kib))
            selected = record_index;
    }

    return selected;
}

static void plan_reclamations(const VmMap *map, MemoryPlan *plan) {
    for (size_t attempt = 0; attempt < map->count; ++attempt) {
        size_t record_index = select_next_vm(map, plan, true);
        if (record_index == map->count) {
            return;
        }

        const VmMemoryState *memory = &map->records[record_index].memory;

        unsigned long long amount = MIN(MEMORY_STEP_KIB, memory->unused_kib - GUEST_MIN_UNUSED_KIB);

        amount = MIN(amount, memory->actual_kib);
        amount -= amount % MEMORY_PAGE_KIB;
        if (amount == 0) {
            return;
        }

        plan->targets[plan->count++] = (MemoryTarget){
            .record_index = record_index,
            .target_kib = (unsigned long)(memory->actual_kib - amount),
            .grow = false
        };
    }
}

static unsigned long long available_grant_kib(const VmMap *map, unsigned long long host_free_kib) {
    unsigned long long budget = host_free_kib > HOST_RESERVE_KIB ? host_free_kib - HOST_RESERVE_KIB : 0;

    for (size_t record_index = 0; record_index < map->count; ++record_index) {
        const VmMemoryState *memory = &map->records[record_index].memory;
        if (memory->pending_target_kib > memory->actual_kib) {
            unsigned long long outstanding = memory->pending_target_kib -
                                             memory->actual_kib;
            budget -= MIN(budget, outstanding);
        }
    }

    return budget;
}

static void plan_grants(const VmMap *map, unsigned long long budget, MemoryPlan *plan) {
    for (size_t attempt = 0; attempt < map->count && budget >= MEMORY_PAGE_KIB; ++attempt) {
        size_t record_index = select_next_vm(map, plan, false);
        if (record_index == map->count) {
            return;
        }

        const VmMemoryState *memory = &map->records[record_index].memory;
        unsigned long long amount = MIN(MEMORY_STEP_KIB, memory->maximum_kib - memory->actual_kib);
        amount = MIN(amount, budget);
        amount -= amount % MEMORY_PAGE_KIB;

        if (amount == 0) {
            return;
        }

        plan->targets[plan->count++] = (MemoryTarget){
            .record_index = record_index,
            .target_kib = (unsigned long)(memory->actual_kib + amount),
            .grow = true
        };

        budget -= amount;
    }
}

/* The plan depends only on the supplied snapshot and host free memory.
 * Reclamation is not spendable until the host reports that memory free. */
static bool plan_target_memory(const VmMap *map, unsigned long long host_free_kib, MemoryPlan *plan) {
    *plan = (MemoryPlan){0};

    if (map->count == 0) {
        return true;
    }

    plan->targets = calloc(map->count, sizeof(*plan->targets));
    if (plan->targets == NULL) {
        return false;
    }

    plan_reclamations(map, plan);
    plan_grants(map, available_grant_kib(map, host_free_kib), plan);

    return true;
}

static void apply_memory_targets(VmMap *map, const MemoryPlan *plan) {
    for (size_t target_index = 0; target_index < plan->count; ++target_index) {
        const MemoryTarget *target = &plan->targets[target_index];
        VmRecord *record = &map->records[target->record_index];

        if (target->target_kib == record->memory.actual_kib ||
            record->memory.pending_target_kib != 0) {
            continue;
        }

        if (virDomainSetMemory(record->domain_handle, target->target_kib) == 0) {
            record->memory.pending_target_kib = target->target_kib;
            record->memory.last_progress_pass = map->pass_number;
            record->memory.has_failed_request = false;
        } else {
            record->memory.has_failed_request = true;
            record->memory.last_failed_pass = map->pass_number;
            log_libvirt_failure("virDomainSetMemory", record->uuid, target->target_kib);
        }
    }
}

static void retry_stalled_requests(VmMap *map) {
    for (size_t record_index = 0; record_index < map->count; ++record_index) {
        VmRecord *record = &map->records[record_index];
        VmMemoryState *memory = &record->memory;
        
        if (!memory->sample_valid || memory->pending_target_kib == 0 ||
            map->pass_number < memory->last_progress_pass ||
            map->pass_number - memory->last_progress_pass < REQUEST_RETRY_PASSES) {
            continue;
        }

        unsigned long long target = memory->pending_target_kib;
        if (virDomainSetMemory(record->domain_handle, (unsigned long)target) < 0) {
            log_libvirt_failure("virDomainSetMemory", record->uuid, target);
        }
        
        memory->last_progress_pass = map->pass_number;
    }
}

static void save_vm_history(MemorySchedulerState *state, VmMap *snapshot) {
    for (size_t record_index = 0; record_index < snapshot->count; ++record_index) {
        if (virDomainFree(snapshot->records[record_index].domain_handle) < 0) {
            log_libvirt_failure("virDomainFree", snapshot->records[record_index].uuid, 0);
        }
        snapshot->records[record_index].domain_handle = NULL;
    }

    cleanup_vm_map(&state->history);

    state->history = *snapshot;
    *snapshot = (VmMap){0};
}

void MemoryScheduler(virConnectPtr connection, int interval) {
    if (connection == NULL || interval <= 0) {
        return;
    }

    if (!scheduler_state.initialized) {
        initialize_memory_scheduler(&scheduler_state);
    }

    VmMap snapshot = {.pass_number = ++scheduler_state.pass_number};
    MemoryPlan plan = {0};
    unsigned long long host_free_kib = 0;

    if (fetch_vm_states(connection, interval, &scheduler_state.history, &snapshot, &host_free_kib)) {

        if (plan_target_memory(&snapshot, host_free_kib, &plan)) {
            retry_stalled_requests(&snapshot);
            apply_memory_targets(&snapshot, &plan);
            save_vm_history(&scheduler_state, &snapshot);
        }
    }

    cleanup_memory_plan(&plan);
    cleanup_vm_map(&snapshot);
}
