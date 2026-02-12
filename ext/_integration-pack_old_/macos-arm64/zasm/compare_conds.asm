; SPDX-FileCopyrightText: 2025 Frogfish
; SPDX-License-Identifier: MIT

main:
  LD HL, #0
  LD DE, #0
  CP HL, DE
  JR eq, is_eq
  JR ne, fail

is_eq:
  CP HL, #1
  JR lt, is_lt
  JR ge, fail

is_lt:
  CP HL, #0
  JR le, is_le
  JR gt, fail

is_le:
  CP HL, #0
  JR ge, done
  JR fail

done:
  RET

fail:
  RET
