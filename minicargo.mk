#
#
#

OUTDIR_SUF ?=
MMIR ?=
RUSTC_CHANNEL ?= stable
RUSTC_VERSION ?= $(shell cat rust-version)
ifeq ($(OS),Windows_NT)
else ifeq ($(shell uname -s || echo not),Darwin)
OVERRIDE_SUFFIX ?= -macos
else
OVERRIDE_SUFFIX ?= -linux
endif
PARLEVEL ?= 1
MINICARGO_FLAGS ?=

ifneq ($(MMIR),)
  OUTDIR_SUF := $(OUTDIR_SUF)-mmir
  MINICARGO_FLAGS += -Z emit-mmir
endif
ifneq ($(PARLEVEL),1)
  MINICARGO_FLAGS += -j $(PARLEVEL)
endif

OUTDIR := output$(OUTDIR_SUF)/

MRUSTC := bin/mrustc
MINICARGO := tools/bin/minicargo
RUSTC_OUT_BIN := rustc
ifeq ($(RUSTC_VERSION),1.29.0)
  RUSTC_OUT_BIN := rustc_binary
endif
ifeq ($(RUSTC_CHANNEL),nightly)
	RUSTCSRC := rustc-nightly-src/
else
	RUSTCSRC := rustc-$(RUSTC_VERSION)-src/
endif

LLVM_CONFIG := $(RUSTCSRC)build/bin/llvm-config
RUSTC_TARGET ?= x86_64-unknown-linux-gnu
LLVM_TARGETS ?= X86;ARM;AArch64#;Mips;PowerPC;SystemZ;JSBackend;MSP430;Sparc;NVPTX
OVERRIDE_DIR := script-overrides/$(RUSTC_CHANNEL)-$(RUSTC_VERSION)$(OVERRIDE_SUFFIX)/

.PHONY: bin/mrustc tools/bin/minicargo
.PHONY: $(OUTDIR)libstd.rlib $(OUTDIR)libtest.rlib $(OUTDIR)libpanic_unwind.rlib $(OUTDIR)libproc_macro.rlib
.PHONY: $(OUTDIR)rustc $(OUTDIR)cargo

.PHONY: all LIBS


all: $(OUTDIR)rustc

LIBS: $(OUTDIR)libstd.rlib $(OUTDIR)libtest.rlib $(OUTDIR)libpanic_unwind.rlib $(OUTDIR)libproc_macro.rlib

$(MRUSTC):
	$(MAKE) -f Makefile all
	test -e $@

$(MINICARGO):
	$(MAKE) -C tools/minicargo/
	test -e $@

# Standard library crates
# - libstd, libpanic_unwind, libtest and libgetopts
# - libproc_macro (mrustc)
$(OUTDIR)libstd.rlib: $(MRUSTC) $(MINICARGO)
	$(MINICARGO) $(RUSTCSRC)src/libstd --script-overrides $(OVERRIDE_DIR) --output-dir $(OUTDIR) $(MINICARGO_FLAGS)
	test -e $@
$(OUTDIR)libpanic_unwind.rlib: $(MRUSTC) $(MINICARGO) $(OUTDIR)libstd.rlib
	$(MINICARGO) $(RUSTCSRC)src/libpanic_unwind --script-overrides $(OVERRIDE_DIR) --output-dir $(OUTDIR) $(MINICARGO_FLAGS)
	test -e $@
$(OUTDIR)libtest.rlib: $(MRUSTC) $(MINICARGO) $(OUTDIR)libstd.rlib $(OUTDIR)libpanic_unwind.rlib
	$(MINICARGO) $(RUSTCSRC)src/libtest --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(OUTDIR) $(MINICARGO_FLAGS)
	test -e $@
$(OUTDIR)libgetopts.rlib: $(MRUSTC) $(MINICARGO) $(OUTDIR)libstd.rlib
	$(MINICARGO) $(RUSTCSRC)src/libgetopts --script-overrides $(OVERRIDE_DIR) --output-dir $(OUTDIR) $(MINICARGO_FLAGS)
	test -e $@
# MRustC custom version of libproc_macro
$(OUTDIR)libproc_macro.rlib: $(MRUSTC) $(MINICARGO) $(OUTDIR)libstd.rlib
	$(MINICARGO) lib/libproc_macro --output-dir $(OUTDIR) $(MINICARGO_FLAGS)
	test -e $@

