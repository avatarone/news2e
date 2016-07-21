#Environment variables:
#
#  BUILD_ARCH=corei7, etc...
#      Overrides the default clang -march settings.
#      Useful to build S2E in VirtualBox or in other VMs that do not support
#      some advanced instruction sets.
#      Used by STP only for now.
#
#  EXTRA_QEMU_FLAGS=...
#      Pass additional flags to QEMU's configure script.
#
#  PARALLEL=no
#      Turn off build parallelization.
#
#  LLVMBUILD=...
#  Contains llvm-native, llvm-debug, llvm-release, and llvm source folders
#  Can be used to avoid rebuilding clang/llvm for every branch of S2E
#

S2ESRC := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
S2EBUILD:=$(CURDIR)
LLVMBUILD?=$(S2EBUILD)

OS := $(shell uname)
JOBS:=2

ifeq ($(PARALLEL),no)
JOBS := 1
else ifeq ($(OS),Darwin)
JOBS := $(patsubst hw.ncpu:%,%,$(shell sysctl hw.ncpu))
else ifeq ($(OS),Linux)
JOBS := $(shell grep -c ^processor /proc/cpuinfo)
endif
MAKE = make -j$(JOBS)

all: all-release guest-tools

guest-tools: stamps/guest-tools32-make stamps/guest-tools64-make

all-release all-asan-release: stamps/tools-release-make
all-release: stamps/qemu-release-make
all-asan-release: stamps/qemu-asan-release-make

all-debug all-asan-debug: stamps/tools-debug-make
all-debug: stamps/qemu-debug-make
all-asan-debug: stamps/qemu-asan-debug-make

ifeq ($(wildcard qemu/vl.c),qemu/vl.c)
    $(error You should not run make in the S2E source directory!)
endif


LLVM_VERSION=3.4
LLVM_SRC=llvm-$(LLVM_VERSION).src.tar.gz
LLVM_SRC_DIR=llvm-$(LLVM_VERSION)
LLVM_NATIVE_SRC_DIR=llvm-$(LLVM_VERSION).src.native
CLANG_SRC=clang-$(LLVM_VERSION).src.tar.gz
CLANG_SRC_DIR=clang-$(LLVM_VERSION)
CLANG_DEST_DIR=$(LLVM_NATIVE_SRC_DIR)/tools/clang
COMPILER_RT_SRC=compiler-rt-$(LLVM_VERSION).src.tar.gz
COMPILER_RT_SRC_DIR=compiler-rt-$(LLVM_VERSION)
COMPILER_RT_DEST_DIR=$(LLVM_NATIVE_SRC_DIR)/projects/compiler-rt
GIT ?= git

KLEE_QEMU_DIRS = $(foreach suffix,-debug -release -asan,$(addsuffix $(suffix),klee qemu))

MINISAT_DIRS = $(foreach suffix,-debug -release -asan, $(addsuffix $(suffix),minisat))
STP_DIRS = $(foreach suffix,-debug -release -asan, $(addsuffix $(suffix),stp))


ifeq ($(LLVMBUILD),$(S2EBUILD))
LLVM_DIRS = llvm-debug llvm-native llvm-release
endif

clean:
	-rm -Rf $(KLEE_QEMU_DIRS)
	-rm -Rf stamps

guestclean:
	-rm -f stamps/guest-tools*
	-rm -rf guest-tools*

distclean: clean guestclean
	-rm -Rf guest-tools $(LLVM_DIRS) stp stp-asan tools-debug tools-release
	-rm -Rf $(COMPILER_RT_SRC_DIR) $(LLVM_SRC_DIR) $(LLVM_NATIVE_SRC_DIR)

.PHONY: all all-debug all-release all-asan-debug all-asan-release
.PHONY: clean distclean guestclean

ALWAYS:

guest-tools32 guest-tools64 $(KLEE_QEMU_DIRS) $(LLVM_DIRS) $(MINISAT_DIRS) $(STP_DIRS) stamps tools-debug tools-release:
	mkdir -p $@

