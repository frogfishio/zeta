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
  CP HL, #0
  JR le, done       ; n <= 0 => EOF or error => stop

  ; write n bytes from buf
  LD DE, HL         ; DE := n
  LD HL, buf
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write

  JR read_loop

done:
  RET

buf: RESB 4096