$(OUTDIR)test/libtest.so: $(MRUSTC) $(MINICARGO)
	mkdir -p $(dir $@)
	MINICARGO_DYLIB=1 $(MINICARGO) $(RUSTCSRC)src/libstd --script-overrides $(OVERRIDE_DIR) --output-dir $(dir $@) $(MINICARGO_FLAGS)
	MINICARGO_DYLIB=1 $(MINICARGO) $(RUSTCSRC)src/libpanic_unwind --script-overrides $(OVERRIDE_DIR) --output-dir $(dir $@) $(MINICARGO_FLAGS)
	MINICARGO_DYLIB=1 $(MINICARGO) $(RUSTCSRC)src/libtest --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(dir $@) $(MINICARGO_FLAGS)
	test -e $@

RUSTC_ENV_VARS := CFG_COMPILER_HOST_TRIPLE=$(RUSTC_TARGET)
RUSTC_ENV_VARS += LLVM_CONFIG=$(abspath $(LLVM_CONFIG))
RUSTC_ENV_VARS += CFG_RELEASE=
RUSTC_ENV_VARS += CFG_RELEASE_CHANNEL=$(RUSTC_CHANNEL)
RUSTC_ENV_VARS += CFG_VERSION=$(RUSTC_VERSION)-$(RUSTC_CHANNEL)-mrustc
RUSTC_ENV_VARS += CFG_PREFIX=mrustc
RUSTC_ENV_VARS += CFG_LIBDIR_RELATIVE=lib
RUSTC_ENV_VARS += LD_LIBRARY_PATH=$(abspath output)

$(OUTDIR)rustc: $(MRUSTC) $(MINICARGO) LIBS $(LLVM_CONFIG)
	mkdir -p $(OUTDIR)rustc-build
	$(RUSTC_ENV_VARS) $(MINICARGO) $(RUSTCSRC)src/rustc --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(OUTDIR)rustc-build -L $(OUTDIR) $(MINICARGO_FLAGS)
#	$(RUSTC_ENV_VARS) $(MINICARGO) $(RUSTCSRC)src/librustc_codegen_llvm --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(OUTDIR)rustc-build -L $(OUTDIR) $(MINICARGO_FLAGS)
	cp $(OUTDIR)rustc-build/$(RUSTC_OUT_BIN) $@
$(OUTDIR)cargo: $(MRUSTC) LIBS
	mkdir -p $(OUTDIR)cargo-build
	$(MINICARGO) $(RUSTCSRC)src/tools/cargo --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(OUTDIR)cargo-build -L $(OUTDIR) $(MINICARGO_FLAGS)
	cp $(OUTDIR)cargo-build/cargo $(OUTDIR)

# Reference $(RUSTCSRC)src/bootstrap/native.rs for these values
LLVM_CMAKE_OPTS := LLVM_TARGET_ARCH=$(firstword $(subst -, ,$(RUSTC_TARGET))) LLVM_DEFAULT_TARGET_TRIPLE=$(RUSTC_TARGET)
LLVM_CMAKE_OPTS += LLVM_TARGETS_TO_BUILD="$(LLVM_TARGETS)"
LLVM_CMAKE_OPTS += LLVM_ENABLE_ASSERTIONS=OFF
LLVM_CMAKE_OPTS += LLVM_INCLUDE_EXAMPLES=OFF LLVM_INCLUDE_TESTS=OFF LLVM_INCLUDE_DOCS=OFF
LLVM_CMAKE_OPTS += LLVM_ENABLE_ZLIB=OFF LLVM_ENABLE_TERMINFO=OFF LLVM_ENABLE_LIBEDIT=OFF WITH_POLLY=OFF
LLVM_CMAKE_OPTS += CMAKE_CXX_COMPILER="$(CXX)" CMAKE_C_COMPILER="$(CC)"
LLVM_CMAKE_OPTS += CMAKE_BUILD_TYPE=RelWithDebInfo


$(LLVM_CONFIG): $(RUSTCSRC)build/Makefile
	$Vcd $(RUSTCSRC)build && $(MAKE)
$(RUSTCSRC)build/Makefile: $(RUSTCSRC)src/llvm/CMakeLists.txt
	@mkdir -p $(RUSTCSRC)build
	$Vcd $(RUSTCSRC)build && cmake $(addprefix -D , $(LLVM_CMAKE_OPTS)) ../src/llvm


#
# Developement-only targets
#
$(OUTDIR)liballoc.rlib: $(MRUSTC) $(MINICARGO)
	$(MINICARGO) $(RUSTCSRC)src/liballoc --script-overrides $(OVERRIDE_DIR) --output-dir $(OUTDIR) $(MINICARGO_FLAGS)
