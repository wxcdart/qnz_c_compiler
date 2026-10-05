#!/bin/bash
# Run a single test with fresh compilation
# Usage: ./tools/run_test.sh <category>/<test_name>
# Examples:
#   ./tools/run_test.sh jit/vla_test
#   ./tools/run_test.sh unit/ir_test
#   ./tools/run_test.sh abi/abi_sum_jit

set -e

if [ -z "$1" ]; then
    echo "Usage: $0 <category>/<test_name>"
    echo "Examples:"
    echo "  $0 jit/vla_test"
    echo "  $0 unit/ir_test"
    echo "  $0 abi/abi_sum_jit"
    exit 1
fi

CATEGORY=$(echo "$1" | cut -d'/' -f1)
TEST_NAME=$(echo "$1" | cut -d'/' -f2)

if [ -z "$CATEGORY" ] || [ -z "$TEST_NAME" ]; then
    echo "Error: Invalid format. Use <category>/<test_name>"
    exit 1
fi

# Check if test exists
TEST_SRC="test/${CATEGORY}/${TEST_NAME}.c"
if [ ! -f "$TEST_SRC" ]; then
    echo "Error: Test not found: $TEST_SRC"
    exit 1
fi

# Build the test
echo "Building test: ${CATEGORY}/${TEST_NAME}"
make "build/test/${CATEGORY}/${TEST_NAME}" > /dev/null 2>&1

# Run the test
echo "Running test: ${CATEGORY}/${TEST_NAME}"
./build/test/${CATEGORY}/${TEST_NAME}
