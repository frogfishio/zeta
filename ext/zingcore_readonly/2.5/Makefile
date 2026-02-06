CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
AR      ?= ar
ARFLAGS ?= rcs

BUILD       ?= build
DIST        ?= dist
DIST_DEBUG  ?= $(DIST)/debug
DIST_LIB    ?= $(DIST_DEBUG)/lib
DIST_INCLUDE?= $(DIST_DEBUG)/include

SRCDIR := zingcore/src
INCDIR := zingcore/include
TESTDIR := zingcore/tests

LIB := $(BUILD)/libzingcore25.a

SRC := \
	$(SRCDIR)/zingcore25.c \
	$(SRCDIR)/zi_hostlib25.c \
	$(SRCDIR)/zi_runtime25.c \
	$(SRCDIR)/zi_handles25.c \
	$(SRCDIR)/zi_zcl1.c \
	$(SRCDIR)/zi_syscalls_core25.c \
	$(SRCDIR)/zi_syscalls_caps25.c \
	$(SRCDIR)/zi_async_default25.c \
	$(SRCDIR)/zi_event_bus25.c \
	$(SRCDIR)/zi_bus_rpc25.c \
	$(SRCDIR)/zi_file_fs25.c \
	$(SRCDIR)/zi_proc_argv25.c \
	$(SRCDIR)/zi_proc_env25.c \
	$(SRCDIR)/zi_proc_hopper25.c \
	$(SRCDIR)/zi_net_tcp25.c \
	$(SRCDIR)/zi_sys_info25.c \
	$(SRCDIR)/vendor/hopper/hopper.c \
	$(SRCDIR)/vendor/hopper/pic.c \
	$(SRCDIR)/zi_caps.c \
	$(SRCDIR)/zi_async.c \
	$(SRCDIR)/zi_problem.c \
	$(SRCDIR)/zi_hopabi25.c \
	$(SRCDIR)/zi_telemetry.c

OBJ := \
	$(BUILD)/zingcore25.o \
	$(BUILD)/zi_hostlib25.o \
	$(BUILD)/zi_runtime25.o \
	$(BUILD)/zi_handles25.o \
	$(BUILD)/zi_zcl1.o \
	$(BUILD)/zi_syscalls_core25.o \
	$(BUILD)/zi_syscalls_caps25.o \
	$(BUILD)/zi_async_default25.o \
	$(BUILD)/zi_event_bus25.o \
	$(BUILD)/zi_bus_rpc25.o \
	$(BUILD)/zi_file_fs25.o \
	$(BUILD)/zi_proc_argv25.o \
	$(BUILD)/zi_proc_env25.o \
	$(BUILD)/zi_proc_hopper25.o \
	$(BUILD)/zi_net_tcp25.o \
	$(BUILD)/zi_sys_info25.o \
	$(BUILD)/vendor/hopper/hopper.o \
	$(BUILD)/vendor/hopper/pic.o \
	$(BUILD)/zi_caps.o \
	$(BUILD)/zi_async.o \
	$(BUILD)/zi_problem.o \
	$(BUILD)/zi_hopabi25.o \
	$(BUILD)/zi_telemetry.o

TESTS := test_caps test_async_registry test_zingcore25_api test_problem test_telemetry_jsonl test_sysabi25_min_core test_sysabi25_ctl_caps_list test_sysabi25_file_cap test_sysabi25_tcp_cap test_sysabi25_argv_cap test_sysabi25_env_cap test_sysabi25_hopper_cap test_sysabi25_async_default_cap test_sysabi25_event_bus_cap test_sysabi25_sys_info_cap test_bus_rpc_v1 test_hopabi25_basic

EXAMPLES := stdio_caps_demo all_caps_demo hopabi_guest_demo

.PHONY: test examples itest

.PHONY: all clean dirs dist-dirs dist-debug

all: dirs $(LIB)

dirs:
	mkdir -p $(BUILD)

dist-dirs:
	mkdir -p $(DIST_LIB) $(DIST_INCLUDE)

$(BUILD)/%.o: $(SRCDIR)/%.c | dirs
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INCDIR) -c $< -o $@

$(BUILD)/test_%.o: $(TESTDIR)/test_%.c | dirs
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INCDIR) -c $< -o $@

$(LIB): $(OBJ)
	$(AR) $(ARFLAGS) $@ $^

test: $(LIB) $(addprefix $(BUILD)/,$(addsuffix .o,$(TESTS)))
	@set -e; \
	for t in $(TESTS); do \
	  bin="$(BUILD)/$$t"; \
	  $(CC) $(CFLAGS) "$(BUILD)/$$t.o" $(LIB) -o "$$bin"; \
	  "$$bin"; \
	done

examples: $(LIB)
	@set -e; \
	for e in $(EXAMPLES); do \
	  bin="$(BUILD)/$$e"; \
	  $(CC) $(CFLAGS) -I$(INCDIR) "zingcore/examples/$$e.c" $(LIB) -o "$$bin"; \
	done

itest: examples
	@set -e; \
	bin="$(BUILD)/all_caps_demo"; \
	root="$$(mktemp -d -t zi_fs_root)"; \
	out="$$(ZI_FS_ROOT="$$root" "$$bin" arg1 arg2 2>&1)"; \
	echo "$$out" | grep -q "^ok$$"; \
	echo "$$out" | grep -q -- "- file/fs"; \
	echo "$$out" | grep -q -- "- async/default"; \
	echo "$$out" | grep -q -- "- event/bus"; \
	echo "$$out" | grep -q -- "- net/tcp"; \
	echo "$$out" | grep -q -- "- proc/argv"; \
	echo "$$out" | grep -q -- "- proc/env"; \
	echo "$$out" | grep -q -- "- proc/hopper"; \
	rm -rf "$$root"

dist-debug: all dist-dirs
	cp $(LIB) $(DIST_LIB)/
	rsync -a $(INCDIR)/ $(DIST_INCLUDE)/

clean:
	rm -rf $(BUILD) $(DIST)
