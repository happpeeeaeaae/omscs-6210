// In-memory libvirt implementation shared by the two independent test programs.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libvirt/libvirt.h"
#include "libvirt/virterror.h"
#define KIB (1024ULL)
struct FakeDomain {
    char uuid[VIR_UUID_STRING_BUFLEN]; unsigned int id; int nv;
    virVcpuInfo v[4]; unsigned char map[4][32]; double rate[4];
    unsigned long actual, unused, maximum, target;
    unsigned long long updated;
    int fail_stats, fail_stats_period, fail_pin, fail_set, fail_uuid;
    int missing, omit_last_update, pin_calls, set_calls;
};
struct FakeDomain guests[16];
int guest_count, cpu_count, pins, sets, refs, frees, last_maplen;
unsigned char host_online[32];
unsigned long long host_free;
double mock_now = 100.0;
int mock_clock_gettime(clockid_t clock, struct timespec *time) {
    time->tv_sec = (long)mock_now;
    time->tv_nsec = (long)((mock_now - time->tv_sec) * 1e9);
    return 0;
}
void init_fixture(int ng, int nc, int nv) {
    memset(guests, 0, sizeof(guests)); memset(host_online, 0, sizeof(host_online));
    guest_count = ng; cpu_count = nc; pins = sets = refs = frees = last_maplen = 0;
    host_free = 4096 * KIB;
    for (int c = 0; c < nc; c++) VIR_USE_CPU(host_online, c);
    for (int d = 0; d < ng; d++) {
        struct FakeDomain *g = &guests[d];
        snprintf(g->uuid, sizeof(g->uuid), "guest-%02d", d); g->id = 100 + d; g->nv = nv;
        g->actual = 512 * KIB; g->unused = 250 * KIB; g->maximum = 2048 * KIB; g->updated = 1;
        for (int v = 0; v < nv; v++) {
            g->v[v].number = v; g->v[v].cpu = 0; g->rate[v] = 25;
            VIR_USE_CPU(g->map[v], 0);
        }
    }
}
void tick(double seconds) {
    mock_now += seconds;
    for (int d = 0; d < guest_count; d++) {
        guests[d].updated++;
        for (int v = 0; v < guests[d].nv; v++)
            guests[d].v[v].cpuTime += (unsigned long long)(guests[d].rate[v] / 100.0 * seconds * 1e9);
    }
}

