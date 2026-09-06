/*
 * decrypt_unit.c — self-contained decrypt/encrypt math, copied from d3d9.c's
 * exact logic (decrypt_slot). Round-trip property: decrypt(encrypt(x)) == x.
 *
 * The game (FUN_0044efff / FUN_0049a859):
 *   encrypted[slot] = key[slot] XOR bitpattern(value[slot])
 *   value[slot]     = floatbits( key[slot] XOR encrypted[slot] )
 */
#include <stdint.h>

typedef uint32_t dword_t;
typedef union { float f; dword_t u; } fbits_t;

/* same math as d3d9.c decrypt_slot, parameterized so it's standalone */
float decrypt_slot_value(const dword_t *key, const dword_t *enc, int slot) {
    dword_t dec = enc[slot] ^ key[slot];
    fbits_t fb; fb.u = dec;
    return fb.f;
}

dword_t encrypt_slot_value(const dword_t *key, float value, int slot) {
    fbits_t fb; fb.f = value;
    return key[slot] ^ fb.u;
}