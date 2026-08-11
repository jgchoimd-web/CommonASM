/* Drives tests/extended-kernel.cas: feeds inputs to the compiled CommonASM
   and checks every extended operation against an implementation written here,
   so the two never share a mistake. Built twice, once against the target's own
   instructions and once against the --emulate-extended expansions. */
#include <stdio.h>
#include <stdint.h>

extern void cas_run(void);
extern uint64_t cas_in, cas_popcnt, cas_clz, cas_ctz, cas_bswap,
                cas_rol, cas_ror, cas_rol_reg, cas_ror_reg;

static uint64_t ref_popcnt(uint64_t x) {
    uint64_t n = 0;
    for (int i = 0; i < 64; i++) n += (x >> i) & 1u;
    return n;
}

static uint64_t ref_clz(uint64_t x) {
    for (int i = 63; i >= 0; i--) {
        if ((x >> i) & 1u) return (uint64_t)(63 - i);
    }
    return 64;
}

static uint64_t ref_ctz(uint64_t x) {
    for (int i = 0; i < 64; i++) {
        if ((x >> i) & 1u) return (uint64_t)i;
    }
    return 64;
}

static uint64_t ref_bswap(uint64_t x) {
    uint64_t r = 0;
    for (int i = 0; i < 8; i++) r = (r << 8) | ((x >> (i * 8)) & 0xffu);
    return r;
}

static uint64_t ref_rol(uint64_t x, unsigned n) {
    n &= 63u;
    return n == 0 ? x : (x << n) | (x >> (64 - n));
}

static uint64_t ref_ror(uint64_t x, unsigned n) {
    n &= 63u;
    return n == 0 ? x : (x >> n) | (x << (64 - n));
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
    /* Edge cases first: zero and all-ones decide whether clz and ctz report
       the full width, and a single set bit at either end catches an off-by-one
       in the smear. */
    static const uint64_t inputs[] = {
        0, 1, 2, 3, 0xffull, 0x100ull, 0x8000000000000000ull,
        0xffffffffffffffffull, 0x0123456789abcdefull, 0xdeadbeefcafebabeull,
        0x5555555555555555ull, 0xaaaaaaaaaaaaaaaaull, 0x000000000000ffffull,
        0x7fffffffffffffffull, 0x0000000100000000ull, 0xfffffffffffffffeull
    };
    int count = (int)(sizeof(inputs) / sizeof(inputs[0]));
    for (int i = 0; i < count; i++) {
        cas_in = inputs[i];
        cas_run();
        check("popcnt", inputs[i], cas_popcnt, ref_popcnt(inputs[i]));
        check("clz", inputs[i], cas_clz, ref_clz(inputs[i]));
        check("ctz", inputs[i], cas_ctz, ref_ctz(inputs[i]));
        check("bswap", inputs[i], cas_bswap, ref_bswap(inputs[i]));
        check("rol", inputs[i], cas_rol, ref_rol(inputs[i], 13));
        check("ror", inputs[i], cas_ror, ref_ror(inputs[i], 13));
        check("rol/reg", inputs[i], cas_rol_reg, ref_rol(inputs[i], 13));
        check("ror/reg", inputs[i], cas_ror_reg, ref_ror(inputs[i], 13));
    }
    printf("  %d inputs x 8 checks: %d failures\n", count, failures);
    return failures != 0;
}
