CC ?= gcc
CFLAGS ?= -Wall -Wextra -Isrc -Iinclude -fcommon -g
LDFLAGS ?=
TEST_SRCS = $(wildcard test/*/*.c)
TESTS = $(TEST_SRCS:.c=)


LEXER_OBJ = src/zero_cc/lexer.o
PARSER_OBJ = src/zero_cc/parser.o
EXTRA_OBJS = src/zero_cc/data_structures/hash_table_tpht.o src/zero_cc/semantic/diagnostics.o src/zero_cc/semantic/type.o src/zero_cc/semantic/scope.o src/zero_cc/semantic/promote.o src/zero_cc/semantic/stmt.o src/zero_cc/semantic/visitor.o src/zero_cc/semantic/expr.o src/zero_cc/semantic/decl.o src/zero_cc/semantic.o src/zero_cc/utils/arena.o src/zero_cc/parser/ir.o src/zero_cc/isa/x86_64.o src/zero_cc/isa/aarch64.o src/zero_cc/isa/riscv.o

.PHONY: all test clean golden_test test-unit test-semantic test-abi test-jit test-tap test-junit benchmark \
	asan asan-unit asan-semantic asan-abi asan-jit \
	test-asan test-asan-unit test-asan-semantic test-asan-abi test-asan-jit \
	valgrind test-valgrind

all: $(TESTS)

$(LEXER_OBJ): src/zero_cc/lexer.c src/zero_cc/lexer.h src/zero_cc/types_lexer.h src/zero_cc/keywords.inc
	$(CC) $(CFLAGS) -c $< -o $@

PARSER_HEADERS = src/zero_cc/parser.h src/zero_cc/types_lexer.h \
	$(wildcard src/zero_cc/parser/*.h) \
	$(wildcard src/zero_cc/types/*.h) \
	$(wildcard src/zero_cc/jit/*.h)

$(PARSER_OBJ): src/zero_cc/parser.c $(PARSER_HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@

TLS_OBJ = src/zero_cc/tls_backend.o
src/zero_cc/tls_backend.o: src/zero_cc/tls_backend.c
	$(CC) $(CFLAGS) -c $< -o $@
src/zero_cc/utils/arena.o: src/zero_cc/utils/arena.c
	$(CC) $(CFLAGS) -c $< -o $@
src/zero_cc/semantic/type.o: src/zero_cc/semantic/type.c src/zero_cc/semantic/type.h
	$(CC) $(CFLAGS) -c $< -o $@
src/zero_cc/semantic/scope.o: src/zero_cc/semantic/scope.c src/zero_cc/semantic/scope.h
	$(CC) $(CFLAGS) -c $< -o $@
src/zero_cc/semantic/promote.o: src/zero_cc/semantic/promote.c src/zero_cc/semantic/promote.h src/zero_cc/semantic/type.h
	$(CC) $(CFLAGS) -c $< -o $@
src/zero_cc/semantic/stmt.o: src/zero_cc/semantic/stmt.c src/zero_cc/semantic/stmt.h src/zero_cc/semantic/visitor.h src/zero_cc/semantic/promote.h
	$(CC) $(CFLAGS) -c $< -o $@
src/zero_cc/semantic/visitor.o: src/zero_cc/semantic/visitor.c src/zero_cc/semantic/visitor.h src/zero_cc/semantic/stmt.h
	$(CC) $(CFLAGS) -c $< -o $@
src/zero_cc/semantic/expr.o: src/zero_cc/semantic/expr.c src/zero_cc/semantic/expr.h src/zero_cc/semantic/type.h src/zero_cc/semantic/promote.h
	$(CC) $(CFLAGS) -c $< -o $@
src/zero_cc/semantic/decl.o: src/zero_cc/semantic/decl.c src/zero_cc/semantic/decl.h src/zero_cc/semantic/type.h src/zero_cc/semantic/promote.h src/zero_cc/semantic/scope.h
	$(CC) $(CFLAGS) -c $< -o $@
src/zero_cc/semantic.o: src/zero_cc/semantic.c src/zero_cc/semantic.h src/zero_cc/semantic/visitor.h
	$(CC) $(CFLAGS) -c $< -o $@

src/zero_cc/%.o: src/zero_cc/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

.SECONDARY: $(EXTRA_OBJS) $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ)

test/%: test/%.c $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS) $(LDFLAGS) -o $@

test: all
	@python3 tools/test_runner.py

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

test-junit: all
	@python3 tools/test_runner.py --junit test_results.xml

benchmark: test/abi/benchmark_struct_by_value
	@echo "Running benchmark..."
	@test/abi/benchmark_struct_by_value

clean:
	rm -f $(TESTS) test.o test.exe test_results.xml
	rm -f $(LEXER_OBJ) $(PARSER_OBJ) $(TLS_OBJ) $(EXTRA_OBJS)

# Target to build and run the golden error test
golden_test: test/unit/golden_error_test
	./test/unit/golden_error_test