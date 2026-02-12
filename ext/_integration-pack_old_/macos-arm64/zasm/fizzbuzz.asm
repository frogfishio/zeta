; SPDX-FileCopyrightText: 2025 Frogfish
; SPDX-License-Identifier: MIT

; FizzBuzz demo (1..100). Computed in ZASM (no input).

main:
  LD HL, #1
  LD BC, #0
  LD DE, #0

loop_top:
  LD A, HL
  LD HL, num_save
  LD (HL), A
  INC BC
  INC DE

  ; if count3 == 3
  LD HL, BC
  CP HL, #3
  JR eq, maybe_fizz

  ; if count5 == 5
  LD HL, DE
  CP HL, #5
  JR eq, do_buzz

  JR do_number

maybe_fizz:
  LD HL, DE
  CP HL, #5
  JR eq, do_fizzbuzz
  JR do_fizz

do_fizz:
  LD HL, DE
  LD A, HL
  LD HL, c5_save
  LD (HL), A

  LD HL, fizz
  LD DE, fizz_len
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write
  LD BC, #0
  LD HL, c5_save
  LD A, (HL)
  LD HL, A
  LD DE, HL
  JR loop_continue

do_buzz:
  LD IX, BC
  LD HL, buzz
  LD DE, buzz_len
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write
  LD BC, IX
  LD DE, #0
  JR loop_continue

do_fizzbuzz:
  LD HL, fizzbuzz
  LD DE, fizzbuzz_len
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write
  LD BC, #0
  LD DE, #0
  JR loop_continue

do_number:
  ; save counters before reusing BC for tens
  LD HL, BC
  LD A, HL
  LD HL, c3_save
  LD (HL), A

  LD HL, DE
  LD A, HL
  LD HL, c5_save
  LD (HL), A

  LD HL, num_save
  LD A, (HL)
  LD HL, A
  CP HL, #100
  JR eq, print_100

  LD BC, #0

tens_loop:
  CP HL, #10
  JR lt, tens_done
  SUB HL, #10
  INC BC
  JR tens_loop

tens_done:
  LD IX, HL
  LD HL, BC
  CP HL, #0
  JR eq, one_digit

  LD HL, BC
  ADD HL, #48
  LD A, HL
  LD HL, num_buf
  LD (HL), A

  LD HL, IX
  ADD HL, #48
  LD A, HL
  LD HL, num_buf
  ADD HL, #1
  LD (HL), A

  LD HL, num_buf
  ADD HL, #2
  LD A, #10
  LD (HL), A

  LD HL, num_buf
  LD DE, #3
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write
  JR restore_counts

one_digit:
  LD HL, IX
  ADD HL, #48
  LD A, HL
  LD HL, num_buf
  LD (HL), A

  LD HL, num_buf
  ADD HL, #1
  LD A, #10
  LD (HL), A

  LD HL, num_buf
  LD DE, #2
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write
  JR restore_counts

print_100:
  LD HL, onehund
  LD DE, onehund_len
  LD BC, DE
  LD DE, HL
  LD HL, #1
  CALL zi_write

restore_counts:
  LD HL, c3_save
  LD A, (HL)
  LD HL, A
  LD BC, HL

  LD HL, c5_save
  LD A, (HL)
  LD HL, A
  LD DE, HL

loop_continue:
  LD HL, num_save
  LD A, (HL)
  LD HL, A
  INC HL
  CP HL, #101
  JR lt, loop_top
  RET

num_buf: RESB 4
c3_save: RESB 1
c5_save: RESB 1
num_save: RESB 1

fizz: STR "Fizz", 10
buzz: STR "Buzz", 10
fizzbuzz: STR "FizzBuzz", 10
onehund: STR "100", 10
