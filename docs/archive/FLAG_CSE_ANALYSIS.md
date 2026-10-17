# Flag CSE Analysis

## Summary

Flag CSE (Common Subexpression Elimination) has been analyzed. The key finding is that **Flag CSE is already "free"** in the current IR design.

## Key Finding

The current IR design naturally shares flag results across multiple flag-reading instructions:

```
CMP a, b
COND_SET x, EQ    // reads flags
COND_SET y, NE    // reads SAME flags (no duplication!)
```

The CMP is **not duplicated** - multiple COND_SET/JCOND instructions simply read the same flags from the same CMP. This is "free" flag CSE.

## True CSE Opportunities

Real CSE opportunities occur when there are **redundant CMP instructions**:

```
// Before:
CMP a, b
COND_SET x, EQ
CMP a, b    // Redundant!
COND_SET y, NE

// After (general CSE):
CMP a, b
COND_SET x, EQ
COND_SET y, NE
```

This is handled by **general CSE** (`opt_cse.c`), not flag-specific logic.

## Comparison: Flag DCE vs Flag CSE

| Aspect | Flag DCE | Flag CSE |
|--------|----------|----------|
| **Purpose** | Remove dead CMPs | Share CMP results |
| **Need** | Yes - removes redundant CMPs | No - already "free" |
| **Implementation** | opt_flag_dce() | Not needed |
| **General CSE** | Not involved | Handles redundant CMPs |

## Conclusion

**Flag CSE is NOT needed as a separate optimization pass.**

The IR design already provides implicit flag sharing, and general CSE handles redundant CMPs.

## Future Work

If specific flag CSE patterns are identified later that general CSE misses, a specialized pass can be added. For now, the general CSE pass is sufficient.