stamps/%-configure: | % stamps
	cd $* && $(CONFIGURE_COMMAND)
	touch $@

stamps/%-make:
	$(MAKE) -C $* $(BUILD_OPTS)
	touch $@

#############
# Downloads #
#############

ifeq ($(LLVMBUILD),$(S2EBUILD))
LLVM_SRC_URL = http://llvm.org/releases/$(LLVM_VERSION)/

$(CLANG_SRC) $(COMPILER_RT_SRC) $(LLVM_SRC):
	wget $(LLVM_SRC_URL)$@

.INTERMEDIATE: $(CLANG_SRC_DIR) $(COMPILER_RT_SRC_DIR)

$(CLANG_SRC_DIR): $(CLANG_SRC)
$(COMPILER_RT_SRC_DIR): $(COMPILER_RT_SRC)
$(LLVM_SRC_DIR): $(LLVM_SRC)
	( cd $(LLVM_SRC_DIR) && patch -p1 < $(S2ESRC)/patches/llvm_system_error_h_cast.patch )
	tar -xmzf $<

$(CLANG_SRC_DIR) $(COMPILER_RT_SRC_DIR):
	tar -xmzf $<

$(LLVM_NATIVE_SRC_DIR): $(LLVM_SRC_DIR)
$(LLVM_NATIVE_SRC_DIR):
	cp -r $< $@

$(CLANG_DEST_DIR): $(CLANG_SRC_DIR) $(LLVM_NATIVE_SRC_DIR)
$(COMPILER_RT_DEST_DIR): $(COMPILER_RT_SRC_DIR) $(LLVM_NATIVE_SRC_DIR)
$(CLANG_DEST_DIR) $(COMPILER_RT_DEST_DIR):
	mv $< $@
endif

########
# LLVM #
########

CLANG_CC=$(LLVMBUILD)/llvm-native/Release/bin/clang
CLANG_CXX=$(LLVMBUILD)/llvm-native/Release/bin/clang++

ifeq ($(LLVMBUILD),$(S2EBUILD))
LLVM_CONFIGURE_FLAGS = --prefix=$(S2EBUILD)/opt \
                       --enable-jit --enable-optimized \

#First build it with the system's compiler
stamps/llvm-native-configure: $(CLANG_DEST_DIR) $(COMPILER_RT_DEST_DIR)
stamps/llvm-native-configure: CONFIGURE_COMMAND = $(S2EBUILD)/$(LLVM_NATIVE_SRC_DIR)/configure \
                                                  $(LLVM_CONFIGURE_FLAGS) \
                                                  --disable-assertions #compiler-rt won't build if we specify explicit targets...

stamps/llvm-native-make: stamps/llvm-native-configure
stamps/llvm-native-make: BUILD_OPTS = ENABLE_OPTIMIZED=1

#Then, build LLVM with the clang compiler.
#Note that we build LLVM without clang and compiler-rt, because S2E does not need them.
stamps/llvm-debug-configure: stamps/llvm-native-make
stamps/llvm-release-configure: stamps/llvm-native-make
stamps/llvm-%-configure: CONFIGURE_COMMAND = $(S2EBUILD)/$(LLVM_SRC_DIR)/configure \
                                             $(LLVM_CONFIGURE_FLAGS) \
                                             --target=x86_64 --enable-targets=x86 \
                                             CC=$(CLANG_CC) \
                                             CXX=$(CLANG_CXX)

stamps/llvm-debug-make: stamps/llvm-debug-configure
stamps/llvm-debug-make: BUILD_OPTS = ENABLE_OPTIMIZED=0 REQUIRES_RTTI=1
stamps/llvm-release-make: stamps/llvm-release-configure
stamps/llvm-release-make: BUILD_OPTS = ENABLE_OPTIMIZED=1 REQUIRES_RTTI=1

else
stamps/llvm-release-make stamps/llvm-debug-make stamps/llvm-native-make:
	@echo "Won't build $@, using $(LLVMBUILD) folder"
endif

###########
# Minisat #
###########


$(S2ESRC)/minisat/CMakeLists.txt:
	( cd $(S2ESRC) && $(GIT) submodule update --init minisat )

