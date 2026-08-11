/* Drives tests/translate-kernel.cas, whose inline blocks are all written for
   AArch64 and therefore all have to be lifted before an x86-64 build can use
   them. Checking the results here is what shows the lifting preserved the
   meaning of each block, not just its shape. */
#include <stdio.h>
#include <stdint.h>

extern void cas_run(void);
extern uint64_t cas_in, cas_triple, cas_sum, cas_leading, cas_swapped, cas_shifted;

static uint64_t ref_clz(uint64_t x) {
    for (int i = 63; i >= 0; i--) {
        if ((x >> i) & 1u) return (uint64_t)(63 - i);
    }
    return 64;
}

static uint64_t ref_bswap(uint64_t x) {
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) r = (r << 8) | ((x >> (i * 8)) & 0xffu);
    return r;
}

static int failures = 0;

static void check(const char *name, uint64_t input, uint64_t got, uint64_t want) {
    if (got != want) {
        printf("  FAIL %-9s input=0x%016llx got=0x%016llx want=0x%016llx\n",
               name, (unsigned long long)input, (unsigned long long)got,
               (unsigned long long)want);
        failures++;
    }
}

int main(void) {
    static const uint64_t inputs[] = {
        0, 1, 7, 0xffull, 0x89abcdefull, 0x0123456789abcdefull,
        0x8000000000000000ull, 0xffffffffffffffffull
    };
    int count = (int)(sizeof(inputs) / sizeof(inputs[0]));
    for (int i = 0; i < count; i++) {
        cas_in = inputs[i];
        cas_run();
        check("triple", inputs[i], cas_triple, inputs[i] * 3u);
        check("sum", inputs[i], cas_sum, inputs[i] + 100u);
        check("clz", inputs[i], cas_leading, ref_clz(inputs[i]));
        check("bswap", inputs[i], cas_swapped, ref_bswap(inputs[i]));
        check("shift", inputs[i], cas_shifted, inputs[i] << 5);
    }
    printf("  %d inputs x 5 checks: %d failures\n", count, failures);
    return failures != 0;
}
