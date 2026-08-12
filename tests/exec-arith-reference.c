/* The line tests/exec-arith.cas has to print, computed by a C compiler.
 *
 * The point of writing it this way is that nobody asserts what signed
 * division of a negative number should answer: C does, and every machine
 * CommonASM emits for has to match it. The values are in the same order as
 * the program, and the harness compares the two outputs directly.
 *
 * Everything is int32_t because the program keeps everything inside 32 bits,
 * so that a 32-bit machine and a 64-bit one have the same answer to print. */

#include <stdint.h>
#include <stdio.h>

/* Shifting a negative value left is undefined in C but well defined in
 * CommonASM: it is the same bits either way. Doing it through the unsigned
 * type is how to say that without the undefined behaviour. */
static int32_t shift_left(int32_t value, int by) {
    return (int32_t)((uint32_t)value << by);
}

int main(void) {
    int32_t out[36];
    int n = 0;
    int32_t byte_read;
    size_t i;

    /* the four sign combinations of a division and of a remainder */
    out[n++] = -7 / 2;
    out[n++] = 7 / -2;
    out[n++] = -7 / -2;
    out[n++] = -7 % 2;
    out[n++] = 7 % -2;
    out[n++] = -7 % -2;

    /* values too big to be a special case */
    out[n++] = 1000000 / -7;
    out[n++] = 1000000 % -7;
    out[n++] = -1000000 / 7;
    out[n++] = -1000000 % 7;

    /* an arithmetic shift rounds down where a division rounds toward zero,
       and a remainder is not the low bits of a negative number */
    out[n++] = -100 >> 3;
    out[n++] = -100 / 8;
    out[n++] = -7 % 4;

    /* a negative shifted left, and both shifts asked for nothing */
    out[n++] = shift_left(-3, 4);
    out[n++] = shift_left(12345, 0);
    out[n++] = -25 >> 0;

    /* multiply and negate */
    out[n++] = -7 * 6;
    out[n++] = -(-2000000);

    /* the extended operations with a sign in them */
    out[n++] = -42 < 0 ? 42 : -42;
    out[n++] = -5 < 3 ? -5 : 3;
    out[n++] = -5 > 3 ? -5 : 3;

    /* a signed comparison and an unsigned one over the same two values */
    out[n++] = (int32_t)(-1) < 1;
    out[n++] = (uint32_t)(-1) < 1u;
    out[n++] = (int32_t)(-5) <= -5;
    out[n++] = (int32_t)(-1) > 1;

    /* a narrow load sign-extends, and masking is how to undo it */
    byte_read = (int32_t)(int8_t)200;
    out[n++] = byte_read;
    out[n++] = byte_read & 0xff;
    out[n++] = (int32_t)(int16_t)0x8001;

    /* the same arithmetic the optimizer folds in the compiler rather than
       leaving to the machine */
    out[n++] = -7 / 2;
    out[n++] = 7 / -2;
    out[n++] = -7 % 2;
    out[n++] = 7 % -2;
    out[n++] = -100 / 8;
    out[n++] = -7 % 4;
    out[n++] = -3 * 16;
    out[n++] = -7 * 6;

    for (i = 0; i < (size_t)n; i++) printf("%d ", (int)out[i]);
    printf("\n");
    return n == (int)(sizeof(out) / sizeof(out[0])) ? 0 : 1;
}