stamps/minisat-debug-configure: CMAKE_BUILD_TYPE = Debug

stamps/minisat-release-configure: CMAKE_BUILD_TYPE = Release

stamps/minisat-asan-configure: ASAN_FLAGS = -fsanitize=address -fno-optimize-sibling-calls -fno-omit-frame-pointer
stamps/minisat-asan-configure: CMAKE_BUILD_TYPE = Debug

stamps/minisat-%-configure: CONFIGURE_COMMAND = cmake -G "Unix Makefiles" \
	-DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
	-DCMAKE_C_FLAGS="-fPIC $(ASAN_FLAGS)" \
	-DCMAKE_CXX_FLAGS="-fPIC $(ASAN_FLAGS)" \
	-DCMAKE_MODULE_LINKER_FLAGS="$(ASAN_FLAGS)" \
	-DCMAKE_SHARED_LINKER_FLAGS="$(ASAN_FLAGS)" \
	-DCMAKE_EXE_LINKER_FLAGS="$(ASAN_FLAGS)" \
	-DCMAKE_C_COMPILER="$(CLANG_CC)" \
	-DCMAKE_CXX_COMPILER="$(CLANG_CXX)" \
	$(S2ESRC)/minisat/ 

stamps/minisat-debug-configure: $(S2ESRC)/minisat/CMakeLists.txt stamps/llvm-native-make
stamps/minisat-asan-configure: $(S2ESRC)/minisat/CMakeLists.txt stamps/llvm-native-make
stamps/minisat-release-configure: $(S2ESRC)/minisat/CMakeLists.txt stamps/llvm-native-make

stamps/minisat-debug-make: stamps/minisat-debug-configure
stamps/minisat-asan-make: stamps/minisat-asan-configure
stamps/minisat-release-make: stamps/minisat-release-configure

#######
# STP #
#######

$(S2ESRC)/stp/CMakeLists.txt:
	( cd $(S2ESRC) && $(GIT) submodule update --init stp )

stamps/stp-debug-configure: stamps/minisat-debug-make
stamps/stp-asan-configure: stamps/minisat-asan-make
stamps/stp-release-configure: stamps/minisat-release-make
stamps/stp-%-configure: $(S2ESRC)/stp/CMakeLists.txt

stamps/stp-debug-configure: CMAKE_BUILD_TYPE = Debug
stamps/stp-debug-configure: BUILD_TYPE = debug

stamps/stp-asan-configure: CMAKE_BUILD_TYPE = Debug
stamps/stp-asan-configure: BUILD_TYPE = asan
stamps/stp-asan-configure: ASAN_FLAGS = -fsanitize=address -fno-optimize-sibling-calls -fno-omit-frame-pointer

stamps/stp-release-configure: CMAKE_BUILD_TYPE = Release
stamps/stp-release-configure: BUILD_TYPE = release


stamps/stp-asan-configure: CONFIGURE_COMMAND = cmake -G "Unix Makefiles" \
	-DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
	-DMINISAT_LIBRARY=$(S2EBUILD)/minisat-$(BUILD_TYPE)/libminisat.a \
	-DMINISAT_INCLUDE_DIR=$(S2ESRC)/minisat/ \
	-DCMAKE_C_FLAGS="-fPIC $(ASAN_FLAGS)" \
	-DCMAKE_CXX_FLAGS="-fPIC $(ASAN_FLAGS)" \
	-DCMAKE_MODULE_LINKER_FLAGS="$(ASAN_FLAGS)" \
	-DCMAKE_SHARED_LINKER_FLAGS="$(ASAN_FLAGS)" \
	-DCMAKE_EXE_LINKER_FLAGS="$(ASAN_FLAGS)" \
	-DCMAKE_C_COMPILER="$(CLANG_CC)" \
	-DCMAKE_CXX_COMPILER="$(CLANG_CXX)" \
	-DCMAKE_EXE_LINKER_FLAGS="$(ASAN_FLAGS)" \
	-DBUILD_STATIC_BIN=OFF \
	-DBUILD_SHARED_LIBS=ON \
	$(S2ESRC)/stp/

