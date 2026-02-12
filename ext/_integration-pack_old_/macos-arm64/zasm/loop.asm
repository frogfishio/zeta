; SPDX-FileCopyrightText: 2025 Frogfish
; SPDX-License-Identifier: MIT

main:
  LD HL, #0
  LD DE, #10

loop_top:
  CP HL, DE
  JR ge, loop_end
  INC HL
  JR loop_top

loop_end:
  RET