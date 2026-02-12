; SPDX-FileCopyrightText: 2025 Frogfish
; SPDX-License-Identifier: MIT

main:
read_loop:
  LD HL, buf
  LD DE, #4096
  LD BC, DE
  LD DE, HL
  LD HL, #0
  CALL zi_read      ; HL := n
  LD IX, HL         ; total length
  LD DE, HL         ; remaining
  LD HL, buf

  LD BC, HL
  LD HL, DE
  CP HL, #0
  JR le, done
  LD HL, BC

loop_top:
  LD A, (HL)
  LD BC, HL
  LD HL, A
  CP HL, #97
  JR lt, no_up
  CP HL, #123
  JR ge, no_up
  SUB HL, #32
  LD A, HL
no_up:
  LD HL, BC
  LD (HL), A
  INC HL
  DEC DE

  LD BC, HL
  LD HL, DE
  CP HL, #0
  JR gt, loop_cont

  LD HL, buf
  LD DE, IX
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write
  JR read_loop

loop_cont:
  LD HL, BC
  JR loop_top

done:
  RET

buf: RESB 4096
