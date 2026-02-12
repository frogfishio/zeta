; SPDX-FileCopyrightText: 2026 Frogfish
; SPDX-License-Identifier: MIT

main:
  ; Signed byte promoted to 64-bit, nudged upward, written back into the packed block.
  LD8S64 HL, (src_signed_byte)
  ADD64 HL, 20
  ST8_64 (dst_signed_byte), HL

  ; Unsigned byte stays positive, but we still exercise the zero-extend path.
  LD8U64 DE, (src_unsigned_byte)
  ADD64 DE, 0x10
  ST8_64 (dst_unsigned_byte), DE

  ; Halfword stays unsigned and rolls forward by 55.
  LD16U64 HL, (src_u16)
  ADD64 HL, 55
  ST16_64 (dst_u16), HL

  ; 32-bit signed lane walks upward by 1337 before being stored back.
  LD32S64 HL, (src_s32)
  ADD64 HL, 1337
  ST32_64 (dst_s32), HL

  ; Emit the fully-updated packed buffer via zABI zi_write.
  LD HL, dst_signed_byte
  LD DE, #16
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write

  RET

result_block:
dst_signed_byte: RESB 1
dst_unsigned_byte: RESB 1
dst_u16: RESB 2
dst_s32: RESB 4

src_signed_byte: DB 0xF6

src_unsigned_byte: DB 0x2A

src_u16: DB 0xC8, 0x00

src_s32: DB 0x18, 0xFC, 0xFF, 0xFF