stamps/stp-debug-configure: CONFIGURE_COMMAND = cmake -G "Unix Makefiles" \
	-DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
	-DMINISAT_LIBRARY=$(S2EBUILD)/minisat-$(BUILD_TYPE)/libminisat.a \
	-DMINISAT_INCLUDE_DIR=$(S2ESRC)/minisat/ \
	-DCMAKE_C_FLAGS="-fPIC" \
	-DCMAKE_CXX_FLAGS="-fPIC" \
	-DCMAKE_C_COMPILER="$(CLANG_CC)" \
	-DCMAKE_CXX_COMPILER="$(CLANG_CXX)" \
	-DBUILD_STATIC_BIN=ON \
	-DBUILD_SHARED_LIBS=OFF \
	$(S2ESRC)/stp/

stamps/stp-release-configure: CONFIGURE_COMMAND = cmake -G "Unix Makefiles" \
	-DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
	-DMINISAT_LIBRARY=$(S2EBUILD)/minisat-$(BUILD_TYPE)/libminisat.a \
	-DMINISAT_INCLUDE_DIR=$(S2ESRC)/minisat/ \
	-DCMAKE_C_FLAGS="-fPIC" \
	-DCMAKE_CXX_FLAGS="-fPIC" \
	-DCMAKE_C_COMPILER="$(CLANG_CC)" \
	-DCMAKE_CXX_COMPILER="$(CLANG_CXX)" \
	-DBUILD_STATIC_BIN=ON \
	-DBUILD_SHARED_LIBS=OFF \
	$(S2ESRC)/stp/

stamps/stp-debug-make: stamps/stp-debug-configure
stamps/stp-asan-make: stamps/stp-asan-configure
stamps/stp-release-make: stamps/stp-release-configure

########
# KLEE #
########

KLEE_CONFIGURE_COMMAND = $(S2ESRC)/klee/configure

$(KLEE_CONFIGURE_COMMAND):
	( cd $(S2ESRC) && $(GIT) submodule update --init klee )

#stamps/klee-debug-make stamps/klee-asan-debug-make stamps/klee-release-make stamps/klee-asan-release-make: ALWAYS

KLEE_CONFIGURE_COMMON = --prefix=$(S2EBUILD)/opt \
                        --with-llvmsrc=$(LLVMBUILD)/$(LLVM_SRC_DIR) \
                        --target=x86_64 \
						--with-uclibc="" \
						--disable-posix-runtime \
                        CC=$(CLANG_CC) CXX=$(CLANG_CXX)


stamps/klee-asan-configure: ASAN_FLAGS = -fsanitize=address -fno-optimize-sibling-calls -fno-omit-frame-pointer
stamps/klee-asan-configure: BUILD_TYPE = asan
stamps/klee-debug-configure: BUILD_TYPE = debug
stamps/klee-release-configure: BUILD_TYPE = release


stamps/klee-debug-configure: stamps/stp-debug-make stamps/llvm-debug-make $(KLEE_CONFIGURE_COMMAND)
stamps/klee-asan-configure: stamps/stp-asan-make stamps/llvm-debug-make $(KLEE_CONFIGURE_COMMAND)
stamps/klee-release-configure: stamps/stp-release-make stamps/llvm-release-make $(KLEE_CONFIGURE_COMMAND)

stamps/klee-debug-configure: CONFIGURE_COMMAND = $(KLEE_CONFIGURE_COMMAND) \
                                                 --with-llvmobj=$(LLVMBUILD)/llvm-debug \
												 --with-stp=$(S2EBUILD)/stp-debug \
                                                 CXXFLAGS="-g -O0 $(CXXCONFIG_CXXFLAGS)" \
												 LDFLAGS="-g -L$(S2EBUILD)/minisat-debug" \
												 --with-llvm-build-mode="Debug+Asserts" \
												 $(KLEE_CONFIGURE_COMMON)

