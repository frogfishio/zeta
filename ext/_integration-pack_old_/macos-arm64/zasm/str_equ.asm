; SPDX-FileCopyrightText: 2025 Frogfish
; SPDX-License-Identifier: MIT

main:
  LD HL, msg
  LD DE, msg_len
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write
  LD DE, buf_size
  RET

msg: STR "Hi", 10
buf_size: EQU 16
