// Minimal libvirt API surface used by the schedulers; no real libvirt needed.

#ifndef MOCK_LIBVIRT_H
#define MOCK_LIBVIRT_H
#include <stddef.h>
#define VIR_UUID_STRING_BUFLEN 37
#define VIR_CONNECT_LIST_DOMAINS_ACTIVE 1
#define VIR_DOMAIN_AFFECT_LIVE 1
#define VIR_CPU_MAPLEN(n) (((n) + 7) / 8)
#define VIR_CPU_USED(m, c) ((m)[(c) / 8] & (1U << ((c) % 8)))
#define VIR_USE_CPU(m, c) ((m)[(c) / 8] |= (1U << ((c) % 8)))
#define VIR_DOMAIN_MEMORY_STAT_NR 13
#define VIR_DOMAIN_MEMORY_STAT_UNUSED 4
#define VIR_DOMAIN_MEMORY_STAT_ACTUAL_BALLOON 6
#define VIR_DOMAIN_MEMORY_STAT_LAST_UPDATE 9
typedef void *virConnectPtr;
typedef struct FakeDomain *virDomainPtr;
typedef struct { unsigned char state; unsigned long maxMem, memory; unsigned short nrVirtCpu; unsigned long long cpuTime; } virDomainInfo;
typedef struct { unsigned int number; int state; unsigned long long cpuTime; int cpu; } virVcpuInfo, *virVcpuInfoPtr;
typedef struct { int tag; unsigned long long val; } virDomainMemoryStatStruct;
virConnectPtr virConnectOpen(const char *uri);
int virConnectClose(virConnectPtr conn);
int virConnectListAllDomains(virConnectPtr conn, virDomainPtr **domains, unsigned int flags);
int virNodeGetCPUMap(virConnectPtr conn, unsigned char **map, unsigned int *online, unsigned int flags);
unsigned int virDomainGetID(virDomainPtr dom);
int virDomainGetUUIDString(virDomainPtr dom, char *uuid);
int virDomainGetInfo(virDomainPtr dom, virDomainInfo *info);
int virDomainGetVcpus(virDomainPtr dom, virVcpuInfoPtr info, int maxinfo, unsigned char *maps, int maplen);
int virDomainPinVcpu(virDomainPtr dom, unsigned int vcpu, unsigned char *map, int maplen);
int virDomainFree(virDomainPtr dom);
int virDomainSetMemoryStatsPeriod(virDomainPtr dom, int period, unsigned int flags);
int virDomainMemoryStats(virDomainPtr dom, virDomainMemoryStatStruct *stats, unsigned int nstats, unsigned int flags);
int virDomainSetMemory(virDomainPtr dom, unsigned long target);
unsigned long long virNodeGetFreeMemory(virConnectPtr conn);
#endif
