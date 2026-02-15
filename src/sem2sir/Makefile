CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -O0 -g

BIN := build/sem2sir

SIRCC := ../../dist/bin/macos/sircc

COMMON_DIR := common
JSON_MIN_C := $(COMMON_DIR)/json_min.c
JSON_MIN_H := $(COMMON_DIR)/json_min.h

APP_SRCS := $(JSON_MIN_C) sem2sir_profile.c
APP_SRCS += sem2sir_check.c
APP_SRCS += sem2sir_emit.c
APP_SRCS += sem2sir_emit_core.c
APP_SRCS += sem2sir_emit_parse_type.c
APP_SRCS += sem2sir_emit_parse_helpers.c
APP_SRCS += sem2sir_emit_expr_call_int.c
APP_SRCS += sem2sir_emit_meta.c
APP_SRCS += sem2sir_emit_expr_name_lvalue.c
APP_SRCS += sem2sir_emit_stmt_assign.c
APP_SRCS += sem2sir_emit_expr_ptr.c
APP_SRCS += sem2sir_emit_expr_unary.c
APP_SRCS += sem2sir_emit_expr_bin.c
APP_SRCS += sem2sir_emit_expr_match.c
APP_SRCS += sem2sir_emit_expr_dispatch.c
APP_SRCS += sem2sir_emit_stmt.c
APP_SRCS += sem2sir_emit_unit.c

.PHONY: all clean test e2e-lumen

all: $(BIN)

build:
	mkdir -p build

$(BIN): main.c sem2sir_profile.h $(APP_SRCS) $(JSON_MIN_H) | build
	$(CC) $(CFLAGS) -I. -I$(COMMON_DIR) -o $(BIN) main.c $(APP_SRCS)

