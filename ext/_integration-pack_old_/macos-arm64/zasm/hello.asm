; SPDX-FileCopyrightText: 2025 Frogfish
; SPDX-License-Identifier: MIT

; Hello, Zing! in Zilog soul

CALL print_hello
RET

print_hello:
  LD HL, msg
  LD DE, msg_len
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write
  RET

msg:      DB "Hello, Zing from Zilog!", 10
msg_len:  DW 24
