
#include "mock.h"
#define main original_main
#include "../memory/src/memory_coordinator.c"
#undef main
void run(void) { MemoryScheduler((void *)1, 2); assert(refs == frees); }
void reset(int ng) { guest_count = 0; run(); init_fixture(ng, 4, 1); }

// Guest allocations and host free memory form a conserved pool in this model.
unsigned long long total_memory(void) {
    unsigned long long total = host_free;
    for (int d = 0; d < guest_count; d++) total += guests[d].actual;
    return total;
}

void test_memory_plan_actions(void) {
    reset(3);
    VmRecord records[3] = {0};
    for (int d = 0; d < 3; d++) {
        strcpy(records[d].uuid, guests[d].uuid);
        records[d].domain_id = guests[d].id;
        records[d].memory.actual_kib = 512 * KIB;
        records[d].memory.maximum_kib = 2048 * KIB;
        records[d].memory.sample_valid = true;
        records[d].memory.fresh = true;
    }
    records[0].memory.unused_kib = 100 * KIB;
    records[1].memory.unused_kib = 150 * KIB;
    records[2].memory.unused_kib = 400 * KIB;
    VmMap snapshot = {.records = records, .count = 3};
    MemoryPlan plan = {0};
    assert(plan_target_memory(&snapshot, 350 * KIB, &plan));
    assert(sets == 0 && plan.count == 3);
    assert(!plan.targets[0].grow && plan.targets[0].target_kib == 412 * KIB);
    assert(plan.targets[0].record_index == 2);
    assert(plan.targets[1].grow && plan.targets[1].target_kib == 612 * KIB);
    assert(plan.targets[1].record_index == 0);
    assert(plan.targets[2].grow && plan.targets[2].target_kib == 562 * KIB);
    assert(plan.targets[2].record_index == 1);
    cleanup_memory_plan(&plan);
    puts("Memory: pure plan returns donor and hungry guest targets in priority order");
}

void test_pending_memory(int grow) {
    reset(1);
    guests[0].unused = (grow ? 100 : 400) * KIB;
    unsigned long target = (grow ? 612 : 412) * KIB;
    unsigned long long pool = total_memory();
    run(); assert(sets == 1); assert(guests[0].target == target);
    for (int round = 0; round < 3; round++) {
        run(); assert(sets == 1 + (round >= 1)); // Reissue only after three passes without progress.
        tick(2); run(); assert(sets == 1 + round);
        assert(guests[0].target == target);
    }

    // Partial completion leaves unused memory outside the stable band.
    if (grow) {
        guests[0].actual += 40 * KIB; guests[0].unused += 40 * KIB; host_free -= 40 * KIB;
    } else {
        guests[0].actual -= 40 * KIB; guests[0].unused -= 40 * KIB; host_free += 40 * KIB;
    }
    for (int round = 0; round < 3; round++) {
        run(); assert(sets == 3 + (round >= 2));
        tick(2); run(); assert(sets == 3 + (round >= 1));
        assert(guests[0].target == target);
        assert(total_memory() == pool);
    }
    settle_memory(); tick(2); run(); assert(sets == 4);
    assert(guests[0].actual == target); assert(guests[0].target == 0);
    assert(total_memory() == pool);
    printf("Memory: pending %s reissues after three stalled passes\n", grow ? "growth" : "reclamation");
}

void test_failed_memory_retry(int grow) {
    reset(1);
    guests[0].unused = (grow ? 100 : 400) * KIB;
    guests[0].fail_set = 1;
    run(); assert(sets == 1); assert(guests[0].target == 0);
    run(); assert(sets == 1); // A failed call does not make stale stats usable.
    tick(2); run(); assert(sets == 1); assert(guests[0].target == 0);
    assert(!scheduler_state.history.records[0].memory.fresh);
    guests[0].fail_set = 0;
    run(); assert(sets == 2);
    assert(guests[0].target == (grow ? 612 : 412) * KIB);
    tick(2); run(); assert(sets == 2);
    settle_memory(); tick(2); run(); assert(sets == 2);
    printf("Memory: failed %s retries after three unchanged passes\n", grow ? "growth" : "reclamation");
}

void test_missing_timestamp_fallback(void) {
    reset(1);
    guests[0].omit_last_update = 1;
    guests[0].unused = 100 * KIB;

    run();
    assert(sets == 1 && guests[0].target == 612 * KIB);
    for (int round = 0; round < 4; ++round) {
        run();
        assert(sets == 1 + (round >= 2));
        assert(guests[0].target == 612 * KIB);
    }

    settle_memory();
    guests[0].unused = 150 * KIB;
    run();
    assert(sets == 3 && guests[0].target == 712 * KIB);

    reset(1);
    guests[0].omit_last_update = 1;
    guests[0].missing = 1;
    guests[0].unused = 100 * KIB;
    run();
    assert(sets == 0);
    puts("Memory: missing timestamp permits valid samples, but missing unused memory does not");
}

void test_unchanged_and_zero_timestamp(void) {
    reset(1);
    run();
    assert(scheduler_state.history.records[0].memory.fresh);
    unsigned long long actual = scheduler_state.history.records[0].memory.actual_kib;
    unsigned long long unused = scheduler_state.history.records[0].memory.unused_kib;

    tick(2); run();
    assert(!scheduler_state.history.records[0].memory.fresh);
    assert(scheduler_state.history.records[0].memory.last_update == guests[0].updated);

    guests[0].actual += 40 * KIB;
    guests[0].missing = 1;
    tick(2); run();
    assert(!scheduler_state.history.records[0].memory.sample_valid);
    assert(scheduler_state.history.records[0].memory.actual_kib == actual);

    guests[0].missing = 0;
    guests[0].unused = 100 * KIB;
    guests[0].updated = 0;
    run();
    assert(!scheduler_state.history.records[0].memory.sample_valid);
    assert(!scheduler_state.history.records[0].memory.fresh);
    assert(scheduler_state.history.records[0].memory.actual_kib == actual);
    assert(scheduler_state.history.records[0].memory.unused_kib == unused);
    assert(sets == 0);

    guests[0].updated = 4;
    run();
    assert(scheduler_state.history.records[0].memory.fresh);
    assert(scheduler_state.history.records[0].memory.actual_kib == guests[0].actual);
    assert(scheduler_state.history.records[0].memory.unused_kib == guests[0].unused);
    assert(sets == 1);
    puts("Memory: unchanged, invalid, and zero-timestamp readings preserve state");
}

void test_failed_timestamp_free_retry(void) {
    reset(1);
    guests[0].omit_last_update = 1;
    guests[0].unused = 100 * KIB;
    guests[0].fail_set = 1;

    run();
    assert(sets == 1);
    run();
    run();
    assert(sets == 1);
    run();
    assert(sets == 2);

    guests[0].fail_set = 0;
    run();
    run();
    assert(sets == 2);
    run();
    assert(sets == 3 && guests[0].target == 612 * KIB);
    puts("Memory: timestamp-free failures retry once every three passes");
}

void test_statistics_setup_failure(void) {
    reset(1);
    guests[0].fail_stats_period = 1;
    guests[0].omit_last_update = 1;
    guests[0].unused = 100 * KIB;
    run();
    assert(sets == 1 && guests[0].target == 612 * KIB);
    puts("Memory: available statistics remain usable when period setup fails");
}

void test_noop_target_is_not_sent(void) {
    reset(1);
    VmRecord record = {.domain_handle = &guests[0]};
    record.memory.actual_kib = guests[0].actual;
    VmMap snapshot = {.records = &record, .count = 1};
    MemoryTarget target = {.record_index = 0, .target_kib = guests[0].actual};
    MemoryPlan plan = {.targets = &target, .count = 1};
    apply_memory_targets(&snapshot, &plan);
    assert(sets == 0);
    puts("Memory: unchanged target is never sent to libvirt");
}

void test_idle_guests_stay_at_512(void) {
    reset(4);
    for (int d = 0; d < 4; d++) guests[d].unused = (190 + d * 2) * KIB;
    run(); assert(sets == 0);
    tick(2); run(); assert(sets == 0);
    for (int d = 0; d < 4; d++) assert(guests[d].actual == 512 * KIB);

    guests[0].unused = 151 * KIB;
    tick(2); run(); assert(sets == 0);
    guests[0].unused = 150 * KIB;
    tick(2); run(); assert(sets == 1 && guests[0].target == 612 * KIB);
    for (int d = 1; d < 4; d++) assert(guests[d].target == 0);
    puts("Memory: idle 512 MiB guests stay put until unused memory reaches 150 MiB");
}

void test_fixed_grant_targets(void) {
    reset(2);
    guests[0].unused = guests[1].unused = 100 * KIB;
    host_free = 350 * KIB; // 150 MiB grant budget: 100 for the first guest, 50 for the second.
    guests[0].fail_set = 1;
    run(); assert(sets == 2);
    assert(guests[0].target == 0 && guests[1].target == 562 * KIB);
    guests[0].fail_set = 0;
    guests[0].unused += KIB; // A changed measurement can retry before the three-pass fallback.
    tick(2); run(); assert(sets == 3);
    assert(guests[0].target == 612 * KIB && guests[1].target == 562 * KIB);
    settle_memory(); assert(host_free == 200 * KIB);
    puts("Memory: failed first grant leaves later fixed target unchanged");
}

void test_memory_update_suppression(void) {
    reset(3);
    for (int d = 0; d < 3; d++) guests[d].unused = (200 + d * 50) * KIB;
    for (int round = 0; round < 6; round++) {
        tick(2); run(); assert(sets == 0);
    }
    puts("Memory: unchanged statistics in the stable band need no requests");

    reset(2);
    guests[0].actual = guests[0].maximum = 600 * KIB;
    guests[1].actual = 2048 * KIB; guests[1].maximum = 4096 * KIB;
    guests[0].unused = guests[1].unused = 100 * KIB;
    for (int round = 0; round < 6; round++) {
        tick(2); run(); assert(sets == 0);
    }
    puts("Memory: guests at configured or project ceilings need no requests");

    test_pending_memory(1);
    test_pending_memory(0);
    test_failed_memory_retry(1);
    test_failed_memory_retry(0);
    test_fixed_grant_targets();

    reset(1); guests[0].unused = 150 * KIB;
    run(); assert(sets == 1); assert(guests[0].target == 612 * KIB);
    settle_memory(); tick(2); run(); assert(sets == 1);
    assert(guests[0].actual == 612 * KIB);
    guests[0].unused = 400 * KIB;
    tick(2); run(); assert(sets == 2); assert(guests[0].target == 512 * KIB);
    settle_memory(); tick(2); run(); assert(sets == 2);
    assert(guests[0].actual == 512 * KIB);
    guests[0].unused = 150 * KIB;
    tick(2); run(); assert(sets == 3); assert(guests[0].target == 612 * KIB);
    settle_memory(); tick(2); run(); assert(sets == 3);
    assert(guests[0].actual == 612 * KIB);
    puts("Memory: completed grow-shrink-grow can reuse an earlier target");

    for (unsigned long budget = 1; budget < 4; budget++) {
        reset(1); guests[0].unused = 100 * KIB; host_free = 200 * KIB + budget;
        run(); assert(sets == 0);
        tick(2); run(); assert(sets == 0);
    }
    host_free = 200 * KIB + 4;
    tick(2); run(); assert(sets == 1); assert(guests[0].target == 512 * KIB + 4);
    settle_memory(); assert(host_free == 200 * KIB);
    puts("Memory: sub-page budgets cause no requests; one page permits a grant");
}

void simulate_memory(const char *name, int consumers, int stop_first_early) {
    reset(4);
    host_free = 8192 * KIB;
    unsigned long used[4], peak[4] = {0};
    for (int d = 0; d < 4; d++) {
        used[d] = (d < consumers ? 312 : 112) * KIB;
        guests[d].unused = guests[d].actual - used[d];
        peak[d] = guests[d].actual;
    }
    unsigned long long pool = total_memory();
    int reclaimed_while_other_grew = 0;

    for (int step = 0; step < 100; step++) {
        unsigned long before[4];
        for (int d = 0; d < 4; d++) {
            before[d] = guests[d].actual;
            if (d < consumers) {
                int finish = stop_first_early && d == 0 ? 20 : 45;
                if (step >= finish) used[d] = 312 * KIB;
                else if (step < 39) used[d] += 40 * KIB;
            }
            assert(used[d] <= guests[d].actual);
            guests[d].unused = guests[d].actual - used[d];
            guests[d].updated++;
        }
        run();
        // Requests take effect between samples, rather than inside SetMemory.
        settle_memory();
        assert(host_free >= 200 * KIB);
        assert(total_memory() == pool);
        for (int d = 0; d < 4; d++) {
            assert(guests[d].unused >= 100 * KIB);
            assert(guests[d].actual <= 2048 * KIB);
            if (guests[d].actual > peak[d]) peak[d] = guests[d].actual;
        }
        if (stop_first_early && guests[0].actual < before[0] && guests[1].actual > before[1])
            reclaimed_while_other_grew = 1;
    }

    if (stop_first_early) assert(reclaimed_while_other_grew);
    printf("Memory simulation (%s; 100 rounds): peak/final MiB", name);
    for (int d = 0; d < 4; d++) {
        if (d < consumers && !(stop_first_early && d == 0)) assert(peak[d] == 2048 * KIB);
        // Reclaim in 100 MiB steps until spare memory is inside the stable band.
        // The final allocation need not equal the initial 512 MiB (e.g. 548 MiB).
        assert(guests[d].unused >= 200 * KIB && guests[d].unused <= 300 * KIB);
        assert(guests[d].actual < peak[d]);
        printf(" %lu/%lu", peak[d] / 1024, guests[d].actual / 1024);
    }
    puts("");
}

