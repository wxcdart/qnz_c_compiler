CC ?= gcc
CFLAGS ?= -Wall -Wextra -Isrc -Iinclude -fcommon
LDFLAGS ?=
TEST_SRCS = $(wildcard test/*.c)
TESTS = $(TEST_SRCS:.c=)

TOOLS = tools/gen_keywords
KEYWORDS_INC = src/zero_cc/keywords.inc
SETTERS_OBJ = src/zero_cc/backends/setters.o
TLS_OBJ = src/zero_cc/tls_backend_static.o

.PHONY: all test clean

all: $(TESTS)

$(SETTERS_OBJ): src/zero_cc/backends/setters.c src/zero_cc/zero_cc_backends.h src/zero_cc/types_lexer.h
	$(CC) $(CFLAGS) -c $< -o $@

$(TLS_OBJ): src/zero_cc/tls_backend_static.c
	$(CC) $(CFLAGS) -c $< -o $@

$(TOOLS): tools/gen_keywords.c
	$(CC) $(CFLAGS) -o $@ $<

$(KEYWORDS_INC): $(TOOLS) tools/keywords.txt
	./$(TOOLS) < tools/keywords.txt > $@

test/%: test/%.c $(KEYWORDS_INC) $(SETTERS_OBJ) $(TLS_OBJ)
	$(CC) $(CFLAGS) $< $(SETTERS_OBJ) $(TLS_OBJ) -o $@

test: all
	@echo "Running tests..."
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