// Feedback model: CPU time depends on the latest pinning and requested work.
// A busy pCPU shares its 100% capacity proportionally between resident vCPUs.
void simulate_cpu(double seconds) {
    double demand[256] = {0};
    for (int d = 0; d < guest_count; d++)
        for (int v = 0; v < guests[d].nv; v++)
            demand[guests[d].v[v].cpu] += guests[d].rate[v];
    mock_now += seconds;
    for (int d = 0; d < guest_count; d++)
        for (int v = 0; v < guests[d].nv; v++) {
            double total = demand[guests[d].v[v].cpu];
            double usage = guests[d].rate[v];
            if (total > 100.0) usage *= 100.0 / total;
            guests[d].v[v].cpuTime += (unsigned long long)(usage / 100.0 * seconds * 1e9);
        }
}
void map_to(int d, int v, int cpu) {
    memset(guests[d].map[v], 0, sizeof(guests[d].map[v]));
    VIR_USE_CPU(guests[d].map[v], cpu); guests[d].v[v].cpu = cpu;
}
void settle_memory(void) {
    for (int d = 0; d < guest_count; d++) {
        struct FakeDomain *g = &guests[d];
        if (g->target) {
            long long delta = (long long)g->target - g->actual;
            assert(delta <= 0 || host_free >= (unsigned long long)delta);
            assert((long long)g->unused + delta >= 0);
            host_free -= delta; g->unused += delta; g->actual = g->target; g->target = 0;
        }
    }
}
virConnectPtr virConnectOpen(const char *uri) { return (void *)1; }
virErrorPtr virGetLastError(void) { return NULL; }
void virResetLastError(void) { }
int virConnectClose(virConnectPtr conn) { return 0; }
int virConnectListAllDomains(virConnectPtr conn, virDomainPtr **domains, unsigned int flags) {
    *domains = calloc(guest_count + 1, sizeof(**domains));
    for (int d = 0; d < guest_count; d++) (*domains)[d] = &guests[d];
    refs += guest_count; return guest_count;
}
int virNodeGetCPUMap(virConnectPtr conn, unsigned char **map, unsigned int *online, unsigned int flags) {
    *map = calloc(VIR_CPU_MAPLEN(cpu_count), 1); memcpy(*map, host_online, VIR_CPU_MAPLEN(cpu_count));
    *online = 0; for (int c = 0; c < cpu_count; c++) if (VIR_CPU_USED(host_online, c)) (*online)++;
    return cpu_count;
}
unsigned int virDomainGetID(virDomainPtr dom) { return dom->id; }
int virDomainGetUUIDString(virDomainPtr dom, char *uuid) { if (dom->fail_uuid) return -1; strcpy(uuid, dom->uuid); return 0; }
int virDomainGetInfo(virDomainPtr dom, virDomainInfo *info) {
    if (dom->fail_stats) return -1;
    memset(info, 0, sizeof(*info)); info->nrVirtCpu = dom->nv; info->memory = dom->actual; info->maxMem = dom->maximum;
    return 0;
}
int virDomainGetVcpus(virDomainPtr dom, virVcpuInfoPtr info, int maxinfo, unsigned char *maps, int maplen) {
    if (dom->fail_stats) return -1;
    assert(maxinfo >= dom->nv);
    for (int v = 0; v < dom->nv; v++) { info[v] = dom->v[v]; memcpy(maps + v * maplen, dom->map[v], maplen); }
    return dom->nv;
}
int virDomainPinVcpu(virDomainPtr dom, unsigned int vcpu, unsigned char *map, int maplen) {
    pins++; dom->pin_calls++; last_maplen = maplen;
    if (dom->fail_pin) return -1;
    int chosen = -1;
    for (int c = 0; c < cpu_count; c++) if (VIR_CPU_USED(map, c)) { assert(chosen == -1); assert(VIR_CPU_USED(host_online, c)); chosen = c; }
    assert(chosen != -1); memcpy(dom->map[vcpu], map, maplen); dom->v[vcpu].cpu = chosen;
    return 0;
}
int virDomainFree(virDomainPtr dom) { frees++; return 0; }
int virDomainSetMemoryStatsPeriod(virDomainPtr dom, int period, unsigned int flags) {
    assert(period > 0);
    return dom->fail_stats_period ? -1 : 0;
}
int virDomainMemoryStats(virDomainPtr dom, virDomainMemoryStatStruct *stats, unsigned int nstats, unsigned int flags) {
    if (dom->fail_stats) return -1;
    int statistic_count = 0;
    if (!dom->omit_last_update)
        stats[statistic_count++] = (virDomainMemoryStatStruct){VIR_DOMAIN_MEMORY_STAT_LAST_UPDATE, dom->updated};
    stats[statistic_count++] = (virDomainMemoryStatStruct){VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON, dom->actual};
    if (!dom->missing)
        stats[statistic_count++] = (virDomainMemoryStatStruct){VIR_DOMAIN_MEMORY_STAT_UNUSED, dom->unused};
    return statistic_count;
}
int virDomainSetMemory(virDomainPtr dom, unsigned long target) {
    sets++; dom->set_calls++;
    assert(target != dom->actual); // Every request must change the observed allocation.
    if (dom->fail_set) return -1;
    assert(dom->target == 0); // Never overwrite a balloon request still in flight.
    assert(target <= dom->maximum); assert(target <= 2048 * KIB);
    long long delta = (long long)target - dom->actual;
    assert(llabs(delta) <= 100 * KIB);
    if (delta < 0) assert((long long)dom->unused + delta >= 100 * KIB);
    dom->target = target; return 0;
}
unsigned long long virNodeGetFreeMemory(virConnectPtr conn) { return host_free * 1024; }
