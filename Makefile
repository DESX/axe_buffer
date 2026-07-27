# axe_buffer — fetch toolchain + Catch2 via graft, build, run the suite.

b  := build
DL := .cache

MAKEFLAGS := -j 8

all: test
.PHONY: all header test lint lint-fix format format-check clean clean-all

$(DL)/graft/graft.mk:; @git clone -q -c advice.detachedHead=false --depth=1 -b v1.3.0 https://github.com/DESX/graft.git $(dir $@)
include $(DL)/graft/graft.mk

# Fetched at build time, so the version is stamped in the recipe, not at parse time.
VTOOL_DIR     := $b/version_tool
VTOOL_TGT     := $(VTOOL_DIR)/version_tool.sh
VTOOL_COMMIT  := v1.0.0
VTOOL_GIT_URL := https://github.com/DESX/version_tool.git
$(eval $(call GRAFT_FETCH,VTOOL))
VT := $(VTOOL_TGT)

# Toolchain. By default graft fetches a pinned clang (Linux x86-64), which also
# provides clang-tidy / clang-format. Set SYS_CXX (e.g. `make test SYS_CXX=g++`)
# to build with a system compiler instead — used by CI and on non-x86 hosts.
# lint / format still require the default graft clang.
CLANG_DIR := $b/clang
ifeq ($(strip $(SYS_CXX)),)
CLANG_TGT     := $(CLANG_DIR)/bin/clang++
CLANG_TAR_URL := https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/LLVM-22.1.8-Linux-X64.tar.xz
$(eval $(call GRAFT_FETCH,CLANG))
CXX     := $(CLANG_TGT)
CXX_DEP := $(CLANG_TGT)
else
CXX     := $(SYS_CXX)
CXX_DEP :=
endif
CXXFLAGS := -std=c++20 -O2 -pthread

CATCH2_DIR     := $b/catch2
CATCH2_TGT     := $(CATCH2_DIR)/extras/catch_amalgamated.cpp
CATCH2_COMMIT  := v3.11.0
CATCH2_GIT_URL := https://github.com/catchorg/Catch2.git
$(eval $(call GRAFT_FETCH,CATCH2))
CATCH2_INC := -I$(CATCH2_DIR)/extras

# Distributable header = source + one injected #define AXE_BUFFER_VERSION.
GEN_HDR := $b/include/axe_buffer.hpp
$(GEN_HDR): axe_buffer.hpp $(VTOOL_TGT) | $b/include
	@v=`'$(VT)' -C . 2>/dev/null || echo v0.0.0`; \
	 sed "1a #define AXE_BUFFER_VERSION \"$$v\"" $< > $@; \
	 echo "  VERSION  $$v"

header: $(GEN_HDR)

$b/catch_amalgamated.o: $(CATCH2_TGT) $(CXX_DEP) | $b
	$(CXX) $(CXXFLAGS) $(CATCH2_INC) -c $(CATCH2_DIR)/extras/catch_amalgamated.cpp -o $@

# <axe_buffer.hpp> (angle include) resolves to the generated, versioned header.
$b/tests: tests.cpp $(GEN_HDR) $b/catch_amalgamated.o $(CXX_DEP) | $b
	$(CXX) $(CXXFLAGS) -I$b/include $(CATCH2_INC) tests.cpp $b/catch_amalgamated.o -o $@

test: $b/tests
	$b/tests

# clang-tidy / clang-format ship in the LLVM toolchain graft already fetches.
TIDY := $(CLANG_DIR)/bin/clang-tidy
FMT  := $(CLANG_DIR)/bin/clang-format
SRCS := axe_buffer.hpp tests.cpp

# clang-tidy config pulled from ClickHouse (overlapping goals: low-level, high-perf,
# concurrent C++), cached by graft, untracked. Lints the library header; '-*' clears
# ClickHouse's WarningsAsErrors '*' so findings report instead of failing the build.
TC_TGT := $b/.clang-tidy
TC_URL := https://raw.githubusercontent.com/ClickHouse/ClickHouse/master/.clang-tidy
$(eval $(call GRAFT_FETCH_FILE,TC))

lint lint-fix: TIDY_FLAGS = $(if $(filter lint-fix,$(MAKECMDGOALS)),--fix,)
lint lint-fix: $(TC_TGT) $(CLANG_TGT)
	$(TIDY) $(TIDY_FLAGS) --config-file=$(TC_TGT) --warnings-as-errors='-*' axe_buffer.hpp -- -std=c++20 -x c++ -Wno-pragma-once-outside-header

# Style passed inline — no .clang-format anywhere. Override e.g. `make format STYLE=Google`.
STYLE ?= "{BasedOnStyle: LLVM, ColumnLimit: 100}"
format: $(CLANG_TGT)            # rewrite SRCS in place
	$(FMT) -i --style=$(STYLE) $(SRCS)

format-check: $(CLANG_TGT)      # report drift, non-zero exit; no changes
	$(FMT) --dry-run --Werror --style=$(STYLE) $(SRCS)

clean:
	rm -rf $b

clean-all:
	rm -rf $b $(DL)

DIRS := $b $b/include $(DL) $(CLANG_DIR) $(CATCH2_DIR) $(VTOOL_DIR)
$(foreach d,$(sort $(DIRS)),$(eval $(call GRAFT_MK_DIR,$d)))
