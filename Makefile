# ZeroCC Makefile
#
# Incremental builds work correctly - trust the system!
#
# Use 'make rebuild' when:
#   - Adding new source files to Makefile
#   - Changing Makefile itself (EXTRA_OBJS, CFLAGS, etc.)
#   - Switching branches
#   - Unexplained build errors
#
# See docs/INCREMENTAL_BUILD_NOTES.md for details.

CC ?= gcc
CFLAGS ?= -Wall -Wextra -Isrc -Iinclude -fcommon -g -MMD -MP
LDFLAGS ?=

BUILD_DIR ?= build
OBJ_DIR ?= $(BUILD_DIR)/obj
TEST_BIN_DIR ?= $(BUILD_DIR)/test

TEST_SRCS = $(wildcard test/*/*.c)
TESTS = $(patsubst test/%.c,$(TEST_BIN_DIR)/%,$(TEST_SRCS))

LEXER_OBJ = $(OBJ_DIR)/src/zero_cc/lexer/lexer.o
PARSER_OBJ = $(OBJ_DIR)/src/zero_cc/parser/parser.o
TLS_OBJ = $(OBJ_DIR)/src/zero_cc/jit/tls_backend.o
EXTRA_OBJS = $(addprefix $(OBJ_DIR)/, \
	src/zero_cc/common/hash_table_tpht.o \
	src/zero_cc/semantic/diagnostics.o \
	src/zero_cc/semantic/type.o \
	src/zero_cc/semantic/scope.o \
	src/zero_cc/semantic/promote.o \
	src/zero_cc/semantic/stmt.o \
	src/zero_cc/semantic/visitor.o \
	src/zero_cc/semantic/expr.o \
	src/zero_cc/semantic/decl.o \
	src/zero_cc/semantic/semantic.o \
	src/zero_cc/common/arena.o \
	src/zero_cc/ir/ir.o \
	src/zero_cc/ir/ast_lowering.o \
	src/zero_cc/ir/ast_lowering_stmt.o \
	src/zero_cc/ir/ast_lowering_expr.o \
	src/zero_cc/ir/ast_lowering_mc.o \
	src/zero_cc/ir/asm_emitter.o \
	src/zero_cc/opt/opt_pass.o \
	src/zero_cc/opt/opt_constant_fold.o \
	src/zero_cc/opt/opt_dce.o \
	src/zero_cc/opt/opt_mem2reg.o \
	src/zero_cc/opt/opt_cse.o \
	src/zero_cc/opt/opt_gvn.o \
	src/zero_cc/opt/opt_branchless.o \
	src/zero_cc/opt/opt_copy_prop.o \
	src/zero_cc/opt/opt_profile.o \
	src/zero_cc/opt/opt_loop.o \
	src/zero_cc/opt/opt_vectorize.o \
	src/zero_cc/opt/cfg.o \
	src/zero_cc/core/pragma_state.o \
	src/zero_cc/isa/x86_64.o \
	src/zero_cc/isa/aarch64.o \
	src/zero_cc/isa/riscv.o \
	src/zero_cc/jit/elf_writer.o \
	src/zero_cc/types/string_pool.o)

.PHONY: all test clean golden_test test-unit test-semantic test-abi test-jit test-tap benchmark \
	asan asan-unit asan-semantic asan-abi asan-jit \
	test-asan test-asan-unit test-asan-semantic test-asan-abi test-asan-jit \
	valgrind test-valgrind

all: $(TESTS) build/bin/zerocc

PARSER_HEADERS = src/zero_cc/parser/parser.h src/zero_cc/types/types.h \
	$(wildcard src/zero_cc/parser/*.h) \
	$(wildcard src/zero_cc/ir/*.h) \
	$(wildcard src/zero_cc/opt/*.h) \
	$(wildcard src/zero_cc/types/*.h) \
	$(wildcard src/zero_cc/jit/*.h) \
	$(wildcard src/zero_cc/common/*.h)

$(LEXER_OBJ): src/zero_cc/lexer/lexer.c src/zero_cc/lexer/lexer.h src/zero_cc/types/types.h src/zero_cc/lexer/keywords.inc
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(PARSER_OBJ): src/zero_cc/parser/parser.c $(PARSER_HEADERS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(TLS_OBJ): src/zero_cc/jit/tls_backend.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/common/arena.o: src/zero_cc/common/arena.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/type.o: src/zero_cc/semantic/type.c src/zero_cc/semantic/type.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/scope.o: src/zero_cc/semantic/scope.c src/zero_cc/semantic/scope.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/promote.o: src/zero_cc/semantic/promote.c src/zero_cc/semantic/promote.h src/zero_cc/semantic/type.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/stmt.o: src/zero_cc/semantic/stmt.c src/zero_cc/semantic/stmt.h src/zero_cc/semantic/visitor.h src/zero_cc/semantic/promote.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/visitor.o: src/zero_cc/semantic/visitor.c src/zero_cc/semantic/visitor.h src/zero_cc/semantic/stmt.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/expr.o: src/zero_cc/semantic/expr.c src/zero_cc/semantic/expr.h src/zero_cc/semantic/type.h src/zero_cc/semantic/promote.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/decl.o: src/zero_cc/semantic/decl.c src/zero_cc/semantic/decl.h src/zero_cc/semantic/type.h src/zero_cc/semantic/promote.h src/zero_cc/semantic/scope.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/semantic.o: src/zero_cc/semantic/semantic.c src/zero_cc/semantic/semantic.h src/zero_cc/semantic/visitor.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

.SECONDARY: $(EXTRA_OBJS) $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ)


# ZeroCC CLI driver
ZEROCCLIB_OBJS = $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS)
build/bin/zerocc: src/main.c $(ZEROCCLIB_OBJS)
	@mkdir -p build/bin
	$(CC) $(CFLAGS) -o $@ src/main.c $(ZEROCCLIB_OBJS) $(LDFLAGS)

.PHONY: zerocc
zerocc: build/bin/zerocc

$(TEST_BIN_DIR)/%: test/%.c $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS) $(LDFLAGS) -o $@

test: all
	@python3 tools/test_runner.py

# Optimizer sweep: compile & run every test at -O0/-O1/-O2/-O3 in BOTH JIT
# and AOT modes with the zerocc compiler itself, comparing exit code + stdout
# against the -O0 baseline. This is the definitive optimizer regression gate
# (the normal test suite compiles test binaries with the host compiler, so it
# never exercises zerocc's optimization passes).
test-opt: all
	@python3 tools/opt_sweep.py

asan: CFLAGS += -fsanitize=address,undefined -g
asan: LDFLAGS += -fsanitize=address,undefined
asan: all
	@python3 tools/test_runner.py

asan-unit: CFLAGS += -fsanitize=address,undefined -g
asan-unit: LDFLAGS += -fsanitize=address,undefined
asan-unit: all
	@python3 tools/test_runner.py --category unit

asan-semantic: CFLAGS += -fsanitize=address,undefined -g
asan-semantic: LDFLAGS += -fsanitize=address,undefined
asan-semantic: all
	@python3 tools/test_runner.py --category semantic

asan-abi: CFLAGS += -fsanitize=address,undefined -g
asan-abi: LDFLAGS += -fsanitize=address,undefined
asan-abi: all
	@python3 tools/test_runner.py --category abi

asan-jit: CFLAGS += -fsanitize=address,undefined -g
asan-jit: LDFLAGS += -fsanitize=address,undefined
asan-jit: all
	@python3 tools/test_runner.py --category jit

test-asan: asan
test-asan-unit: asan-unit
test-asan-semantic: asan-semantic
test-asan-abi: asan-abi
test-asan-jit: asan-jit

valgrind: all
	@which valgrind > /dev/null 2>&1 || (echo "Error: valgrind is not installed on this system." && exit 1)
	@python3 tools/test_runner.py --valgrind

test-valgrind: valgrind

test-unit: all
	@python3 tools/test_runner.py --category unit

test-semantic: all
	@python3 tools/test_runner.py --category semantic

test-abi: all
	@python3 tools/test_runner.py --category abi

test-jit: all
	@python3 tools/test_runner.py --category jit

test-tap: all
	@python3 tools/test_runner.py --tap

benchmark: $(TEST_BIN_DIR)/abi/benchmark_struct_by_value
	@echo "Running benchmark..."
	@$(TEST_BIN_DIR)/abi/benchmark_struct_by_value

clean:
	rm -rf $(BUILD_DIR)

# Force complete rebuild (clean + build)
# Use this if you encounter strange bugs after adding new source files
rebuild: clean all

# Alternative alias for rebuild
force-rebuild: rebuild

# Target to build and run the golden error test
golden_test: $(TEST_BIN_DIR)/unit/golden_error_test
	./$(TEST_BIN_DIR)/unit/golden_error_test

DEPS = $(shell find $(BUILD_DIR) -name "*.d" 2>/dev/null)
-include $(DEPS)

# Run a single test with fresh compilation
# Usage: make run-test CATEGORY=<category> TEST=<test_name>
# Examples:
#   make run-test CATEGORY=jit TEST=vla_test
#   make run-test CATEGORY=unit TEST=ir_test
run-test:
	@python3 tools/test_wrapper.py $(CATEGORY)/$(TEST)

# Shortcut: make test-jit-vla_test
# Equivalent to: make run-test CATEGORY=jit TEST=vla_test
test-%:
	@CATEGORY=$(shell echo $* | sed 's/-.*//') && TEST=$(shell echo $* | sed 's/.*-//') && python3 tools/test_wrapper.py $$CATEGORY/$$TEST

# List available tests
list-tests:
	@echo "Available tests:"
	@echo ""
	@echo "JIT Tests:"
	@ls test/jit/*.c | sed 's|test/jit/||; s|\.c$$||' | sed 's|^|  - test-|'
	@echo ""
	@echo "Unit Tests:"
	@ls test/unit/*.c | sed 's|test/unit/||; s|\.c$$||' | sed 's|^|  - test-|'
	@echo ""
	@echo "Semantic Tests:"
	@ls test/semantic/*.c | sed 's|test/semantic/||; s|\.c$$||' | sed 's|^|  - test-|'
	@echo ""
	@echo "ABI Tests:"
	@ls test/abi/*.c | sed 's|test/abi/||; s|\.c$$||' | sed 's|^|  - test-|'