stamps/klee-asan-configure: CONFIGURE_COMMAND = $(KLEE_CONFIGURE_COMMAND) \
                                                --with-llvmobj=$(LLVMBUILD)/llvm-debug \
												--with-stp=$(S2EBUILD)/stp-asan \
                                                CXXFLAGS="-g -O0 $(ASAN_FLAGS) $(CXXCONFIG_CXXFLAGS)" \
                                                LDFLAGS="-g $(ASAN_FLAGS) -L$(S2EBUILD)/minisat-asan" \
                                                --with-llvm-build-mode="Debug+Asserts" \
												$(KLEE_CONFIGURE_COMMON)

stamps/klee-release-configure: CONFIGURE_COMMAND = $(KLEE_CONFIGURE_COMMAND) \
                                                   --with-llvmobj=$(LLVMBUILD)/llvm-release \
                                                   --with-stp=$(S2EBUILD)/stp-release \
												   CXXFLAGS="$(CXXCONFIG_CXXFLAGS)" \
												   LDFLAGS="-L$(S2EBUILD)/minisat-release" \
												   --with-llvm-build-mode="Release+Asserts" \
												   $(KLEE_CONFIGURE_COMMON)

stamps/klee-debug-make: stamps/klee-debug-configure
stamps/klee-debug-make: BUILD_OPTS = ENABLE_OPTIMIZED=0

stamps/klee-release-make: stamps/klee-release-configure
stamps/klee-release-make: BUILD_OPTS = ENABLE_OPTIMIZED=1

stamps/klee-asan-make: stamps/klee-asan-configure
stamps/klee-asan-make: BUILD_OPTS = ENABLE_OPTIMIZED=0

########
# QEMU #
########

#Use special include folder for c++config.h on Ubuntu 14.04
EXTRA_QEMU_FLAGS += --extra-cflags="$(CXXCONFIG_CXXFLAGS)" --extra-cxxflags="$(CXXCONFIG_CXXFLAGS)"

QEMU_S2E_ARCH =
QEMU_S2E_ARCH += i386-s2e-softmmu i386-softmmu
QEMU_S2E_ARCH += x86_64-s2e-softmmu x86_64-softmmu
QEMU_S2E_ARCH += arm-s2e-softmmu arm-softmmu

empty :=
comma := ,
space := $(empty) $(empty)
QEMU_S2E_ARCH := $(subst $(space),$(comma),$(strip $(QEMU_S2E_ARCH)))

QEMU_CONFIGURE_COMMAND := $(S2ESRC)/qemu/configure

QEMU_COMMON_FLAGS = --prefix=$(S2EBUILD)/opt\
                    --cc=$(CLANG_CC) \
                    --cxx=$(CLANG_CXX) \
                    --target-list=$(QEMU_S2E_ARCH)\
                    --enable-llvm \
                    --enable-s2e \
                    --with-pkgversion=S2E \
                    --disable-virtfs \
					--with-klee-src=$(S2ESRC)/klee

QEMU_CONFIGURE_FLAGS = --clang=$(CLANG_CC) \
                       $(QEMU_COMMON_FLAGS)

$(QEMU_CONFIGURE_COMMAND):
	( cd $(S2ESRC) && $(GIT) submodule update --init qemu )

QEMU_ASAN_FLAGS = -fstack-protector-all -Wno-mismatched-tags -fsanitize=address \
                  -fno-optimize-sibling-calls -fno-omit-frame-pointer

stamps/qemu-debug-configure: stamps/klee-debug-make $(QEMU_CONFIGURE_COMMAND)
stamps/qemu-asan-configure: stamps/klee-asan-make $(QEMU_CONFIGURE_COMMAND)
stamps/qemu-release-configure: stamps/klee-release-make $(QEMU_CONFIGURE_COMMAND)

stamps/qemu-debug-configure: CONFIGURE_COMMAND = $(QEMU_CONFIGURE_COMMAND) \
                                                 $(QEMU_CONFIGURE_FLAGS) \
                                                 --with-llvm-config=$(LLVMBUILD)/llvm-debug/Debug+Asserts/bin/llvm-config \
												 --with-stp=$(S2EBUILD)/stp-debug/ \
                                                 --with-klee=$(S2EBUILD)/klee-debug/Debug+Asserts \
                                                 --enable-debug \
                                                 --extra-cflags="$(CXXCONFIG_CXXFLAGS)" \
												 --extra-cxxflags="$(CXXCONFIG_CXXFLAGS)" 

