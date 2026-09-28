
#include "mock.h"
#define main original_main
#define clock_gettime mock_clock_gettime
#include "../cpu/src/vcpu_scheduler.c"
#undef main
#undef clock_gettime
void run(void) { CPUScheduler((void *)1, 2); assert(refs == frees); }
void reset(int ng, int nc, int nv) { guest_count = 0; run(); init_fixture(ng, nc, nv); }
void check_counts(int each) {
    int counts[256] = {0};
    for (int d = 0; d < guest_count; d++) for (int v = 0; v < guests[d].nv; v++) counts[guests[d].v[v].cpu]++;
    for (int c = 0; c < cpu_count; c++) if (VIR_CPU_USED(host_online, c)) assert(counts[c] == each);
}

void test_cpu_plan_actions(void) {
    reset(2, 2, 1);
    CpuVmState current_vms[2] = {0}, previous_vms[2] = {0};
    CpuVcpuState current_vcpus[2] = {0}, previous_vcpus[2] = {0};
    int online_cpus[2] = {0, 1};
    for (int d = 0; d < 2; d++) {
        strcpy(current_vms[d].uuid, guests[d].uuid);
        current_vms[d].domain_id = guests[d].id;
        current_vms[d].vcpu_count = 1;
        current_vms[d].vcpus = &current_vcpus[d];
        current_vcpus[d].cpu_time_ns = 1000000000ULL;
        current_vcpus[d].current_cpu = 0;
        current_vcpus[d].has_single_online_pin = true;
        previous_vms[d] = current_vms[d];
        previous_vms[d].vcpus = &previous_vcpus[d];
    }
    CpuSnapshot current = {.vms = current_vms, .vm_count = 2,
                           .online_cpus = online_cpus, .online_cpu_count = 2,
                           .sampled_at = {.tv_sec = 2}};
    CpuSnapshot previous = {.vms = previous_vms, .vm_count = 2,
                            .sampled_at = {.tv_sec = 0}};
    CpuPlan plan = {0};
    assert(plan_cpu_pins(&current, &previous, &plan) == 1);
    assert(pins == 0 && plan.target_count == 2 && plan.change_count == 1);
    assert(plan.changes[0].target_cpu == 1);
    assert(!strcmp(plan.changes[0].uuid, guests[1].uuid));
    release_plan(&plan);
    puts("CPU: pure plan identifies the vCPU to move and its target pCPU");
}

// Check observable distribution, without using the scheduler's own load helper.
void check_simulated_distribution(const char *name) {
    double demand[4] = {0};
    for (int d = 0; d < guest_count; d++)
        demand[guests[d].v[0].cpu] += guests[d].rate[0];
    printf("CPU simulation (%s): demand per pCPU [", name);
    for (int c = 0; c < 4; c++) {
        assert(demand[c] == demand[0]);
        printf("%s%.0f%%", c ? ", " : "", demand[c]);
    }
    printf("], pin changes %d\n", pins);
}

void simulate_balancing(void) {
    reset(8, 4, 1);
    for (int d = 0; d < 8; d++) guests[d].rate[0] = 75;
    run();
    for (int step = 0; step < 15; step++) { simulate_cpu(2); run(); }
    check_counts(2);
    int stable_pins = pins;
    for (int step = 0; step < 10; step++) { simulate_cpu(2); run(); }
    assert(pins == stable_pins);
    check_simulated_distribution("all on CPU 0; 25 rounds");

    reset(8, 4, 1);
    for (int d = 0; d < 8; d++) {
        guests[d].rate[0] = d < 4 ? 80 : 20;
        map_to(d, 0, d < 4 ? d / 2 : 2);
    }
    run();
    for (int step = 0; step < 15; step++) { simulate_cpu(2); run(); }
    check_counts(2);
    check_simulated_distribution("heavy/light workload");
    stable_pins = pins;
    for (int step = 0; step < 10; step++) { simulate_cpu(2); run(); }
    assert(pins == stable_pins);

    // Redistribute demand so the previously balanced pins are no longer good.
    for (int d = 0; d < 8; d++)
        guests[d].rate[0] = guests[d].v[0].cpu < 2 ? 90 : 10;
    for (int step = 0; step < 15; step++) { simulate_cpu(2); run(); }
    assert(pins > stable_pins);
    check_counts(2);
    check_simulated_distribution("workload changes mid-run");
    stable_pins = pins;
    for (int step = 0; step < 10; step++) { simulate_cpu(2); run(); }
    assert(pins == stable_pins);
}

