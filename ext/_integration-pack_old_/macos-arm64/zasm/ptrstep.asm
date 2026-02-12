; SPDX-FileCopyrightText: 2025 Frogfish
; SPDX-License-Identifier: MIT

main:
  LD HL, buf
  ADD HL, #4
  ADD HL, #8
  RET

buf: RESB 32
