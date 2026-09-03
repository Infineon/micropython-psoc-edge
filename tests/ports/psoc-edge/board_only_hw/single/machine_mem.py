from machine import mem8, mem16, mem32
import uctypes

print("*** machine.mem Tests ***")

# Allocate a buffer owned by the GC - safe to read/write via mem*
# MicroPython GC allocates 16-byte-aligned blocks, so addr is always
# at least 4-byte aligned.
buf = bytearray(8)
addr = uctypes.addressof(buf)

# --- mem8 write/readback ---
print("\nmem8 write/readback:")
for i, val in enumerate([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88]):
    mem8[addr + i] = val
for i, expected in enumerate([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88]):
    print(mem8[addr + i] == expected)

# --- mem16 write/readback ---
print("\nmem16 write/readback:")
mem16[addr] = 0x1234
mem16[addr + 2] = 0x5678
print(mem16[addr] == 0x1234)
print(mem16[addr + 2] == 0x5678)

# --- mem32 write/readback ---
print("\nmem32 write/readback:")
mem32[addr] = 0x12345678
print(hex(mem32[addr]) == "0x12345678")

# --- Width consistency (little-endian Cortex-M) ---
# Write 0x44332211 via mem32, verify byte/halfword views match little-endian layout
print("\nWidth consistency (LE):")
mem32[addr] = 0x44332211
print(mem8[addr] == 0x11)
print(mem8[addr + 1] == 0x22)
print(mem8[addr + 2] == 0x33)
print(mem8[addr + 3] == 0x44)
print(mem16[addr] == 0x2211)
print(mem16[addr + 2] == 0x4433)

# --- Reverse: write mem8, read back as mem16/mem32 ---
print("\nReverse width (mem8 -> mem16/mem32):")
mem8[addr] = 0xAA
mem8[addr + 1] = 0xBB
mem8[addr + 2] = 0xCC
mem8[addr + 3] = 0xDD
print(mem16[addr] == 0xBBAA)  # little-endian
print(mem16[addr + 2] == 0xDDCC)
print(hex(mem32[addr] & 0xFFFFFFFF) == "0xddccbbaa")

# --- Real hardware register read (ARM Cortex-M33 CPUID) ---
# 0xE000ED00 is a read-only CPU identification register, constant on this SoC.
# Implementer=0x41 (ARM), Architecture=0xF (ARMv8-M), PartNo=0xD21 (Cortex-M33)
print("\nReal HW register (CPUID):")
cpuid = mem32[0xE000ED00]
print((cpuid >> 24) & 0xFF == 0x41)  # ARM implementer
print((cpuid >> 16) & 0xF == 0xF)  # ARMv8-M architecture
print((cpuid >> 4) & 0xFFF == 0xD21)  # Cortex-M33 part number

print("\nDone.")
