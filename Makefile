CC ?= gcc
CFLAGS ?= -Wall -Wextra -Isrc -Iinclude -fcommon
LDFLAGS ?=
TEST_SRCS = $(wildcard test/*.c)
TESTS = $(TEST_SRCS:.c=)

SETTERS_OBJ = src/zero_cc/backends/setters.o
LEXER_OBJ = src/zero_cc/lexer.o
PARSER_OBJ = src/zero_cc/parser.o
EXTRA_OBJS = src/zero_cc/hash_table_tpht.o src/zero_cc/semantic/diagnostics.o src/zero_cc/semantic/type.o src/zero_cc/semantic/scope.o src/zero_cc/semantic/promote.o src/zero_cc/semantic/stmt.o src/zero_cc/semantic/visitor.o src/zero_cc/semantic/expr.o src/zero_cc/arena.o

.PHONY: all test clean

all: $(TESTS)

$(SETTERS_OBJ): src/zero_cc/backends/setters.c src/zero_cc/zero_cc_backends.h src/zero_cc/types_lexer.h
	$(CC) $(CFLAGS) -c $< -o $@

$(LEXER_OBJ): src/zero_cc/lexer.c src/zero_cc/lexer.h src/zero_cc/types_lexer.h src/zero_cc/keywords.inc
	$(CC) $(CFLAGS) -c $< -o $@

$(PARSER_OBJ): src/zero_cc/parser.c src/zero_cc/parser.h src/zero_cc/types_lexer.h
	$(CC) $(CFLAGS) -c $< -o $@

TLS_OBJ = src/zero_cc/tls_backend_static.o
src/zero_cc/tls_backend_static.o: src/zero_cc/tls_backend_static.c
	$(CC) $(CFLAGS) -c $< -o $@
src/zero_cc/arena.o: src/zero_cc/arena.c
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

test/align: test/align.c $(SETTERS_OBJ) $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS)
	$(CC) $(CFLAGS) $< $(SETTERS_OBJ) $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS) -o $@

test/%: test/%.c $(SETTERS_OBJ) $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS)
	$(CC) $(CFLAGS) $< $(SETTERS_OBJ) $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS) -o $@

test: all
benchmark: test/benchmark_struct_by_value
	@echo "Running benchmark..."
	@test/benchmark_struct_by_value
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

clean:
	rm -f $(TESTS) test.o test.exe