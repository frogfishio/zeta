; SPDX-FileCopyrightText: 2025 Frogfish
; SPDX-License-Identifier: MIT

main:
  LD HL, #1
  LD DE, #2
  LD A, #3
  LD BC, #4
  LD IX, #5

  LD DE, HL
  LD HL, DE
  LD A, HL
  LD HL, A
  LD BC, HL
  LD IX, DE
  RET
