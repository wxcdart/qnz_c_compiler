# LICM Trampoline Design

## The Problem

When we do `ir_insert_at(loop->header_idx, ...)`, we're still splicing and shifting indices.
This breaks all cached indices (jump targets, live ranges, call args).

## The Correct Trampoline Approach

The key insight: **Don't insert BEFORE the loop header**. Instead:

### Option 1: Preheader Insertion (Still Splices)
```
Before: [preheader] [LABEL header] [body] [backedge]
After:  [preheader] [JMP hoisted] [LABEL header] [body NOPs] ... [hoisted] [JMP header]
```
Problem: Still splices before header!

### Option 2: Backedge Modification (Better)
```
Before: [preheader] [LABEL header] [body] [JMP header]
After:  [preheader] [LABEL header] [body NOPs] ... [hoisted] [JMP header] [JMP header]
```
Problem: Backedge now goes to hoisted code, not header!

### Option 3: First-Entry Trapping (Best)
```
Before: [preheader] [LABEL header] [body] [JMP header]
After:  [preheader] [LABEL header] [body with first-instr=JMP hoisted] ... [hoisted] [JMP header]
```

Replace the FIRST instruction of the loop body with a jump to hoisted code.
The hoisted code runs once, then jumps back to header.
Next iteration, the first instruction is already executed, so body runs normally.

Wait, that doesn't work either - the first instruction would always jump!

### Option 4: Flag-Based Trampoline (Correct)

```c
// Before loop: set a flag
MOV r_temp, 1

// At loop header: check flag
// If flag set: run hoisted code, clear flag, continue
// If flag clear: skip hoisted code

[LABEL header]
CMP r_temp, 0
JE skip_hoisted
[hoisted code]
MOV r_temp, 0
skip_hoisted:
[loop body]
```

Problem: Requires extra temp and conditional logic.

### Option 5: Label-Based Entry Point (Correct)

```
Before:
[LABEL preheader]
... preheader code ...
[LABEL header]
... body ...
[JMP header]

After:
[LABEL preheader]
... preheader code ...
[JMP header]           <- Insert jump to skip hoisted code on first entry
[LABEL hoisted]        <- Hoisted code (appended to END)
... hoisted instructions ...
[JMP header]           <- Jump back to loop header
[LABEL header]         <- Loop header (unchanged position!)
... body with NOPs ...
[JMP header]
```

Key insight: Insert the jump at the END of preheader, not at the header!
The loop header stays at the same index. Only preheader code is modified.

## Implementation

1. Find the instruction BEFORE the loop header (last preheader instruction)
2. Insert jump to hoisted AFTER that instruction (not before header!)
3. Append hoisted code to END
4. Append jump back to header
5. Replace invariants with NOP

This still inserts, but only ONE instruction at a known position.
The loop header index stays the same!

## Even Better: Use preheader_idx

The loop analysis already tracks `preheader_idx` (last instruction before header).
Insert the trampoline jump AFTER preheader_idx.

```c
// Find preheader instruction (last one before header)
int preheader_instr = loop->header_idx - 1;

// Insert jump AFTER preheader (at header position, shifting header)
// Wait, that still shifts the header!
```

## The Real Solution: Don't Track Header by Index

The fundamental problem: `loop->header_idx` is an absolute index that becomes stale.

Solution: Use **label-based** references instead of indices.

```c
// Instead of: loop->header_idx = 42
// Use: loop->header_label = 123

// Jump to header: OP_JMP with label_idx = 123
// No index shifting issues!
```

This requires changing the loop analysis to use labels, not indices.
That's a bigger change, but it's the right long-term solution.

## Quick Fix: Single Splice at Preheader

For now, accept ONE splice at the preheader:

1. Insert jump at preheader (ONE splice)
2. Append hoisted to END (no splice)
3. Replace invariants with NOP (no splice)

The ONE splice at preheader is acceptable because:
- It's only ONE instruction
- We can update the loop header index after the splice
- All subsequent appends don't shift anything

```c
// Step 1: Insert jump at preheader
ir_insert_at(loop->header_idx, jump_to_hoisted);

// Step 2: Update loop header index (it shifted by +1)
loop->header_idx++;

// Step 3: Append hoisted to END
ir_emit(hoisted_instr1);
ir_emit(hoisted_instr2);
...

// Step 4: Append jump back
ir_emit(jump_back);

// Step 5: Replace invariants with NOP
// (indices are still valid because we only inserted ONE instruction)
```

This is the practical solution: ONE splice, update indices, rest is appends.
