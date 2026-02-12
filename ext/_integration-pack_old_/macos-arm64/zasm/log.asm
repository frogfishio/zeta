; SPDX-FileCopyrightText: 2025 Frogfish
; SPDX-License-Identifier: MIT

main:
  LD HL, topic
  LD DE, topic_len
  LD BC, msg
  LD IX, msg_len
  CALL zi_telemetry
  RET

topic:     DB "demo"
topic_len: DW 4
msg:       DB "hello", 10
msg_len:   DW 6
