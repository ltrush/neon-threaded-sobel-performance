// test_pmu.c
#include <stdio.h>
#include <stdint.h>

static inline void pmu_init(void) {
    uint64_t pmcr;
    asm volatile("mrs %0, pmcr_el0" : "=r"(pmcr));
    pmcr |= (1 << 0) | (1 << 2); // enable + reset cycle counter
    asm volatile("msr pmcr_el0, %0" :: "r"(pmcr));
    asm volatile("msr pmcntenset_el0, %0" :: "r"(1u << 31)); // unmask cycle counter
}

static inline uint64_t read_cycles(void) {
    uint64_t val;
    asm volatile("isb; mrs %0, pmccntr_el0" : "=r"(val));
    return val;
}

int main(void) {
    pmu_init();

    uint64_t start = read_cycles();

    // Something measurable
    volatile long sum = 0;
    for (int i = 0; i < 1000000; i++) sum += i;

    uint64_t end = read_cycles();

    printf("Cycles elapsed: %llu\n", (unsigned long long)(end - start));
    printf("Sum (ignore): %ld\n", sum);
    return 0;
}
