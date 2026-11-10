# ZeroCC Makefile
#
# Incremental builds work correctly - trust the system!
#
# Use 'make rebuild' when:
#   - Adding new source files to Makefile
#   - Changing Makefile itself (EXTRA_OBJS, CFLAGS, etc.)
#   - Switching branches
#   - Unexplained build errors
#
# See docs/INCREMENTAL_BUILD_NOTES.md for details.

CC ?= gcc
CFLAGS ?= -Wall -Wextra -Isrc -Iinclude -fcommon -g -MMD -MP
LDFLAGS ?=

BUILD_DIR ?= build
OBJ_DIR ?= $(BUILD_DIR)/obj
TEST_BIN_DIR ?= $(BUILD_DIR)/test
# test/diag/*.c is the diagnostics-battery corpus (deliberately invalid C,
# consumed by tools/diag_battery.py) — not test drivers; keep it out of the
# test binary glob.  test/jit/builtin_apply.c uses __builtin_apply, which
# host gcc miscompiles on x86-64 (garbage int/FP results; only the
# void-return shape works) - it is exercised by the zerocc-only
# test-opt sweep instead.  test/jit/unroll_nonlocal_goto.c uses a goto
# from a nested function to an enclosing label (Sprint 99), which host
# gcc rejects - exercised by tools/loop_unroll_verifier.py instead.
#  test/san/*.c is the zero_san (-fsanitize=undefined)
# corpus: san_trap_* tests DIE with SIGILL by design, so they cannot run
# through the host-gcc suite; tools/san_sweep.py (make test-san) drives them
# with zerocc directly at -O0..-O3 in JIT + AOT.
TEST_SRCS = $(filter-out test/diag/% test/driver/% test/linker/% test/san/% test/jit/builtin_apply.c test/jit/unroll_nonlocal_goto.c,$(wildcard test/*/*.c))
TESTS = $(patsubst test/%.c,$(TEST_BIN_DIR)/%,$(TEST_SRCS))

LEXER_OBJ = $(OBJ_DIR)/src/zero_cc/lexer/lexer.o
PARSER_OBJ = $(OBJ_DIR)/src/zero_cc/parser/parser.o
TLS_OBJ = $(OBJ_DIR)/src/zero_cc/jit/tls_backend.o
EXTRA_OBJS = $(addprefix $(OBJ_DIR)/, \
	src/zero_cc/core/compiler_free.o \
	src/zero_cc/core/preprocessor.o \
	src/zero_cc/common/hash_table_tpht.o \
	src/zero_cc/semantic/diagnostics.o \
	src/zero_cc/semantic/type.o \
	src/zero_cc/semantic/scope.o \
	src/zero_cc/semantic/promote.o \
	src/zero_cc/semantic/stmt.o \
	src/zero_cc/semantic/visitor.o \
	src/zero_cc/semantic/expr.o \
	src/zero_cc/semantic/decl.o \
	src/zero_cc/semantic/semantic.o \
	src/zero_cc/parser/decl_struct.o \
	src/zero_cc/parser/decl_local.o \
	src/zero_cc/parser/const_eval.o \
	src/zero_cc/parser/init_common.o \
	src/zero_cc/parser/ast_expr.o \
	src/zero_cc/common/arena.o \
	src/zero_cc/ir/ir.o \
	src/zero_cc/ir/ast_lowering.o \
	src/zero_cc/ir/ast_lowering_stmt.o \
	src/zero_cc/ir/ast_lowering_expr.o \
	src/zero_cc/ir/ast_lowering_call.o \
	src/zero_cc/ir/ast_lowering_builtins.o \
	src/zero_cc/ir/ast_lowering_builtin_helpers.o \
	src/zero_cc/ir/ast_lowering_varargs.o \
	src/zero_cc/ir/ast_lowering_assign.o \
	src/zero_cc/ir/ast_lowering_bitfield.o \
	src/zero_cc/ir/ast_lowering_binop.o \
	src/zero_cc/ir/ast_lowering_unop.o \
	src/zero_cc/ir/ast_lowering_cast.o \
	src/zero_cc/ir/ast_lowering_mc.o \
	src/zero_cc/ir/asm_emitter.o \
	src/zero_cc/opt/opt_pass.o \
	src/zero_cc/opt/opt_constant_fold.o \
	src/zero_cc/opt/opt_dce.o \
	src/zero_cc/opt/opt_mem2reg.o \
	src/zero_cc/opt/opt_cse.o \
	src/zero_cc/opt/opt_gvn.o \
	src/zero_cc/opt/opt_branchless.o \
	src/zero_cc/opt/opt_copy_prop.o \
	src/zero_cc/opt/opt_profile.o \
	src/zero_cc/opt/opt_loop.o \
	src/zero_cc/opt/opt_loop_unroll.o \
	src/zero_cc/opt/opt_temp_renumber.o \
	src/zero_cc/opt/opt_vectorize.o \
	src/zero_cc/opt/cfg.o \
	src/zero_cc/core/pragma_state.o \
	src/zero_cc/isa/backend_registry.o \
	src/zero_cc/isa/x86_64.o \
	src/zero_cc/isa/aarch64.o \
	src/zero_cc/isa/riscv64.o \
	src/zero_cc/isa/riscv32.o \
	src/zero_cc/jit/elf_writer.o \
	src/zero_cc/jit/compile_unit.o \
	src/zero_cc/jit/compile.o \
	src/zero_cc/linker/elf_reader.o \
	src/zero_cc/linker/ar_reader.o \
	src/zero_cc/linker/ar_writer.o \
	src/zero_cc/linker/linker.o \
	src/zero_cc/types/string_pool.o)

.PHONY: all test clean golden_test test-unit test-semantic test-abi test-jit test-tap benchmark \
	asan asan-unit asan-semantic asan-abi asan-jit \
	test-asan test-asan-unit test-asan-semantic test-asan-abi test-asan-jit \
	test-project-diff test-project-runs \
	test-isa-qemu test-driver test-linker \
	test-c23 test-asm test-opt test-san \
	valgrind test-valgrind

all: $(TESTS) build/bin/zerocc build/lib/zero_rt.a build/lib/zero_rt_aarch64.a build/lib/zero_rt_riscv64.a build/lib/zero_rt_riscv32.a

# --- Vendored static-musl host toolchain ----------------------------------
# zerocc and every test binary that links the compiler library are built and
# FULLY STATICALLY linked against the vendored musl (third_party/vendor/musl,
# built in-tree by tools/musl_host_toolchain.sh). The shipped compiler
# therefore has no host-libc (glibc) dependency at runtime (ldd: "not a
# dynamic executable"). Consequences:
#   - musl has no dynamic loader: its dlopen/dlsym/dlclose/dlerror are
#     stubs, and the toolchain strips them from the installed libc.a.
#     zero_rt/dlfcn/dlfcn.c supplies a working dlfcn (resolving against
#     the executable's own ELF .symtab) that is linked ahead of the
#     whole-archived libc, so the JIT keeps using dlsym(RTLD_DEFAULT) as
#     before.
#   - the link uses --whole-archive on musl libc.a so the ENTIRE musl C
#     library is present in that symtab (printf/sin/malloc/... for JITed
#     user code), not just what zerocc itself calls.
#   - -no-pie: a static ET_EXEC keeps .symtab st_value addresses absolute,
#     which the self-lookup relies on (ET_DYN is handled via /proc/self/maps
#     anyway).
MUSL_DIR := third_party/vendor/musl/musl-1.2.6
MUSL_LIBC := $(BUILD_DIR)/musl/lib/libc.a
HOST_GCC ?= $(CC)
ZC_CC ?= $(BUILD_DIR)/bin/musl-gcc
ZC_MUSL_LINK = -static -no-pie -Wl,--whole-archive $(MUSL_LIBC) -Wl,--no-whole-archive

$(ZC_CC): $(MUSL_DIR)/VERSION
	@HOST_GCC=$(HOST_GCC) BUILD_DIR=$(BUILD_DIR) sh tools/musl_host_toolchain.sh
	@test -f $(MUSL_LIBC) || (echo "Error: vendored musl build failed" && exit 1)

.PHONY: musl
musl: $(ZC_CC)


PARSER_HEADERS = src/zero_cc/parser/parser.h src/zero_cc/types/types.h \
	$(wildcard src/zero_cc/parser/*.h) \
	$(wildcard src/zero_cc/ir/*.h) \
	$(wildcard src/zero_cc/opt/*.h) \
	$(wildcard src/zero_cc/types/*.h) \
	$(wildcard src/zero_cc/jit/*.h) \
	$(wildcard src/zero_cc/common/*.h)

$(LEXER_OBJ): src/zero_cc/lexer/lexer.c src/zero_cc/lexer/lexer.h src/zero_cc/types/types.h src/zero_cc/lexer/keywords.inc $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

$(PARSER_OBJ): src/zero_cc/parser/parser.c $(PARSER_HEADERS) $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

$(TLS_OBJ): src/zero_cc/jit/tls_backend.c $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/common/arena.o: src/zero_cc/common/arena.c $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/type.o: src/zero_cc/semantic/type.c src/zero_cc/semantic/type.h $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/scope.o: src/zero_cc/semantic/scope.c src/zero_cc/semantic/scope.h $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/promote.o: src/zero_cc/semantic/promote.c src/zero_cc/semantic/promote.h src/zero_cc/semantic/type.h $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/stmt.o: src/zero_cc/semantic/stmt.c src/zero_cc/semantic/stmt.h src/zero_cc/semantic/visitor.h src/zero_cc/semantic/promote.h $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/visitor.o: src/zero_cc/semantic/visitor.c src/zero_cc/semantic/visitor.h src/zero_cc/semantic/stmt.h $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/expr.o: src/zero_cc/semantic/expr.c src/zero_cc/semantic/expr.h src/zero_cc/semantic/type.h src/zero_cc/semantic/promote.h $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/decl.o: src/zero_cc/semantic/decl.c src/zero_cc/semantic/decl.h src/zero_cc/semantic/type.h src/zero_cc/semantic/promote.h src/zero_cc/semantic/scope.h $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/src/zero_cc/semantic/semantic.o: src/zero_cc/semantic/semantic.c src/zero_cc/semantic/semantic.h src/zero_cc/semantic/visitor.h $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

$(OBJ_DIR)/%.o: %.c $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

.SECONDARY: $(EXTRA_OBJS) $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ)


# 128-bit div/mod runtime for the JIT (host x86-64): compiled by the host CC,
# linked into zerocc and exported so the JIT resolves it via dlsym(RTLD_DEFAULT).
# (The AOT zero_rt.a below compiles the same source with zerocc per-target.)
JIT_INT128_OBJ := $(BUILD_DIR)/obj/zero_rt/jit_int128.o
$(JIT_INT128_OBJ): zero_rt/core/int128.c $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

# zero_rt's own dlfcn (host side): musl's dlopen/dlsym/dlclose/dlerror are
# static-build stubs that the toolchain strips from libc.a, so this object
# provides them (self-ELF .symtab resolution) and must be linked ahead of
# the whole-archived libc.
ZC_DLCFN_OBJ := $(BUILD_DIR)/obj/zero_rt/dlfcn.o
$(ZC_DLCFN_OBJ): zero_rt/dlfcn/dlfcn.c $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

# zero_rt's own libgcc complex-division helpers (host side): zerocc's
# complex `/` lowering calls __div{sc3,dc3,tc3}, which the JIT resolves via
# dlsym on the executable's own .symtab.  A static build has no libgcc_s to
# dlopen, so these objects provide the symbols (self-hosting, like dlfcn).
# Without them: "undefined reference to '__divdc3'" (c-torture ieee).
ZC_COMPLEX_OBJ := $(BUILD_DIR)/obj/zero_rt/jit_complex.o
$(ZC_COMPLEX_OBJ): zero_rt/complex/complex.c $(ZC_CC)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) -c $< -o $@

# Sprint 153 (Host Runtime #11 Phase 1): pre-baked .symtab for zero_rt's
# dlfcn. The generated file takes every symbol's address as a link-time
# relocation, so it is always valid for the binary it is linked into.
# Two-pass link: link, regenerate from the fresh binary, relink if the
# table changed (the generator exits 3 when it rewrites). A fresh build
# bootstraps with the sentinel-only table from --emit-empty.
ZC_EMBED_SYMS := $(BUILD_DIR)/gen/embedded_syms.c
$(ZC_EMBED_SYMS):
	@mkdir -p $(dir $@)
	python3 tools/embed_symtab.py --emit-empty $@ || [ $$? -eq 3 ]

# ZeroCC CLI driver (static-musl: see ZC_MUSL_LINK at the top of this file).
# The former --export-dynamic-symbol=zc_*128 flags are gone: a static binary
# has no dynamic symbol table, and the JIT resolves those helpers through
# zero_rt's dlfcn (dlsym on the executable's own .symtab).
ZEROCCLIB_OBJS = $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS)
build/bin/zerocc: src/main.c $(ZEROCCLIB_OBJS) $(JIT_INT128_OBJ) $(ZC_DLCFN_OBJ) $(ZC_COMPLEX_OBJ) $(ZC_EMBED_SYMS) $(ZC_CC)
	@mkdir -p build/bin
	$(ZC_CC) $(CFLAGS) $(ZC_MUSL_LINK) -o $@ src/main.c $(ZEROCCLIB_OBJS) $(JIT_INT128_OBJ) $(ZC_DLCFN_OBJ) $(ZC_COMPLEX_OBJ) $(LDFLAGS) $(ZC_EMBED_SYMS)
	@rc=0; python3 tools/embed_symtab.py --bin $@ --out $(ZC_EMBED_SYMS) || rc=$$?; \
	if [ $$rc -eq 3 ]; then \
		echo "build/bin/zerocc: embedded symtab changed; relinking"; \
		$(ZC_CC) $(CFLAGS) $(ZC_MUSL_LINK) -o $@ src/main.c $(ZEROCCLIB_OBJS) $(JIT_INT128_OBJ) $(ZC_DLCFN_OBJ) $(ZC_COMPLEX_OBJ) $(LDFLAGS) $(ZC_EMBED_SYMS); \
	fi

.PHONY: zerocc
zerocc: build/bin/zerocc

$(TEST_BIN_DIR)/%: test/%.c $(ZC_CC) $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS) $(ZC_DLCFN_OBJ) $(ZC_COMPLEX_OBJ)
	@mkdir -p $(dir $@)
	$(ZC_CC) $(CFLAGS) $(ZC_MUSL_LINK) $< $(TLS_OBJ) $(LEXER_OBJ) $(PARSER_OBJ) $(EXTRA_OBJS) $(ZC_DLCFN_OBJ) $(ZC_COMPLEX_OBJ) $(LDFLAGS) -o $@

test: all
	@python3 tools/test_runner.py

# Optimizer sweep: compile & run every test at -O0/-O1/-O2/-O3 in BOTH JIT
# and AOT modes with the zerocc compiler itself, comparing exit code + stdout
# against the -O0 baseline. This is the definitive optimizer regression gate
# (the normal test suite compiles test binaries with the host compiler, so it
# never exercises zerocc's optimization passes).
test-opt: all
	@python3 tools/opt_sweep.py

# zero_san v1 (-fsanitize=undefined) regression sweep: compiles test/san/*.c
# with zerocc at -O0..-O3 in JIT + AOT, asserting trap (SIGILL) vs clean
# exit per the san_ok_/san_trap_ name contract, plus a cross-ISA raw-AOT
# qemu smoke (no x86 check bytes leak into aarch64/riscv binaries).
test-san: build/bin/zerocc
	@python3 tools/san_sweep.py

# Multi-file driver tests (T1): run build/bin/zerocc on multiple .c inputs
# (JIT --run, AOT -o, per-input -c/-S, duplicate-def / missing-main errors).
test-driver: build/bin/zerocc
	@python3 tools/driver_tests.py

# Own-linker (T2) tests: link zerocc-emitted .o files (and .a archives) with
# zero_cc_link_exec into a directly-executable static ET_EXEC, diffing
# stdout+exit against the system-linker reference.
test-linker: build/bin/zerocc
	@python3 tools/linker_tests.py

# GCC c-torture conformance gate (Sprint 57): runs the vendored gcc.c-torture
# corpus (third_party/vendor/c-torture) against a gcc -O1 reference and checks
# against the ratcheting baseline in test/torture/baseline.json.
test-torture: build/bin/zerocc
	@python3 tools/torture_run.py
	@# Heavy sub-sweep: the 6 `run_expensive_tests` tests, sequential, merged
	@# into last_run.json (kept out of the parallel main sweep - their gcc
	@# reference builds are the corpus's load-sensitive edge cases).
	@python3 tools/torture_run.py --chunk heavy

# C23 differential conformance gate (Sprint 134): feature-tagged C23 cases
# (std_version, true/false, constexpr, nullptr, designator sizing, #warning,
# c23_misc) compiled & run under zerocc at -O0..-O3 in JIT + AOT and diffed
# against a reference gcc (C23_GCC override, else gcc-15/14/gcc) with the
# matching -std flag; negative cases check accept/reject agreement.
test-c23: build/bin/zerocc
	@python3 tools/c23/test_c23.py --all

# Loop-unrolling differential gate (Sprint 150): test/jit/unroll_*.c at
# -O0 (baseline) vs -O3 JIT + AOT, diffed against host gcc (or the -O0
# baseline when gcc cannot compile the file), plus the pass's own
# unroll-count accounting (expect-unroll markers).
test-loop-unroll: build/bin/zerocc
	@python3 tools/loop_unroll_verifier.py

# Inline-assembly Phase 1 differential gate (Sprint 135): every test/asm/*.c
# compiled under zerocc -O0..-O3 on the AOT *text* path (-S), assembled and
# linked with the host gcc, run, and diffed (rc + stdout) against a
# host-`gcc -O2` reference. Guards the OP_ASM lowering, operand side table,
# no_spill LRA, clobber spill/forbidden-mask, and template interpolation.
test-asm: build/bin/zerocc
	@python3 tools/asm_diff.py

# Project-differential gate (Verification & Scale #1): real-world TUs
# (cJSON, zlib, the sqlite amalgamation) must compile under zerocc at every
# level, differential against host gcc acceptance of the same TU.
test-project-diff: build/bin/zerocc
	@python3 tools/project_diff.py

# Project run + behavior-diff gate (Verification & Scale #2/#3): builds each
# project's own test programs with gcc AND zerocc, runs both on identical
# input, and diffs stdout/stderr/exit code.  `--jit` additionally drives the
# supported cases through the in-process `zerocc --run` merge path.
test-project-runs: build/bin/zerocc
	@python3 tools/project_runs.py

# zero_rt: the ZeroCC-owned runtime (heap, minimal stdio, strings, exit).
# Compiled by zerocc itself; the objects are archived with the system `ar`
# (the own linker reads classic archives; writing them is a follow-up).
ZERO_RT_CFLAGS = -O0 -Izero_rt -Ithird_party/vendor/ryu
ZERO_RT_OBJS = $(BUILD_DIR)/obj/zero_rt/os.o \
               $(BUILD_DIR)/obj/zero_rt/heap.o $(BUILD_DIR)/obj/zero_rt/string.o \
               $(BUILD_DIR)/obj/zero_rt/exit.o $(BUILD_DIR)/obj/zero_rt/stdio.o \
               $(BUILD_DIR)/obj/zero_rt/d2s.o $(BUILD_DIR)/obj/zero_rt/d2fixed.o \
               $(BUILD_DIR)/obj/zero_rt/int128.o $(BUILD_DIR)/obj/zero_rt/errno.o \
               $(BUILD_DIR)/obj/zero_rt/complex.o $(BUILD_DIR)/obj/zero_rt/thread.o
$(BUILD_DIR)/lib/zero_rt.a: $(ZERO_RT_OBJS) $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --ar rcs $@ $(ZERO_RT_OBJS)
$(BUILD_DIR)/obj/zero_rt/os.o: zero_rt/linux/os.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt/heap.o: zero_rt/core/heap.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt/string.o: zero_rt/core/string.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt/exit.o: zero_rt/core/exit.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt/stdio.o: zero_rt/core/stdio.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt/int128.o: zero_rt/core/int128.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt/complex.o: zero_rt/complex/complex.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt/d2s.o: third_party/vendor/ryu/ryu/d2s.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc -c $< -Ithird_party/vendor/ryu -o $@
$(BUILD_DIR)/obj/zero_rt/d2fixed.o: third_party/vendor/ryu/ryu/d2fixed.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc -c $< -Ithird_party/vendor/ryu -o $@
$(BUILD_DIR)/obj/zero_rt/errno.o: zero_rt/core/errno.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt/thread.o: zero_rt/core/thread.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc -c $< $(ZERO_RT_CFLAGS) -o $@

# zero_rt for AArch64
ZERO_RT_AARCH64_OBJS = $(BUILD_DIR)/obj/zero_rt_aarch64/os.o \
                       $(BUILD_DIR)/obj/zero_rt_aarch64/heap.o $(BUILD_DIR)/obj/zero_rt_aarch64/string.o \
                       $(BUILD_DIR)/obj/zero_rt_aarch64/exit.o $(BUILD_DIR)/obj/zero_rt_aarch64/stdio.o \
                       $(BUILD_DIR)/obj/zero_rt_aarch64/d2s.o $(BUILD_DIR)/obj/zero_rt_aarch64/d2fixed.o \
                       $(BUILD_DIR)/obj/zero_rt_aarch64/int128.o $(BUILD_DIR)/obj/zero_rt_aarch64/errno.o \
                       $(BUILD_DIR)/obj/zero_rt_aarch64/complex.o $(BUILD_DIR)/obj/zero_rt_aarch64/thread.o
$(BUILD_DIR)/lib/zero_rt_aarch64.a: $(ZERO_RT_AARCH64_OBJS) $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --ar rcs $@ $(ZERO_RT_AARCH64_OBJS)
$(BUILD_DIR)/obj/zero_rt_aarch64/os.o: zero_rt/linux/os.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target aarch64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_aarch64/heap.o: zero_rt/core/heap.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target aarch64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_aarch64/string.o: zero_rt/core/string.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target aarch64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_aarch64/exit.o: zero_rt/core/exit.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target aarch64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_aarch64/stdio.o: zero_rt/core/stdio.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target aarch64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_aarch64/int128.o: zero_rt/core/int128.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target aarch64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_aarch64/complex.o: zero_rt/complex/complex.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target aarch64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_aarch64/d2s.o: third_party/vendor/ryu/ryu/d2s.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target aarch64 -c $< -Ithird_party/vendor/ryu -o $@
$(BUILD_DIR)/obj/zero_rt_aarch64/d2fixed.o: third_party/vendor/ryu/ryu/d2fixed.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target aarch64 -c $< -Ithird_party/vendor/ryu -o $@
$(BUILD_DIR)/obj/zero_rt_aarch64/errno.o: zero_rt/core/errno.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target aarch64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_aarch64/thread.o: zero_rt/core/thread.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target aarch64 -c $< $(ZERO_RT_CFLAGS) -o $@

# zero_rt for RISC-V 64-bit
ZERO_RT_RISCV64_OBJS = $(BUILD_DIR)/obj/zero_rt_riscv64/os.o \
                       $(BUILD_DIR)/obj/zero_rt_riscv64/heap.o $(BUILD_DIR)/obj/zero_rt_riscv64/string.o \
                       $(BUILD_DIR)/obj/zero_rt_riscv64/exit.o $(BUILD_DIR)/obj/zero_rt_riscv64/stdio.o \
                       $(BUILD_DIR)/obj/zero_rt_riscv64/d2s.o $(BUILD_DIR)/obj/zero_rt_riscv64/d2fixed.o \
                       $(BUILD_DIR)/obj/zero_rt_riscv64/int128.o $(BUILD_DIR)/obj/zero_rt_riscv64/errno.o \
                       $(BUILD_DIR)/obj/zero_rt_riscv64/complex.o $(BUILD_DIR)/obj/zero_rt_riscv64/thread.o
$(BUILD_DIR)/lib/zero_rt_riscv64.a: $(ZERO_RT_RISCV64_OBJS) $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --ar rcs $@ $(ZERO_RT_RISCV64_OBJS)
$(BUILD_DIR)/obj/zero_rt_riscv64/os.o: zero_rt/linux/os.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv64/heap.o: zero_rt/core/heap.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv64/string.o: zero_rt/core/string.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv64/exit.o: zero_rt/core/exit.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv64/stdio.o: zero_rt/core/stdio.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv64/int128.o: zero_rt/core/int128.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv64/complex.o: zero_rt/complex/complex.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv64/d2s.o: third_party/vendor/ryu/ryu/d2s.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv64 -c $< -Ithird_party/vendor/ryu -o $@
$(BUILD_DIR)/obj/zero_rt_riscv64/d2fixed.o: third_party/vendor/ryu/ryu/d2fixed.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv64 -c $< -Ithird_party/vendor/ryu -o $@
$(BUILD_DIR)/obj/zero_rt_riscv64/errno.o: zero_rt/core/errno.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv64 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv64/thread.o: zero_rt/core/thread.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv64 -c $< $(ZERO_RT_CFLAGS) -o $@

# zero_rt for RISC-V 32-bit
ZERO_RT_RISCV32_OBJS = $(BUILD_DIR)/obj/zero_rt_riscv32/os.o \
                       $(BUILD_DIR)/obj/zero_rt_riscv32/heap.o $(BUILD_DIR)/obj/zero_rt_riscv32/string.o \
                       $(BUILD_DIR)/obj/zero_rt_riscv32/exit.o $(BUILD_DIR)/obj/zero_rt_riscv32/stdio.o \
                       $(BUILD_DIR)/obj/zero_rt_riscv32/d2s.o $(BUILD_DIR)/obj/zero_rt_riscv32/d2fixed.o \
                       $(BUILD_DIR)/obj/zero_rt_riscv32/int128.o $(BUILD_DIR)/obj/zero_rt_riscv32/errno.o \
                       $(BUILD_DIR)/obj/zero_rt_riscv32/complex.o $(BUILD_DIR)/obj/zero_rt_riscv32/thread.o
$(BUILD_DIR)/lib/zero_rt_riscv32.a: $(ZERO_RT_RISCV32_OBJS) $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --ar rcs $@ $(ZERO_RT_RISCV32_OBJS)
$(BUILD_DIR)/obj/zero_rt_riscv32/os.o: zero_rt/linux/os.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv32 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv32/heap.o: zero_rt/core/heap.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv32 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv32/string.o: zero_rt/core/string.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv32 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv32/exit.o: zero_rt/core/exit.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv32 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv32/stdio.o: zero_rt/core/stdio.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv32 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv32/int128.o: zero_rt/core/int128.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv32 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv32/complex.o: zero_rt/complex/complex.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv32 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv32/d2s.o: third_party/vendor/ryu/ryu/d2s.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv32 -c $< -Ithird_party/vendor/ryu -o $@
$(BUILD_DIR)/obj/zero_rt_riscv32/d2fixed.o: third_party/vendor/ryu/ryu/d2fixed.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv32 -c $< -Ithird_party/vendor/ryu -o $@
$(BUILD_DIR)/obj/zero_rt_riscv32/errno.o: zero_rt/core/errno.c $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv32 -c $< $(ZERO_RT_CFLAGS) -o $@
$(BUILD_DIR)/obj/zero_rt_riscv32/thread.o: zero_rt/core/thread.c zero_rt/rt_os.h $(BUILD_DIR)/bin/zerocc
	@mkdir -p $(dir $@)
	$(BUILD_DIR)/bin/zerocc --target riscv32 -c $< $(ZERO_RT_CFLAGS) -o $@

# Cross-ISA raw-binary-emitter differential test under QEMU. Builds each
# test/isa_qemu/*.c for every target ISA via zerocc's RAW binary emitter
# (ZC_RAW_AOT=1) into a static executable, then diffs stdout+exit against the
# system cross-gcc reference -- both run under the matching qemu-*-static.
# DEPENDENCY: requires qemu-user-static (qemu-{x86_64,aarch64,riscv64,riscv32}-static)
# and the cross toolchains (aarch64-linux-gnu-gcc, riscv64-linux-gnu-gcc).
# The harness preflights these and reports exactly what is missing. As of
# Sprint 41 all four ISAs are green (44/44); this gate keeps them honest.
test-isa-qemu: all
	@python3 tools/isa_qemu_diff.py

# Cross-ISA __int128 differential gate (Sprint 109): compiles test/jit/int128_*.c
# for x86-64/aarch64/riscv64 at -O0..-O3 in BOTH AOT paths (raw binary and
# -S + cross-assembler), runs under qemu-*-static (x86-64 native), and diffs
# stdout+exit against a host `gcc -static -O2` reference. Guards the cross-ISA
# 128-bit (lo,hi) GPR128 emitters (large-offset scratch clobbers, multiword
# borrow chains, spilled MUL128, call-return high-half protection) that the
# x86-64-only test-opt cannot see.
test-isa128: all
	@python3 tools/isa128_diff.py

asan: CFLAGS += -fsanitize=address,undefined -g
asan: LDFLAGS += -fsanitize=address,undefined
asan: all
	@python3 tools/test_runner.py

asan-unit: CFLAGS += -fsanitize=address,undefined -g
asan-unit: LDFLAGS += -fsanitize=address,undefined
asan-unit: all
	@python3 tools/test_runner.py --category unit

asan-semantic: CFLAGS += -fsanitize=address,undefined -g
asan-semantic: LDFLAGS += -fsanitize=address,undefined
asan-semantic: all
	@python3 tools/test_runner.py --category semantic

asan-abi: CFLAGS += -fsanitize=address,undefined -g
asan-abi: LDFLAGS += -fsanitize=address,undefined
asan-abi: all
	@python3 tools/test_runner.py --category abi

asan-jit: CFLAGS += -fsanitize=address,undefined -g
asan-jit: LDFLAGS += -fsanitize=address,undefined
asan-jit: all
	@python3 tools/test_runner.py --category jit

test-asan: asan
test-asan-unit: asan-unit
test-asan-semantic: asan-semantic
test-asan-abi: asan-abi
test-asan-jit: asan-jit

valgrind: all
	@which valgrind > /dev/null 2>&1 || (echo "Error: valgrind is not installed on this system." && exit 1)
	@python3 tools/test_runner.py --valgrind

test-valgrind: valgrind

test-unit: all
	@python3 tools/test_runner.py --category unit

test-semantic: all
	@python3 tools/test_runner.py --category semantic

test-abi: all
	@python3 tools/test_runner.py --category abi

test-jit: all
	@python3 tools/test_runner.py --category jit

test-tap: all
	@python3 tools/test_runner.py --tap

benchmark: $(TEST_BIN_DIR)/abi/benchmark_struct_by_value
	@echo "Running benchmark..."
	@$(TEST_BIN_DIR)/abi/benchmark_struct_by_value

clean:
	rm -rf $(BUILD_DIR)

# Force complete rebuild (clean + build)
# Use this if you encounter strange bugs after adding new source files
rebuild: clean all

# Alternative alias for rebuild
force-rebuild: rebuild

# Target to build and run the golden error test
golden_test: $(TEST_BIN_DIR)/unit/golden_error_test
	./$(TEST_BIN_DIR)/unit/golden_error_test

DEPS = $(shell find $(BUILD_DIR) -name "*.d" 2>/dev/null)
-include $(DEPS)

# Run a single test with fresh compilation
# Usage: make run-test CATEGORY=<category> TEST=<test_name>
# Examples:
#   make run-test CATEGORY=jit TEST=vla_test
#   make run-test CATEGORY=unit TEST=ir_test
run-test:
	@python3 tools/test_wrapper.py $(CATEGORY)/$(TEST)

# Shortcut: make test-jit-vla_test
# Equivalent to: make run-test CATEGORY=jit TEST=vla_test
test-%:
	@CATEGORY=$(shell echo $* | sed 's/-.*//') && TEST=$(shell echo $* | sed 's/.*-//') && python3 tools/test_wrapper.py $$CATEGORY/$$TEST

# List available tests
list-tests:
	@echo "Available tests:"
	@echo ""
	@echo "JIT Tests:"
	@ls test/jit/*.c | sed 's|test/jit/||; s|\.c$$||' | sed 's|^|  - test-|'
	@echo ""
	@echo "Unit Tests:"
	@ls test/unit/*.c | sed 's|test/unit/||; s|\.c$$||' | sed 's|^|  - test-|'
	@echo ""
	@echo "Semantic Tests:"
	@ls test/semantic/*.c | sed 's|test/semantic/||; s|\.c$$||' | sed 's|^|  - test-|'
	@echo ""
	@echo "ABI Tests:"
	@ls test/abi/*.c | sed 's|test/abi/||; s|\.c$$||' | sed 's|^|  - test-|'
