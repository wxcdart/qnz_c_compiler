test/jit/vla_test: test/jit/vla_test.c include/zero_cc.h \
 src/zero_cc/core/core.h src/zero_cc/core/utils.h \
 src/zero_cc/core/buffers.h src/zero_cc/core/compiler_state.h \
 src/zero_cc/core/preprocessor.h src/zero_cc/lexer/scan.h \
 src/zero_cc/lexer/state.h src/zero_cc/types/token.h \
 src/zero_cc/jit/backends.h src/zero_cc/lexer/init.h \
 src/zero_cc/lexer/keywords.inc src/zero_cc/lexer/tokenize.h \
 src/zero_cc/types/symbols.h src/zero_cc/types/../isa/isa_backend.h \
 src/zero_cc/core/compiler_state.h src/zero_cc/core/buffers.h \
 src/zero_cc/types/type_system.h src/zero_cc/semantic/expr.h \
 src/zero_cc/semantic/../common/arena.h \
 src/zero_cc/semantic/../parser/parser.h \
 src/zero_cc/semantic/diagnostics.h src/zero_cc/semantic/scope.h \
 src/zero_cc/semantic/../common/zero_tpht.h \
 src/zero_cc/semantic/../common/zero_posix.h \
 src/zero_cc/semantic/../common/posix_error.h \
 src/zero_cc/semantic/../common/posix_mmap.h \
 src/zero_cc/semantic/../common/posix_fileio.h \
 src/zero_cc/semantic/../common/posix_system.h \
 src/zero_cc/semantic/../common/posix_libc.h src/zero_cc/semantic/type.h \
 src/zero_cc/semantic/promote.h src/zero_cc/semantic/../types/token.h \
 src/zero_cc/types/../parser/regalloc.h \
 src/zero_cc/types/../parser/../types/vregs.h \
 src/zero_cc/types/../parser/lowering_state.h \
 src/zero_cc/types/../parser/label_patcher.h src/zero_cc/lexer/next.h \
 src/zero_cc/core/preprocessor.h src/zero_cc/types/types.h \
 src/zero_cc/types/token.h src/zero_cc/types/type_system.h \
 src/zero_cc/types/symbols.h src/zero_cc/types/type_funcs.h \
 src/zero_cc/types/registry.h src/zero_cc/types/emit_helpers.h \
 src/zero_cc/types/symbol_mgmt.h src/zero_cc/parser/parser.h \
 src/zero_cc/jit/jit_linker.h src/zero_cc/jit/type_info.h \
 src/zero_cc/semantic/decl.h src/zero_cc/jit/compile.h \
 src/zero_cc/semantic/semantic.h src/zero_cc/semantic/visitor.h \
 src/zero_cc/parser/ir_emitter.h src/zero_cc/parser/ir.h \
 src/zero_cc/parser/regalloc.h src/zero_cc/parser/live_range.h \
 src/zero_cc/parser/lowering_state.h src/zero_cc/parser/regalloc.h \
 src/zero_cc/jit/compile_unit.h src/zero_cc/types/global_vars.h \
 src/zero_cc/jit/../isa/isa_backend.h src/zero_cc/parser/static_assert.h \
 src/zero_cc/parser/const_eval.h src/zero_cc/parser/../types/types.h \
 src/zero_cc/parser/../types/registry.h src/zero_cc/parser/definitions.h \
 src/zero_cc/jit/execution.h src/zero_cc/jit/../types/global_vars.h \
 src/zero_cc/jit/../common/mmap_adapter.h src/zero_cc/jit/elf_emitter.h
include/zero_cc.h:
src/zero_cc/core/core.h:
src/zero_cc/core/utils.h:
src/zero_cc/core/buffers.h:
src/zero_cc/core/compiler_state.h:
src/zero_cc/core/preprocessor.h:
src/zero_cc/lexer/scan.h:
src/zero_cc/lexer/state.h:
src/zero_cc/types/token.h:
src/zero_cc/jit/backends.h:
src/zero_cc/lexer/init.h:
src/zero_cc/lexer/keywords.inc:
src/zero_cc/lexer/tokenize.h:
src/zero_cc/types/symbols.h:
src/zero_cc/types/../isa/isa_backend.h:
src/zero_cc/core/compiler_state.h:
src/zero_cc/core/buffers.h:
src/zero_cc/types/type_system.h:
src/zero_cc/semantic/expr.h:
src/zero_cc/semantic/../common/arena.h:
src/zero_cc/semantic/../parser/parser.h:
src/zero_cc/semantic/diagnostics.h:
src/zero_cc/semantic/scope.h:
src/zero_cc/semantic/../common/zero_tpht.h:
src/zero_cc/semantic/../common/zero_posix.h:
src/zero_cc/semantic/../common/posix_error.h:
src/zero_cc/semantic/../common/posix_mmap.h:
src/zero_cc/semantic/../common/posix_fileio.h:
src/zero_cc/semantic/../common/posix_system.h:
src/zero_cc/semantic/../common/posix_libc.h:
src/zero_cc/semantic/type.h:
src/zero_cc/semantic/promote.h:
src/zero_cc/semantic/../types/token.h:
src/zero_cc/types/../parser/regalloc.h:
src/zero_cc/types/../parser/../types/vregs.h:
src/zero_cc/types/../parser/lowering_state.h:
src/zero_cc/types/../parser/label_patcher.h:
src/zero_cc/lexer/next.h:
src/zero_cc/core/preprocessor.h:
src/zero_cc/types/types.h:
src/zero_cc/types/token.h:
src/zero_cc/types/type_system.h:
src/zero_cc/types/symbols.h:
src/zero_cc/types/type_funcs.h:
src/zero_cc/types/registry.h:
src/zero_cc/types/emit_helpers.h:
src/zero_cc/types/symbol_mgmt.h:
src/zero_cc/parser/parser.h:
src/zero_cc/jit/jit_linker.h:
src/zero_cc/jit/type_info.h:
src/zero_cc/semantic/decl.h:
src/zero_cc/jit/compile.h:
src/zero_cc/semantic/semantic.h:
src/zero_cc/semantic/visitor.h:
src/zero_cc/parser/ir_emitter.h:
src/zero_cc/parser/ir.h:
src/zero_cc/parser/regalloc.h:
src/zero_cc/parser/live_range.h:
src/zero_cc/parser/lowering_state.h:
src/zero_cc/parser/regalloc.h:
src/zero_cc/jit/compile_unit.h:
src/zero_cc/types/global_vars.h:
src/zero_cc/jit/../isa/isa_backend.h:
src/zero_cc/parser/static_assert.h:
src/zero_cc/parser/const_eval.h:
src/zero_cc/parser/../types/types.h:
src/zero_cc/parser/../types/registry.h:
src/zero_cc/parser/definitions.h:
src/zero_cc/jit/execution.h:
src/zero_cc/jit/../types/global_vars.h:
src/zero_cc/jit/../common/mmap_adapter.h:
src/zero_cc/jit/elf_emitter.h:
