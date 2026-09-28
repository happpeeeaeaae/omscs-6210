
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
    host_free = 300 * KIB; tick(2); guests[0].fail_set = 1; run(); assert(sets == 2);
    assert(guests[0].target == 0); assert(guests[1].target == 612 * KIB);
    puts("Memory: missing stats, host query failure, low memory, and failed grants handled");

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