int main(void) {
    setbuf(stdout, NULL);
    test_cpu_plan_actions();
    reset(8, 4, 1);
    for (int d = 0; d < 8; d++) map_to(d, 0, d % 4);
    run(); assert(pins == 0); tick(2); run(); assert(pins == 0);
    puts("CPU: warmup and balanced pins preserved");

    reset(8, 4, 1);
    for (int d = 0; d < 8; d++) map_to(d, 0, d / 2);
    memcpy(guests[0].map[0], host_online, VIR_CPU_MAPLEN(cpu_count));
    run(); tick(2); run(); check_counts(2); assert(pins == 1);
    tick(2); run(); assert(pins == 1);
    puts("CPU: incomplete affinity fixed without moving balanced guests");

    reset(8, 4, 1); run(); tick(2); run(); check_counts(2); assert(pins == 6);
    int before = pins; tick(2); run(); assert(pins == before);
    puts("CPU: concentrated equal workloads balanced and stable");

    reset(8, 4, 1);
    for (int d = 0; d < 8; d++) { guests[d].rate[0] = d < 4 ? 100 : 50; map_to(d, 0, d < 4 ? 0 : 1); }
    run(); tick(2); run(); check_counts(2);
    double loads[4] = {0};
    for (int d = 0; d < 8; d++) loads[guests[d].v[0].cpu] += guests[d].rate[0];
    for (int c = 0; c < 4; c++) assert(loads[c] == 150);
    before = pins; struct FakeDomain tmp = guests[0]; guests[0] = guests[7]; guests[7] = tmp;
    tick(2); run(); assert(pins == before);
    puts("CPU: mixed workloads balanced; domain enumeration order irrelevant");

    reset(2, 4, 2); run(); tick(2); run(); check_counts(1);
    reset(4, 10, 1); memset(host_online, 0, sizeof(host_online)); VIR_USE_CPU(host_online, 0); VIR_USE_CPU(host_online, 9);
    run(); tick(2); run(); check_counts(2); assert(last_maplen == 2);
    puts("CPU: multiple vCPUs and sparse multi-byte CPU masks handled");

    reset(4, 4, 1);
    for (int d = 0; d < 4; d++) { memcpy(guests[d].map[0], host_online, 1); guests[d].v[0].cpu = d; }
    run(); tick(2); run(); check_counts(1); assert(pins == 4);
    before = pins; tick(2); run(); assert(pins == before);
    puts("CPU: initially unpinned guests become stable");

    reset(4, 2, 1); run(); tick(2); guests[1].fail_stats = 1; run(); assert(pins == 0);
    guests[1].fail_stats = 0; tick(2); run(); check_counts(2);
    puts("CPU: incomplete statistics abort redistribution and later recover");

    reset(4, 2, 1); run(); tick(2); guests[1].fail_pin = 1; guests[3].fail_pin = 1; run();
    guests[1].fail_pin = guests[3].fail_pin = 0; tick(2); run(); check_counts(2);
    puts("CPU: failed pin requests retried from observed affinities");

    reset(4, 2, 1); run(); tick(2); guests[0].id += 1000; run(); assert(pins == 0);
    tick(2); run(); check_counts(2);
    before = pins; tick(2); guests[0].v[0].cpuTime = 0; run(); assert(pins == before);
    tick(2); run();
    guest_count = 3; tick(2); run(); assert(refs == frees);
    puts("CPU: restart, counter reset, and departure handled");

    reset(2, 4, 1); run(); tick(2); run(); assert(guests[0].v[0].cpu != guests[1].v[0].cpu);
    reset(4, 1, 1); run(); tick(2); run(); assert(pins == 0);
    simulate_balancing();
    reset(0, 1, 1); run();
    puts("CPU: fewer vCPUs, a single pCPU, and no domains handled");
    return 0;
}
