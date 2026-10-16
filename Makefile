-include ../local.mk

LLVM_BIN        = $(shell $(LLVM_CONFIG) --bindir)
LLVM_LIB        = $(shell $(LLVM_CONFIG) --libdir)
LLVM_BUILD_ROOT = $(shell $(LLVM_CONFIG) --obj-root)
LLVM_SRC_ROOT   = $(LLVM_BUILD_ROOT)/../llvm
CXXFLAGS        = $(shell $(LLVM_CONFIG) --cxxflags) -fPIC -shared -O3
LDFLAGS         = $(shell $(LLVM_CONFIG) --ldflags)
X86_SRC_INC     = -I$(LLVM_SRC_ROOT)/lib/Target/X86
X86_BUILD_INC   = -I$(LLVM_BUILD_ROOT)/lib/Target/X86
CXX             = clang++
MIR_PLUGIN      = build/libX86ShadowStack.so
MIR_SRC         = X86ShadowStack.cpp

mir-plugin: $(MIR_SRC)
	mkdir -p build
	$(CXX) $(CXXFLAGS) $(X86_SRC_INC) $(X86_BUILD_INC) $(MIR_SRC) -o $(MIR_PLUGIN) $(LDFLAGS)

compdb:
	bear -- $(MAKE) -B mir-plugin

clean:
	rm -f $(MIR_PLUGIN)