$(OUTDIR)rustc-build/librustdoc.rlib: $(MRUSTC) LIBS
	$(MINICARGO) $(RUSTCSRC)src/librustdoc --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(dir $@) -L $(OUTDIR) $(MINICARGO_FLAGS)
#$(OUTDIR)cargo-build/libserde-1_0_6.rlib: $(MRUSTC) LIBS
#	$(MINICARGO) $(RUSTCSRC)src/vendor/serde --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(dir $@) -L $(OUTDIR) $(MINICARGO_FLAGS)
$(OUTDIR)cargo-build/libgit2-0_6_6.rlib: $(MRUSTC) LIBS
	$(MINICARGO) $(RUSTCSRC)src/vendor/git2 --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(dir $@) -L $(OUTDIR) --features ssh,https,curl,openssl-sys,openssl-probe $(MINICARGO_FLAGS)
$(OUTDIR)cargo-build/libserde_json-1_0_2.rlib: $(MRUSTC) LIBS
	$(MINICARGO) $(RUSTCSRC)src/vendor/serde_json --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(dir $@) -L $(OUTDIR) $(MINICARGO_FLAGS)
$(OUTDIR)cargo-build/libcurl-0_4_6.rlib: $(MRUSTC) LIBS
	$(MINICARGO) $(RUSTCSRC)src/vendor/curl --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(dir $@) -L $(OUTDIR) $(MINICARGO_FLAGS)
$(OUTDIR)cargo-build/libterm-0_4_5.rlib: $(MRUSTC) LIBS
	$(MINICARGO) $(RUSTCSRC)src/vendor/term --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(dir $@) -L $(OUTDIR) $(MINICARGO_FLAGS)
$(OUTDIR)cargo-build/libfailure-0_1_2.rlib: $(MRUSTC) LIBS
	$(MINICARGO) $(RUSTCSRC)src/vendor/failure --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(dir $@) -L $(OUTDIR) --features std,derive,backtrace,failure_derive $(MINICARGO_FLAGS)

#
# Testing
#
.PHONY: rust_tests-libs

LIB_TESTS := alloc std
LIB_TESTS += rustc_data_structures
rust_tests-libs: $(patsubst %,$(OUTDIR)stdtest/%-test_out.txt, $(LIB_TESTS)) $(OUTDIR)stdtest/collectionstests_out.txt
.PRECIOUS: $(OUTDIR)stdtest/alloc-test
.PRECIOUS: $(OUTDIR)stdtest/std-test
.PRECIOUS: $(OUTDIR)stdtest/rustc_data_structures-test

RUNTIME_ARGS_$(OUTDIR)stdtest/alloc-test := --test-threads 1
RUNTIME_ARGS_$(OUTdIR)stdtest/std-test := --test-threads 1
# VVV Requires panic destructors (unwinding panics)
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::io::stdio::tests::panic_doesnt_poison
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::mutex::tests::test_arc_condvar_poison
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::mutex::tests::test_mutex_arc_poison
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::once::tests::poison_bad
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::once::tests::wait_for_force_to_finish
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::rwlock::tests::test_rw_arc_no_poison_rw
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::rwlock::tests::test_rw_arc_poison_wr
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::rwlock::tests::test_rw_arc_poison_ww
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sys_common::remutex::tests::poison_works
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::thread::local::tests::dtors_in_dtors_in_dtors
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::thread::local::tests::smoke_dtor
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::mutex::tests::test_get_mut_poison
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::mutex::tests::test_into_inner_poison
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::mutex::tests::test_mutex_arc_access_in_unwind
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::rwlock::tests::test_get_mut_poison
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::rwlock::tests::test_into_inner_poison
RUNTIME_ARGS_$(OUTDIR)stdtest/std-test += --skip ::sync::rwlock::tests::test_rw_arc_access_in_unwind
RUNTIME_ARGS_$(OUTDIR)stdtest/rustc_data_structures-test := --test-threads 1

$(OUTDIR)stdtest/%-test: $(RUSTCSRC)src/lib%/lib.rs LIBS
	$(MINICARGO) --test $(RUSTCSRC)src/lib$* --vendor-dir $(RUSTCSRC)src/vendor --output-dir $(dir $@) -L $(OUTDIR)
$(OUTDIR)stdtest/collectionstests: $(OUTDIR)stdtest/alloc-test
	test -e $@
$(OUTDIR)collectionstest_out.txt: $(OUTDIR)%
$(OUTDIR)%_out.txt: $(OUTDIR)%
	@echo "--- [$<]"
	$V./$< $(RUNTIME_ARGS_$<) > $@ 2>&1 || (tail -n 1 $@; mv $@ $@_fail; false)
