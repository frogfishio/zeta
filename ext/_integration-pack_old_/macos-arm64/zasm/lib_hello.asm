; SPDX-FileCopyrightText: 2025 Frogfish
; SPDX-License-Identifier: MIT

print_hello:
  LD HL, msg
  LD DE, msg_len
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write
  RET

msg: STR "Hello from lib", 10
