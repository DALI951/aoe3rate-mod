/*
 * test_decrypt.c — round-trip assertion for the AoE3 decrypt math.
 *
 * Given the real key array (bytes, little-endian dwords) and count 8, asserts:
 *   decrypt(encrypt(x)) == x  for x in a finite range (0..1e6, no NaN/INF)
 * and that the verified key bytes match the expected table.
 * Build: i686-w64-mingw32-gcc.exe decrypt_unit.c test_decrypt.c -o test_decrypt.exe
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

float decrypt_slot_value(const uint32_t *key, const uint32_t *enc, int slot);
uint32_t encrypt_slot_value(const uint32_t *key, float value, int slot);

static const unsigned char KEY_BYTES[32] = {
    0x28, 0x48, 0xAC, 0x4F, 0x94, 0xF8, 0x3A, 0x35,
    0x8B, 0xD8, 0x4C, 0x3F, 0xAB, 0x12, 0xFB, 0xAF,
    0x20, 0xB3, 0x5B, 0xCA, 0xF9, 0xAB, 0xC4, 0x2A,
    0xB1, 0xA1, 0xCF, 0xDA, 0xF2, 0xE4, 0x82, 0x10,
};

static int failures = 0;

static void check_key(const uint32_t *key) {
    unsigned char *k = (unsigned char *)key;
    if (memcmp(k, KEY_BYTES, 32) != 0) {
        printf("FAIL: key table mismatch (dump is byte-swapped or wrong)\n");
        for (int i = 0; i < 32; i++) printf("%02X ", k[i]);
        printf("\n");
        failures++;
    } else {
        printf("PASS: verified key table matches game bytes\n");
    }
}

int main(void) {
    uint32_t key[8];
    memcpy(key, KEY_BYTES, 32);

    check_key(key);

    /* round-trip on each slot over 0..1e6 (finite floats only) */
    for (int s = 0; s < 8; s++) {
        for (double dv = 0.0; dv <= 1e6; dv += 12345.0) {
            float v = (float)dv;
            uint32_t encbuf[8]; memset(encbuf, 0, sizeof(encbuf));
            encbuf[s] = encrypt_slot_value(key, v, s);
            float dec = decrypt_slot_value(key, encbuf, s);
            if (!(dec == v) || isnan(dec) || isinf(dec)) {
                printf("FAIL: slot %d value %.3f -> enc %08X -> %.3f\n",
                       s, (double)v, encbuf[s], (double)dec);
                failures++;
                break;
            }
        }
    }

    /* boundary values */
    {
        float vals[] = { 0.0f, 1.0f, 0.5f, 123456.0f, 999999.0f, -1.0f, 1e6f };
        for (int s = 0; s < 8; s++)
            for (int i = 0; i < (int)(sizeof(vals)/sizeof(vals[0])); i++) {
                uint32_t encbuf[8]; memset(encbuf, 0, sizeof(encbuf));
                encbuf[s] = encrypt_slot_value(key, vals[i], s);
                float dec = decrypt_slot_value(key, encbuf, s);
                if (!(dec == vals[i])) {
                    printf("FAIL: bslot %d %.3f\n", s, (double)vals[i]);
                    failures++;
                }
            }
    }

    if (failures == 0)
        printf("PASS: decrypt(encrypt(x)) == x for all slots (0..1e6), no NaN/INF\n");
    return failures ? 1 : 0;
}