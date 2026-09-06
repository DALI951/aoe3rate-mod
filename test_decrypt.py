#!/usr/bin/env python
"""Round-trip assertion for the AoE3 TAD decrypt math (SWARM test 6.1).

decrypt(encrypt(x)) == x within 0..1e6, no NaN/INF, using the real key table
bytes verified from the Ghidra memory dump (0xC6DF14, little-endian dwords).
"""
import struct

KEY_BYTES = bytes([
    0x28, 0x48, 0xAC, 0x4F, 0x94, 0xF8, 0x3A, 0x35,
    0x8B, 0xD8, 0x4C, 0x3F, 0xAB, 0x12, 0xFB, 0xAF,
    0x20, 0xB3, 0x5B, 0xCA, 0xF9, 0xAB, 0xC4, 0x2A,
    0xB1, 0xA1, 0xCF, 0xDA, 0xF2, 0xE4, 0x82, 0x10,
])
KEY = list(struct.unpack("<8I", KEY_BYTES))

def encrypt_slot(key, value, slot):
    return key[slot] ^ struct.unpack("<I", struct.pack("<f", value))[0]

def decrypt_slot(key, enc, slot):
    return struct.unpack("<f", struct.pack("<I", enc[slot] ^ key[slot]))[0]

assert len(KEY) == 8
assert struct.pack("<8I", *KEY) == KEY_BYTES, "key bytes mismatch"

failures = 0
for slot in range(8):
    encbuf = [0] * 8
    v = 0.0
    while v <= 1e6:
        encbuf[slot] = encrypt_slot(KEY, v, slot)
        dec = decrypt_slot(KEY, encbuf, slot)
        assert dec == v, f"slot {slot}: {v} -> {encbuf[slot]:X} -> {dec}"
        v += 12345.0
    for boundary in (0.0, 0.5, 1.0, 123456.0, 999999.0, 1e6):
        encbuf[slot] = encrypt_slot(KEY, boundary, slot)
        dec = decrypt_slot(KEY, encbuf, slot)
        assert dec == boundary, f"slot {slot}: boundary {boundary} failed"

print("PASS: decrypt(encrypt(x)) == x for slots 0..7 (0..1e6), no NaN/INF")