int main(void) {
    setbuf(stdout, NULL);
    test_memory_plan_actions();
    test_missing_timestamp_fallback();
    test_unchanged_and_zero_timestamp();
    test_failed_timestamp_free_retry();
    test_statistics_setup_failure();
    test_noop_target_is_not_sent();
    test_idle_guests_stay_at_512();
    test_memory_update_suppression();
    reset(4); guests[0].unused = 150 * KIB;
    for (int d = 1; d < 4; d++) guests[d].unused = 400 * KIB;
    run(); assert(sets == 4); assert(guests[0].target == 612 * KIB);
    for (int d = 1; d < 4; d++) assert(guests[d].target == 412 * KIB);
    run(); assert(sets == 4); tick(2); run(); assert(sets == 4);
    settle_memory(); tick(2); run(); assert(sets == 4);
    puts("Memory: hungry guest grows, donors shrink, stale/pending samples stay unchanged");

    reset(4); host_free = 350 * KIB;
    for (int d = 0; d < 4; d++) guests[d].unused = (100 + d * 10) * KIB;
    run(); assert(sets == 2); assert(guests[0].target == 612 * KIB); assert(guests[1].target == 562 * KIB);
    tick(2); run(); assert(sets == 2);
    settle_memory(); assert(host_free == 200 * KIB);
    puts("Memory: hungry-first ordering, partial grants, and shared host reserve");

    reset(2); guests[0].unused = 400 * KIB; guests[1].unused = 100 * KIB; host_free = 200 * KIB;
    run(); assert(sets == 1); assert(guests[0].target == 412 * KIB); assert(guests[1].target == 0);
    tick(2); run(); assert(sets == 1);
    settle_memory(); tick(2); run(); assert(sets == 2); assert(guests[1].target == 612 * KIB);
    puts("Memory: reclaimed memory spent only after host reports it free");

    reset(2); guests[0].unused = 100 * KIB; host_free = 300 * KIB; run(); assert(sets == 1);
    guests[1].unused = 100 * KIB; tick(2); run(); assert(sets == 1);
    puts("Memory: outstanding grants reserved across calls");

    reset(3); guests[0].actual = 2000 * KIB; guests[0].unused = 100 * KIB;
    guests[1].actual = 550 * KIB; guests[1].maximum = 600 * KIB; guests[1].unused = 100 * KIB;
    guests[2].actual = 2048 * KIB; guests[2].unused = 100 * KIB;
    run(); assert(sets == 2); assert(guests[0].target == 2048 * KIB); assert(guests[1].target == 600 * KIB);
    puts("Memory: configured and project maxima respected");

    reset(4); guests[0].unused = 200 * KIB; guests[1].unused = 300 * KIB;
    guests[2].unused = 301 * KIB; guests[3].unused = 600 * KIB;
    run(); assert(sets == 1); assert(guests[2].target == 412 * KIB);
    puts("Memory: threshold boundaries and invalid statistics handled");

    reset(2); guests[0].unused = guests[1].unused = 100 * KIB;
    guests[0].missing = 1; guests[1].updated = 0; run(); assert(sets == 0);
    guests[0].missing = 0; guests[1].updated = 1; host_free = 0; run(); assert(sets == 0);
    host_free = 200 * KIB; tick(2); run(); assert(sets == 0);
    host_free = 300 * KIB; tick(2); guests[0].fail_set = 1; run(); assert(sets == 1);
    assert(guests[0].target == 0); assert(guests[1].target == 0);
    guests[0].unused = 250 * KIB; tick(2); run(); assert(sets == 2);
    assert(guests[1].target == 612 * KIB);
    puts("Memory: missing stats, low memory, and failed grant budget reused next cycle");

    reset(1); guests[0].unused = 150 * KIB; run(); assert(sets == 1);
    guests[0].id++; guests[0].target = 0; run(); assert(sets == 2);
    reset(2); guests[0].unused = 100 * KIB; host_free = 300 * KIB;
    run(); assert(sets == 1);
    guests[1].unused = 100 * KIB; guests[0].fail_uuid = 1; tick(2); run(); assert(sets == 1);
    guests[0].fail_uuid = 0; tick(2); run(); assert(sets == 1);
    puts("Memory: identity-query failure preserves outstanding grants");

    simulate_memory("one consumer", 1, 0);
    simulate_memory("all consumers", 4, 0);
    simulate_memory("one of two consumers finishes early", 2, 1);
    reset(0); run();
    puts("Memory: restart and no domains handled");
    return 0;
}
