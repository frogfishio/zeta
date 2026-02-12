CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
AR      ?= ar
ARFLAGS ?= rcs

# Override these to test against a different runtime
ZINGCORE_LIB    ?= ../../build/libzingcore25.a
ZINGCORE_INCLUDE ?= ../../zingcore/include

BUILD ?= build

CONFORMANCE_TESTS := \
	test_abi_version \
	test_alloc_zero \
	test_ctl_caps_list \
	test_handle_reserved \
	test_async_default \
	test_sys_info \
	test_event_bus \
	test_event_bus_rpc

.PHONY: all test clean

all: $(addprefix $(BUILD)/,$(CONFORMANCE_TESTS))

test: all
	@echo "Running conformance tests..."
	@set -e; \
	for t in $(CONFORMANCE_TESTS); do \
	  echo "  $$t..."; \
	  $(BUILD)/$$t; \
	done
	@echo "All conformance tests passed."

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/%: %.c $(ZINGCORE_LIB) | $(BUILD)
	$(CC) $(CFLAGS) -I$(ZINGCORE_INCLUDE) $< $(ZINGCORE_LIB) -o $@

clean:
	rm -rf $(BUILD)
