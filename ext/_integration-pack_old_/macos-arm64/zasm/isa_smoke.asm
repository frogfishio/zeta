; SPDX-FileCopyrightText: 2025 Frogfish
; SPDX-License-Identifier: MIT

main:
  LD BC, buf
  INC BC
  DEC BC

  LD HL, #1
  LD DE, #1
  ADD HL, DE
  ADD HL, #1
  SUB HL, DE
  SUB HL, #1
  ADD HL, #1
  SUB HL, #1
  LD A, HL
  LD HL, A

  INC DE
  DEC DE
  CP HL, DE
  JR eq, cond_ok
  JR fail

cond_ok:
  LD HL, #65
  LD A, HL
  LD HL, BC
  LD (HL), A
  LD HL, BC
  ADD HL, #1
  LD A, #66
  LD (HL), A
  JR out

fail:
  LD HL, BC
  LD A, #0
  LD (HL), A
  LD HL, BC
  ADD HL, #1
  LD A, #0
  LD (HL), A

out:
  LD HL, BC
  LD DE, #2
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write
  RET

buf: RESB 2
