#!/bin/bash
# Automated IR generation fix verification script

set -e

echo "=========================================="
echo "IR Generation Fix Verification"
echo "=========================================="
echo ""

# Test file
TEST_FILE="/tmp/test_accum.c"
IR_DUMP="/tmp/ir_dump.txt"

# Create test case
cat > "$TEST_FILE" << 'EOF'
int abs_val(int x) {
    if (x < 0) return -x;
    return x;
}

int main() {
    int acc = 42;
    acc = acc + abs_val(10);
    return acc;
}
EOF

echo "1. Generating IR dump..."
./build/bin/zerocc -c "$TEST_FILE" -o /tmp/test_accum.o --target aarch64 -dump-ir 2> "$IR_DUMP"
echo "   IR saved to: $IR_DUMP"
echo ""

echo "2. Running accumulator bug detector..."
if python3 tools/ir_accumulator_bug_detector.py --ir "$IR_DUMP" 2>&1 | grep -q "No accumulator bugs"; then
    echo "   ✓ PASS: No accumulator bugs detected"
    BUG_FOUND=0
else
    echo "   ✗ FAIL: Accumulator bugs detected"
    python3 tools/ir_accumulator_bug_detector.py --ir "$IR_DUMP" --verbose
    BUG_FOUND=1
fi
echo ""

echo "3. Checking for missing loads..."
if python3 tools/ir_fix_verifier.py --ir "$IR_DUMP" --check-memory 2>&1 | grep -q "All checks passed"; then
    echo "   ✓ PASS: All memory loads correct"
    LOAD_CHECK=0
else
    echo "   ✗ FAIL: Memory load issues found"
    LOAD_CHECK=1
fi
echo ""

echo "=========================================="
if [ $BUG_FOUND -eq 0 ] && [ $LOAD_CHECK -eq 0 ]; then
    echo "✓ All checks passed!"
    exit 0
else
    echo "✗ Some checks failed"
    exit 1
fi
