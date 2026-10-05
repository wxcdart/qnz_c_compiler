#!/bin/bash
# Fix stale object files - forces complete rebuild
# Usage: ./tools/fix_stale_objects.sh

echo "=== Fixing Stale Object Files ==="
echo ""

# Check if build directory exists
if [ -d "build" ]; then
    echo "🗑️  Removing build directory..."
    rm -rf build
fi

# Check for any .o files in current directory
if ls *.o 2>/dev/null; then
    echo "🗑️  Removing stray .o files in root..."
    rm -f *.o
fi

# Check for any .o files in test directories
if find test -name "*.o" 2>/dev/null | grep -q .; then
    echo "🗑️  Removing .o files in test directories..."
    find test -name "*.o" -delete
fi

echo ""
echo "✅ Clean complete. Run 'make -j4' to rebuild."
echo ""
