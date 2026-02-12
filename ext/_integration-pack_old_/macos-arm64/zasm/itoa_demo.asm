; SPDX-FileCopyrightText: 2025 Frogfish
; SPDX-License-Identifier: MIT

; Build order: place this file before lib/itoa.asm so main is the entry slice.
; itoa reads from itoa_in (0..255) and writes itoa_buf + itoa_len.

main:
  LD HL, itoa_in
  LD A, #42
  LD (HL), A
  CALL itoa
  LD HL, itoa_len
  LD A, (HL)
  LD HL, A
  LD DE, HL
  LD HL, itoa_buf
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write

  LD HL, itoa_in
  LD A, #100
  LD (HL), A
  CALL itoa
  LD HL, itoa_len
  LD A, (HL)
  LD HL, A
  LD DE, HL
  LD HL, itoa_buf
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write

  RET