test: $(BIN)
	./$(BIN) --dump-profile >/dev/null
	@echo OK_sem2sir_profile
	@if [ -f ../4_ast/cert/cases/lumen/lumen.ast.ops_assign.jsonl ]; then \
		./$(BIN) --check ../4_ast/cert/cases/lumen/lumen.ast.ops_assign.jsonl >/dev/null && echo OK_sem2sir_check_lumen_ops_assign; \
	else \
		echo SKIP_sem2sir_check_lumen_ops_assign; \
	fi
	! ./$(BIN) --check tests/bad_op.jsonl >/dev/null 2>&1
	@echo OK_sem2sir_rejects_bad_op
	! ./$(BIN) --check tests/bad_unknown_constructor.jsonl >/dev/null 2>&1
	@echo OK_sem2sir_rejects_unknown_constructor
	! ./$(BIN) --check tests/bad_extra_field.jsonl >/dev/null 2>&1
	@echo OK_sem2sir_rejects_extra_field
	! ./$(BIN) --check tests/bad_name_id_not_tok.jsonl >/dev/null 2>&1
	@echo OK_sem2sir_rejects_name_id_not_tok
	! ./$(BIN) --check tests/bad_call_args_scalar.jsonl >/dev/null 2>&1
	@echo OK_sem2sir_rejects_call_args_scalar
	! ./$(BIN) --check tests/bad_default_int_not_integer.jsonl >/dev/null 2>&1
	@echo OK_sem2sir_rejects_bad_default_int
	! ./$(BIN) --check tests/bad_default_ptr_pointee_is_ptr.jsonl >/dev/null 2>&1
	@echo OK_sem2sir_rejects_bad_default_ptr_pointee
	! ./$(BIN) --check tests/bad_meta_ops_nonempty.jsonl >/dev/null 2>&1
	@echo OK_sem2sir_rejects_meta_ops_nonempty
	! ./$(BIN) --check tests/bad_proc_link_name_without_extern.jsonl >/dev/null 2>&1
	@echo OK_sem2sir_rejects_link_name_without_extern
	! ./$(BIN) --check tests/bad_proc_extern_body_not_null.jsonl >/dev/null 2>&1
	@echo OK_sem2sir_rejects_extern_body_not_null
	rm -f tests/out.sir.jsonl tests/out_fail.sir.jsonl tests/out_add.sir.jsonl tests/out_add_i64.sir.jsonl tests/out_eq.sir.jsonl tests/out_lt_i64.sir.jsonl tests/out_default_int_lt.sir.jsonl tests/out_and_sc.sir.jsonl tests/out_or_sc.sir.jsonl tests/out_not.sir.jsonl tests/out_neg_i64.sir.jsonl tests/out_bitnot_i32.sir.jsonl tests/out_ptr_addrof_deref_i64.sir.jsonl tests/out_ptr_typeapp_i64.sir.jsonl tests/out_ptr_typeapp_void_opaque.sir.jsonl tests/out_assign_stmt_i64.sir.jsonl tests/out_assign_expr_return_i64.sir.jsonl tests/out_assign_expr_chain_return_i64.sir.jsonl tests/out_assign_expr_in_add_i64.sir.jsonl tests/out_assign_deref_stmt_i64.sir.jsonl tests/out_assign_deref_stmt_ptr_typeapp_i64_no_default.sir.jsonl tests/out_assign_deref_expr_in_add_i64.sir.jsonl tests/out_assign_expr_rhs_first_i64.sir.jsonl tests/out_assign_deref_stmt_rhs_first_i64.sir.jsonl tests/out_match_i32.sir.jsonl tests/out_loop_break.sir.jsonl tests/out_dowhile_break.sir.jsonl tests/out_for_break.sir.jsonl tests/out_forint_break.sir.jsonl tests/out_dowhile_continue.sir.jsonl tests/out_for_continue.sir.jsonl tests/out_for_continue_no_step.sir.jsonl tests/out_forint_continue.sir.jsonl build/out_ptr_param_inc_i64.sir.jsonl build/out_call2_add2_i64.sir.jsonl
	rm -f tests/out_extern_call_add2_i64.sir.jsonl
	rm -f tests/out_extern_call_add2_i64_no_link_name.sir.jsonl
	./$(BIN) --emit-sir tests/good_typed_min.jsonl --out tests/out.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_typed_min
	./$(BIN) --emit-sir tests/good_typed_bin_add.jsonl --out tests/out_add.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_add.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_bin_add
	./$(BIN) --emit-sir tests/good_typed_bin_add_i64.jsonl --out tests/out_add_i64.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_add_i64.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_bin_add_i64
	./$(BIN) --emit-sir tests/good_typed_bin_eq.jsonl --out tests/out_eq.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_eq.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_bin_eq
	./$(BIN) --emit-sir tests/good_typed_bin_lt_i64.jsonl --out tests/out_lt_i64.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_lt_i64.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_bin_lt_i64
	./$(BIN) --emit-sir tests/good_default_int_i64_cmp_lt.jsonl --out tests/out_default_int_lt.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_default_int_lt.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_default_int_cmp
	./$(BIN) --emit-sir tests/good_typed_bool_and_sc.jsonl --out tests/out_and_sc.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_and_sc.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_bool_and_sc
	./$(BIN) --emit-sir tests/good_typed_bool_or_sc.jsonl --out tests/out_or_sc.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_or_sc.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_bool_or_sc
	./$(BIN) --emit-sir tests/good_typed_unary_not.jsonl --out tests/out_not.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_not.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_unary_not
	./$(BIN) --emit-sir tests/good_typed_unary_neg_i64.jsonl --out tests/out_neg_i64.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_neg_i64.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_unary_neg_i64
	./$(BIN) --emit-sir tests/good_typed_unary_bitnot_i32.jsonl --out tests/out_bitnot_i32.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_bitnot_i32.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_unary_bitnot_i32
	./$(BIN) --emit-sir tests/good_typed_match_i32.jsonl --out tests/out_match_i32.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_match_i32.sir.jsonl >/dev/null
	grep -q '"tag":"sem.switch"' tests/out_match_i32.sir.jsonl
	@echo OK_sem2sir_emit_sir_match_i32
	./$(BIN) --emit-sir tests/good_typed_ptr_addrof_deref_i64.jsonl --out tests/out_ptr_addrof_deref_i64.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_ptr_addrof_deref_i64.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_ptr_addrof_deref_i64
	./$(BIN) --emit-sir tests/good_typed_ptr_typeapp_i64.jsonl --out tests/out_ptr_typeapp_i64.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_ptr_typeapp_i64.sir.jsonl >/dev/null
	grep -q '"k":"type","id":"t:p_i64","kind":"ptr","of":"t:i64"' tests/out_ptr_typeapp_i64.sir.jsonl
	grep -q '"tag":"load.ptr","type_ref":"t:p_i64"' tests/out_ptr_typeapp_i64.sir.jsonl
	@echo OK_sem2sir_emit_sir_ptr_typeapp_i64
	./$(BIN) --emit-sir tests/good_typed_ptr_typeapp_void_opaque.jsonl --out tests/out_ptr_typeapp_void_opaque.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_ptr_typeapp_void_opaque.sir.jsonl >/dev/null
	grep -q '"k":"type","id":"t:p_void","kind":"ptr","of":"t:void"' tests/out_ptr_typeapp_void_opaque.sir.jsonl
	@echo OK_sem2sir_emit_sir_ptr_typeapp_void_opaque
	./$(BIN) --emit-sir tests/good_extern_call_add2_i64.jsonl --out tests/out_extern_call_add2_i64.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_extern_call_add2_i64.sir.jsonl >/dev/null
	grep -q '"tag":"decl.fn"' tests/out_extern_call_add2_i64.sir.jsonl
	grep -q '"fields":{"name":"add2"}' tests/out_extern_call_add2_i64.sir.jsonl
	grep -q '"tag":"call.indirect"' tests/out_extern_call_add2_i64.sir.jsonl
	@echo OK_sem2sir_emit_sir_extern_call_add2_i64
	./$(BIN) --emit-sir tests/good_extern_call_add2_i64_no_link_name.jsonl --out tests/out_extern_call_add2_i64_no_link_name.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_extern_call_add2_i64_no_link_name.sir.jsonl >/dev/null
	grep -q '"tag":"decl.fn"' tests/out_extern_call_add2_i64_no_link_name.sir.jsonl
	grep -q '"fields":{"name":"add2"}' tests/out_extern_call_add2_i64_no_link_name.sir.jsonl
	grep -q '"tag":"call.indirect"' tests/out_extern_call_add2_i64_no_link_name.sir.jsonl
	@echo OK_sem2sir_emit_sir_extern_call_add2_i64_no_link_name
	./$(BIN) --emit-sir tests/good_loop_break.jsonl --out tests/out_loop_break.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_loop_break.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_loop_break
	./$(BIN) --emit-sir tests/good_dowhile_break.jsonl --out tests/out_dowhile_break.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_dowhile_break.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_dowhile_break
	./$(BIN) --emit-sir tests/good_for_break.jsonl --out tests/out_for_break.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_for_break.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_for_break
	./$(BIN) --emit-sir tests/good_forint_break.jsonl --out tests/out_forint_break.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_forint_break.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_forint_break
	./$(BIN) --emit-sir tests/good_dowhile_continue.jsonl --out tests/out_dowhile_continue.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_dowhile_continue.sir.jsonl >/dev/null
	python3 tests/assert_loop_continue.py tests/out_dowhile_continue.sir.jsonl dowhile
	@echo OK_sem2sir_emit_sir_dowhile_continue
	./$(BIN) --emit-sir tests/good_for_continue.jsonl --out tests/out_for_continue.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_for_continue.sir.jsonl >/dev/null
	python3 tests/assert_loop_continue.py tests/out_for_continue.sir.jsonl for
	@echo OK_sem2sir_emit_sir_for_continue
	./$(BIN) --emit-sir tests/good_for_continue_no_step.jsonl --out tests/out_for_continue_no_step.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_for_continue_no_step.sir.jsonl >/dev/null
	python3 tests/assert_loop_continue.py tests/out_for_continue_no_step.sir.jsonl for-nostep
	@echo OK_sem2sir_emit_sir_for_continue_no_step
	./$(BIN) --emit-sir tests/good_forint_continue.jsonl --out tests/out_forint_continue.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_forint_continue.sir.jsonl >/dev/null
	python3 tests/assert_loop_continue.py tests/out_forint_continue.sir.jsonl forint
	@echo OK_sem2sir_emit_sir_forint_continue
	@if [ -f ../4_ast/cert/cases/lumen/lumen.ast.sem2sir_ptr_param_inc_i64.jsonl ]; then \
		./$(BIN) --emit-sir ../4_ast/cert/cases/lumen/lumen.ast.sem2sir_ptr_param_inc_i64.jsonl --out build/out_ptr_param_inc_i64.sir.jsonl >/dev/null && \
		$(SIRCC) --verify-strict --verify-only build/out_ptr_param_inc_i64.sir.jsonl >/dev/null && \
		echo OK_sem2sir_emit_sir_ptr_param_inc_i64; \
	else \
		echo SKIP_sem2sir_emit_sir_ptr_param_inc_i64; \
	fi
	@if [ -f ../4_ast/cert/cases/lumen/lumen.ast.sem2sir_call2_add2_i64.jsonl ]; then \
		./$(BIN) --emit-sir ../4_ast/cert/cases/lumen/lumen.ast.sem2sir_call2_add2_i64.jsonl --out build/out_call2_add2_i64.sir.jsonl >/dev/null && \
		$(SIRCC) --verify-strict --verify-only build/out_call2_add2_i64.sir.jsonl >/dev/null && \
		echo OK_sem2sir_emit_sir_call2_add2_i64; \
	else \
		echo SKIP_sem2sir_emit_sir_call2_add2_i64; \
	fi
	@if [ -f ../4_ast/cert/cases/lumen/lumen.ast.sem2sir_bad_match_expr_i64.jsonl ]; then \
		! ./$(BIN) --emit-sir ../4_ast/cert/cases/lumen/lumen.ast.sem2sir_bad_match_expr_i64.jsonl --out tests/out_fail.sir.jsonl >/dev/null 2>&1 && \
		echo OK_sem2sir_emit_rejects_match_guard; \
	else \
		echo SKIP_sem2sir_emit_rejects_match_guard; \
	fi
	./$(BIN) --emit-sir tests/good_typed_assign_stmt_i64.jsonl --out tests/out_assign_stmt_i64.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_assign_stmt_i64.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_assign_stmt_i64
	./$(BIN) --emit-sir tests/good_typed_assign_expr_return_i64.jsonl --out tests/out_assign_expr_return_i64.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_assign_expr_return_i64.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_assign_expr_return_i64
	./$(BIN) --emit-sir tests/good_typed_assign_expr_chain_return_i64.jsonl --out tests/out_assign_expr_chain_return_i64.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_assign_expr_chain_return_i64.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_assign_expr_chain_return_i64
	./$(BIN) --emit-sir tests/good_typed_assign_expr_in_add_i64.jsonl --out tests/out_assign_expr_in_add_i64.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_assign_expr_in_add_i64.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_assign_expr_in_add_i64
	./$(BIN) --emit-sir tests/good_typed_assign_deref_stmt_i64.jsonl --out tests/out_assign_deref_stmt_i64.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_assign_deref_stmt_i64.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_assign_deref_stmt_i64
	./$(BIN) --emit-sir tests/good_typed_assign_deref_stmt_ptr_typeapp_i64_no_default.jsonl --out tests/out_assign_deref_stmt_ptr_typeapp_i64_no_default.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_assign_deref_stmt_ptr_typeapp_i64_no_default.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_assign_deref_stmt_ptr_typeapp_i64_no_default
	./$(BIN) --emit-sir tests/good_typed_assign_deref_expr_in_add_i64.jsonl --out tests/out_assign_deref_expr_in_add_i64.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_assign_deref_expr_in_add_i64.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_assign_deref_expr_in_add_i64
	./$(BIN) --emit-sir tests/good_typed_assign_expr_rhs_first_i64.jsonl --out tests/out_assign_expr_rhs_first_i64.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_assign_expr_rhs_first_i64.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_assign_expr_rhs_first_i64
	./$(BIN) --emit-sir tests/good_typed_assign_deref_stmt_rhs_first_i64.jsonl --out tests/out_assign_deref_stmt_rhs_first_i64.sir.jsonl >/dev/null
	$(SIRCC) --verify-strict --verify-only tests/out_assign_deref_stmt_rhs_first_i64.sir.jsonl >/dev/null
	@echo OK_sem2sir_emit_sir_assign_deref_stmt_rhs_first_i64
	@if [ -f ../4_ast/cert/cases/lumen/lumen.ast.sem2sir_bool_sc.jsonl ]; then \
		./$(BIN) --emit-sir ../4_ast/cert/cases/lumen/lumen.ast.sem2sir_bool_sc.jsonl --out tests/out_lumen_bool_sc.sir.jsonl >/dev/null && \
		$(SIRCC) --verify-strict --verify-only tests/out_lumen_bool_sc.sir.jsonl >/dev/null && \
		echo OK_sem2sir_emit_sir_lumen_bool_sc; \
	else \
		echo SKIP_sem2sir_emit_sir_lumen_bool_sc; \
	fi
	@if [ -f ../4_ast/cert/cases/lumen/lumen.ast.ops_assign.jsonl ]; then \
		! ./$(BIN) --emit-sir ../4_ast/cert/cases/lumen/lumen.ast.ops_assign.jsonl --out tests/out_fail.sir.jsonl >/dev/null 2>&1 && \
		echo OK_sem2sir_emit_rejects_untyped; \
	else \
		echo SKIP_sem2sir_emit_rejects_untyped; \
	fi
	! ./$(BIN) --emit-sir tests/bad_assign_deref_missing_default_ptr_pointee.jsonl --out tests/out_fail.sir.jsonl >/dev/null 2>&1
	@echo OK_sem2sir_emit_rejects_deref_assign_without_default_ptr_pointee
	! ./$(BIN) --emit-sir tests/bad_ptr_of_ptr_typeapp.jsonl --out tests/out_fail.sir.jsonl >/dev/null 2>&1
	@echo OK_sem2sir_emit_rejects_ptr_of_ptr_typeapp
	! ./$(BIN) --emit-sir tests/bad_deref_ptr_void.jsonl --out tests/out_fail.sir.jsonl >/dev/null 2>&1
	@echo OK_sem2sir_emit_rejects_deref_ptr_void
	@if [ -f ../4_ast/cert/cases/lumen/lumen.ast.sem2sir_bad_param_ptr_void_deref.jsonl ]; then \
		! ./$(BIN) --emit-sir ../4_ast/cert/cases/lumen/lumen.ast.sem2sir_bad_param_ptr_void_deref.jsonl --out tests/out_fail.sir.jsonl >/dev/null 2>&1 && \
		echo OK_sem2sir_emit_rejects_param_deref_ptr_void; \
	else \
		echo SKIP_sem2sir_emit_rejects_param_deref_ptr_void; \
	fi
	@if [ -f ../4_ast/cert/cases/lumen/lumen.ast.sem2sir_bad_call_arity_add2_i64.jsonl ]; then \
		! ./$(BIN) --emit-sir ../4_ast/cert/cases/lumen/lumen.ast.sem2sir_bad_call_arity_add2_i64.jsonl --out tests/out_fail.sir.jsonl >/dev/null 2>&1 && \
		echo OK_sem2sir_emit_rejects_call_arity_add2_i64; \
	else \
		echo SKIP_sem2sir_emit_rejects_call_arity_add2_i64; \
	fi
	@if [ -f ../4_ast/cert/cases/lumen/lumen.ast.sem2sir_bad_call_arg_type_add2_i64.jsonl ]; then \
		! ./$(BIN) --emit-sir ../4_ast/cert/cases/lumen/lumen.ast.sem2sir_bad_call_arg_type_add2_i64.jsonl --out tests/out_fail.sir.jsonl >/dev/null 2>&1 && \
		echo OK_sem2sir_emit_rejects_call_arg_type_add2_i64; \
	else \
		echo SKIP_sem2sir_emit_rejects_call_arg_type_add2_i64; \
	fi
	! ./$(BIN) --emit-sir tests/bad_deref_pointee_mismatch_i64_as_i32.jsonl --out tests/out_fail.sir.jsonl >/dev/null 2>&1
	@echo OK_sem2sir_emit_rejects_deref_pointee_mismatch
	! ./$(BIN) --emit-sir tests/bad_cmp_untyped.jsonl --out tests/out_fail.sir.jsonl >/dev/null 2>&1
	@echo OK_sem2sir_emit_rejects_untyped_cmp

clean:
	rm -rf build
	rm -f $(BIN)

e2e-lumen: $(BIN)
	bash ../../tools/lumen_e2e_sem2sir.sh
