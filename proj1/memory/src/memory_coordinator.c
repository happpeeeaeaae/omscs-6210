#include <libvirt/libvirt.h>

#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

enum {
    MEMORY_PAGE_KIB = 4,
    MEMORY_STEP_KIB = 100 * 1024,
    GUEST_MIN_UNUSED_KIB = 100 * 1024,
    GROW_BELOW_KIB = 200 * 1024,
    RECLAIM_ABOVE_KIB = 300 * 1024,
    HOST_RESERVE_KIB = 200 * 1024,
    GUEST_MAX_KIB = 2048 * 1024,
    FAILED_REQUEST_RETRY_PASSES = 3
};

typedef struct {
    unsigned long long actual_kib;
    unsigned long long unused_kib;
    unsigned long long maximum_kib;
    unsigned long long last_update;
    unsigned long long pending_target_kib;
    unsigned long long last_failed_pass;
    bool statistics_enabled;
    bool timestamp_available;
    bool has_failed_request;
    bool fresh;
} VmMemoryState;

typedef struct {
    unsigned int domain_id;
    char uuid[VIR_UUID_STRING_BUFLEN];
    virDomainPtr domain_handle;
    VmMemoryState memory;
} VmRecord;

/* Up to four guests are tested. This array-backed map uses an ID lookup and
 * verifies the UUID so a reused libvirt ID cannot inherit another VM's state. */
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
    bool debug;
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
static volatile sig_atomic_t is_exit;

void MemoryScheduler(virConnectPtr connection, int interval);

static unsigned long long smaller_value(unsigned long long first, unsigned long long second) {
    return first < second ? first : second;
}

static void initialize_memory_scheduler(MemorySchedulerState *state)
{
    const char *debug_setting = getenv("MEMORY_DEBUG");
    *state = (MemorySchedulerState) {
        .debug = debug_setting != NULL && strcmp(debug_setting, "1") == 0,
        .initialized = true
    };
}

static void memory_debug(const char *format, ...) {
    if (!scheduler_state.debug) {
        return;
    }

    va_list arguments;
    va_start(arguments, format);
    fprintf(stderr, "[memory] ");
    vfprintf(stderr, format, arguments);
    fputc('\n', stderr);
    va_end(arguments);
}

static virConnectPtr open_hypervisor_connection(void) {
    virConnectPtr connection = virConnectOpen("qemu:///system");
    if (connection == NULL)
        fprintf(stderr, "Failed to open libvirt connection\n");
    return connection;
}

