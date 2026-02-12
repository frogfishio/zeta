; SPDX-FileCopyrightText: 2025 Frogfish
; SPDX-License-Identifier: MIT

main:
  LD HL, buf
  LD A, #65
  LD (HL), A
  LD A, #0
  LD A, (HL)
  LD HL, buf
  LD DE, #1
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write
  RET

buf: RESB 4
