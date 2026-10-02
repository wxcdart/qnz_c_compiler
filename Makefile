CC ?= gcc
CFLAGS ?= -Wall -Wextra -Isrc -Iinclude -fcommon
LDFLAGS ?=
TEST_SRCS = $(wildcard test/*.c)
TESTS = $(TEST_SRCS:.c=)


LEXER_OBJ = src/zero_cc/lexer.o
PARSER_OBJ = src/zero_cc/parser.o
EXTRA_OBJS = src/zero_cc/data_structures/hash_table_tpht.o src/zero_cc/semantic/diagnostics.o src/zero_cc/semantic/type.o src/zero_cc/semantic/scope.o src/zero_cc/semantic/promote.o src/zero_cc/semantic/stmt.o src/zero_cc/semantic/visitor.o src/zero_cc/semantic/expr.o src/zero_cc/semantic/decl.o src/zero_cc/semantic.o src/zero_cc/utils/arena.o src/zero_cc/parser/ir.o src/zero_cc/statements.o src/zero_cc/isa/x86_64.o src/zero_cc/isa/aarch64.o src/zero_cc/isa/riscv.o

.PHONY: all test clean golden_test

all: $(TESTS)

$(LEXER_OBJ): src/zero_cc/lexer.c src/zero_cc/lexer.h src/zero_cc/types_lexer.h src/zero_cc/keywords.inc
	$(CC) $(CFLAGS) -c $< -o $@

PARSER_HEADERS = src/zero_cc/parser.h src/zero_cc/types_lexer.h \
	$(wildcard src/zero_cc/parser/*.h) \
	$(wildcard src/zero_cc/expressions/*.h) \
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

STATEMENTS_HEADERS = src/zero_cc/statements.h $(wildcard src/zero_cc/statements/*.h)
src/zero_cc/statements.o: src/zero_cc/statements.c $(STATEMENTS_HEADERS)
	$(CC) $(CFLAGS) -c $< -o $@


test/%: test/%.c $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS)
	$(CC) $(CFLAGS) $< $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS) -o $@
test/align: test/align.c $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS)
	$(CC) $(CFLAGS) $< $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS) -o $@

test: all
	@for t in $(TESTS); do \
		echo "Running $$t..."; \
		if $$t; then \
			echo "  PASS"; \
		else \
			echo "  FAIL"; \
			exit 1; \
		fi; \
	done
	@echo "All tests passed."

benchmark: test/benchmark_struct_by_value
	@echo "Running benchmark..."
	@test/benchmark_struct_by_value

clean:
	rm -f $(TESTS) test.o test.exe
	rm -f $(LEXER_OBJ) $(PARSER_OBJ) $(TLS_OBJ) $(EXTRA_OBJS)

# Target to build and run the golden error test

golden_test: test/golden_error_test
	./test/golden_error_test