stamps/qemu-asan-configure: CONFIGURE_COMMAND = $(QEMU_CONFIGURE_COMMAND) \
                                                $(QEMU_CONFIGURE_FLAGS) \
												--with-stp=$(S2EBUILD)/stp-asan/ \
                                                --with-llvm-config=$(LLVMBUILD)/llvm-debug/Debug+Asserts/bin/llvm-config \
                                                --with-klee=$(S2EBUILD)/klee-asan/Debug+Asserts \
                                                --enable-debug \
                                                --extra-cflags="$(CXXCONFIG_CXXFLAGS) $(QEMU_ASAN_FLAGS)" \
												--extra-cxxflags="$(CXXCONFIG_CXXFLAGS) $(QEMU_ASAN_FLAGS)" \
												--extra-ldflags="$(QEMU_ASAN_FLAGS)" 


stamps/qemu-release-configure: CONFIGURE_COMMAND = $(QEMU_CONFIGURE_COMMAND) \
                                                   $(QEMU_CONFIGURE_FLAGS) \
												   --with-stp=$(S2EBUILD)/stp-debug/ \
                                                   --with-llvm-config=$(LLVMBUILD)/llvm-release/Release+Asserts/bin/llvm-config \
                                                   --with-klee=$(S2EBUILD)/klee-release/Release+Asserts \
                                                   --extra-cflags="$(CXXCONFIG_CXXFLAGS)" \
												   --extra-cxxflags="$(CXXCONFIG_CXXFLAGS)" 

stamps/qemu-debug-make: stamps/qemu-debug-configure
stamps/qemu-asan-make: stamps/qemu-asan-configure
stamps/qemu-release-make: stamps/qemu-release-configure

#########
# Tools #
#########

TOOLS_CONFIGURE_COMMAND = $(S2ESRC)/tools/configure \
                          --with-llvmsrc=$(LLVMBUILD)/$(LLVM_SRC_DIR) \
                          --with-s2esrc=$(S2ESRC)/qemu \
                          --target=x86_64 CC=$(CLANG_CC) CXX=$(CLANG_CXX)

stamps/tools-debug-configure: stamps/llvm-debug-make $(QEMU_CONFIGURE_COMMAND)
stamps/tools-debug-configure: CONFIGURE_COMMAND = $(TOOLS_CONFIGURE_COMMAND) \
                                                  --with-llvmobj=$(LLVMBUILD)/llvm-debug

stamps/tools-release-configure: stamps/llvm-release-make $(QEMU_CONFIGURE_COMMAND)
stamps/tools-release-configure: CONFIGURE_COMMAND = $(TOOLS_CONFIGURE_COMMAND) \
                                                    --with-llvmobj=$(LLVMBUILD)/llvm-release

stamps/tools-debug-make stamps/tools-release-make: ALWAYS

stamps/tools-debug-make: stamps/tools-debug-configure
stamps/tools-debug-make: BUILD_OPTS = ENABLE_OPTIMIZED=0 REQUIRES_RTTI=1

stamps/tools-release-make: stamps/tools-release-configure
stamps/tools-release-make: BUILD_OPTS = ENABLE_OPTIMIZED=1 REQUIRES_RTTI=1



############
#Guest tools
############

GUEST_CONFIGURE_COMMAND = $(S2ESRC)/guest/configure

stamps/guest-tools%-configure: CONFIGURE_COMMAND = $(GUEST_CONFIGURE_COMMAND)

stamps/guest-tools32-make: stamps/guest-tools32-configure
stamps/guest-tools32-make: BUILD_OPTS = CFLAGS="-m32"

stamps/guest-tools64-make: stamps/guest-tools64-configure
stamps/guest-tools64-make: BUILD_OPTS = CFLAGS="-m64"
