CC ?= gcc
CFLAGS ?= -Wall -Wextra -Isrc -Iinclude
LDFLAGS ?=
TEST_SRCS = $(wildcard test/*.c)
TESTS = $(TEST_SRCS:.c=)

.PHONY: all test clean

all: $(TESTS)

test/%: test/%.c
	$(CC) $(CFLAGS) $< -o $@

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