static void cleanup_vm_map(VmMap *map) {
    for (size_t record_index = 0; record_index < map->count; ++record_index) {
        if (map->records[record_index].domain_handle != NULL) {
            virDomainFree(map->records[record_index].domain_handle);
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

static void cleanup_memory_scheduler(MemorySchedulerState *state) {
    cleanup_vm_map(&state->history);
    state->initialized = false;
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

    if (record->domain_id == UINT_MAX ||virDomainGetUUIDString(domain, record->uuid) < 0) {
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
        memory_debug("%s: could not enable balloon statistics", record->uuid);
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

static void update_vm_memory(VmMemoryState *memory, const MemoryReading *reading, unsigned long maximum_kib) {
    if (reading->has_actual) {
        memory->actual_kib = reading->actual_kib;
    }

    bool not_ready = !reading->has_actual || !reading->has_unused ||
        reading->actual_kib == 0 ||
        reading->unused_kib > reading->actual_kib ||
        maximum_kib == 0 ||
        (reading->has_update && (reading->update == 0 || reading->update <= memory->last_update));

    if (not_ready) {
        return;
    }

    if (reading->has_update) {
        memory->last_update = reading->update;
    }

    memory->unused_kib = reading->unused_kib;
    memory->maximum_kib = smaller_value(maximum_kib, GUEST_MAX_KIB);
    memory->timestamp_available = reading->has_update;

    if (memory->pending_target_kib != 0) {
        if (memory->actual_kib != memory->pending_target_kib) {
            return;
        }

        memory->pending_target_kib = 0;
    }

    memory->fresh = true;
}

static void read_memory_statistics(VmRecord *record) {
    virDomainInfo domain_info;
    virDomainMemoryStatStruct statistics[VIR_DOMAIN_MEMORY_STAT_NR];

    if (virDomainGetInfo(record->domain_handle, &domain_info) < 0) {
        memory_debug("%s: could not read domain info", record->uuid);
        return;
    }

    int statistic_count = virDomainMemoryStats(record->domain_handle, statistics, VIR_DOMAIN_MEMORY_STAT_NR, 0);

    if (statistic_count < 0) {
        memory_debug("%s: could not read balloon statistics", record->uuid);
        return;
    }

    MemoryReading reading = parse_memory_statistics(statistics, statistic_count);
    update_vm_memory(&record->memory, &reading, domain_info.maxMem);

    if (!record->memory.fresh) {
        memory_debug("%s: skipped sample actual=%llu unused=%llu max=%lu update=%llu "
                     "has_actual=%d has_unused=%d has_update=%d pending=%llu",
                     record->uuid, reading.actual_kib, reading.unused_kib,
                     domain_info.maxMem, reading.update, reading.has_actual,
                     reading.has_unused, reading.has_update,
                     record->memory.pending_target_kib);
    } else if (!reading.has_update) {
        memory_debug("%s: using balloon statistics without LAST_UPDATE", record->uuid);
    }
}

static bool allocate_vm_map(VmMap *snapshot, virDomainPtr **domains, virConnectPtr connection) {
    int domain_count = virConnectListAllDomains(connection, domains, VIR_CONNECT_LIST_DOMAINS_ACTIVE);
    if (domain_count < 0) {
        memory_debug("could not list active VMs");
        return false;
    }

    snapshot->count = (size_t)domain_count;
    if (snapshot->count == 0) {
        memory_debug("no active VMs");
        return true;
    }

    snapshot->records = calloc(snapshot->count, sizeof(*snapshot->records));
    if (snapshot->records != NULL) {
        return true;
    }

    for (int domain_index = 0; domain_index < domain_count; ++domain_index) {
        virDomainFree((*domains)[domain_index]);
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

        enable_memory_statistics(record, interval);
        read_memory_statistics(record);
    }

    for (size_t record_index = 0; record_index < snapshot->count; ++record_index) {
        if (domains[record_index] != NULL)
            virDomainFree(domains[record_index]);
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
        *host_free_kib = virNodeGetFreeMemory(connection) / 1024;
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
    if (!memory->fresh || memory->pending_target_kib != 0) {
        return false;
    }

    /* Without a timestamp, a failed request must not be retried every pass. */
    if (!memory->timestamp_available && memory->has_failed_request &&
        (pass_number < memory->last_failed_pass ||
         pass_number - memory->last_failed_pass < FAILED_REQUEST_RETRY_PASSES)) {
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

        unsigned long long amount = smaller_value(MEMORY_STEP_KIB, memory->unused_kib - GUEST_MIN_UNUSED_KIB);

        amount = smaller_value(amount, memory->actual_kib);
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
            budget -= smaller_value(budget, outstanding);
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
        unsigned long long amount = smaller_value(MEMORY_STEP_KIB, memory->maximum_kib - memory->actual_kib);
        amount = smaller_value(amount, budget);
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
            record->memory.has_failed_request = false;
            memory_debug("%s: %s actual=%llu target=%lu", record->uuid,
                         target->grow ? "grant" : "reclaim",
                         record->memory.actual_kib, target->target_kib);
        } else {
            record->memory.has_failed_request = true;
            record->memory.last_failed_pass = map->pass_number;
            fprintf(stderr, "Could not %s memory for VM %s\n",
                    target->grow ? "grant" : "reclaim", record->uuid);
        }
    }
}

static void save_vm_history(MemorySchedulerState *state, VmMap *snapshot) {
    for (size_t record_index = 0; record_index < snapshot->count; ++record_index) {
        virDomainFree(snapshot->records[record_index].domain_handle);
        snapshot->records[record_index].domain_handle = NULL;
    }

    cleanup_vm_map(&state->history);

    state->history = *snapshot;
    *snapshot = (VmMap){0};
}

static void debug_memory_snapshot(const VmMap *snapshot, unsigned long long host_free_kib) {
    if (!scheduler_state.debug) {
        return;
    }

    unsigned long long grant_budget_kib = available_grant_kib(snapshot, host_free_kib);
    memory_debug("pass=%llu host_free=%llu KiB grant_budget=%llu KiB",
                 snapshot->pass_number, host_free_kib, grant_budget_kib);

    for (size_t record_index = 0; record_index < snapshot->count; ++record_index) {
        const VmRecord *record = &snapshot->records[record_index];
        const VmMemoryState *memory = &record->memory;
        memory_debug("%s: actual=%llu unused=%llu max=%llu fresh=%d pending=%llu",
                     record->uuid, memory->actual_kib, memory->unused_kib,
                     memory->maximum_kib, memory->fresh, memory->pending_target_kib);

        if (memory->fresh && memory->unused_kib < GROW_BELOW_KIB &&
            memory->actual_kib >= memory->maximum_kib) {
            memory_debug("%s: allocation ceiling prevents growth", record->uuid);
        }
    }

    if (grant_budget_kib < MEMORY_PAGE_KIB) {
        memory_debug("host reserve or pending grants prevent new grants");
    }
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
        debug_memory_snapshot(&snapshot, host_free_kib);

        if (plan_target_memory(&snapshot, host_free_kib, &plan)) {
            apply_memory_targets(&snapshot, &plan);
            save_vm_history(&scheduler_state, &snapshot);
        }
    }

    cleanup_memory_plan(&plan);
    cleanup_vm_map(&snapshot);
}

static void signal_callback_handler(int signal_number) {
    (void)signal_number;
    is_exit = 1;
}

static bool parse_interval(const char *argument, int *interval) {
    char *end = NULL;
    long parsed = strtol(argument, &end, 10);
    if (argument == end || *end != '\0' || parsed <= 0 || parsed > INT_MAX)
        return false;
    *interval = (int)parsed;
    return true;
}

int main(int argument_count, char *arguments[]) {
    int interval;
    if (argument_count != 2 || !parse_interval(arguments[1], &interval)) {
        fprintf(stderr, "Usage: %s <positive interval in seconds>\n", arguments[0]);
        return EXIT_FAILURE;
    }

    virConnectPtr connection = open_hypervisor_connection();
    if (connection == NULL) {
        return EXIT_FAILURE;
    }

    initialize_memory_scheduler(&scheduler_state);
    signal(SIGINT, signal_callback_handler);
    while (!is_exit) {
        MemoryScheduler(connection, interval);
        sleep((unsigned int)interval);
    }

    cleanup_memory_scheduler(&scheduler_state);
    virConnectClose(connection);

    return EXIT_SUCCESS;
}
