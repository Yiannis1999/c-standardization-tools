LLVM_CONFIG ?= $(shell command -v llvm-config-18 || command -v llvm-config)
CXX := g++
CXXFLAGS := $(shell $(LLVM_CONFIG) --cxxflags)
INCLUDES := -I$(shell $(LLVM_CONFIG) --includedir)
LIBDIR := $(shell $(LLVM_CONFIG) --libdir)
LDFLAGS := -L$(LIBDIR) -Wl,-rpath,$(LIBDIR) $(shell $(LLVM_CONFIG) --ldflags --libs --system-libs)

ifneq ("$(wildcard $(LIBDIR)/libclang-cpp.so)","")
  LDLIBS := -lclang-cpp
else
  LLVM_VER := $(shell $(LLVM_CONFIG) --version | cut -d. -f1)
  LDLIBS := -Wl,--no-as-needed -l:libclang-cpp.so.$(LLVM_VER)
endif

all: macro-expand expr-simplify binop-reorder unop-simplify type-simplify implicit-cast-reveal size-check

macro-expand: macro-expand.cpp
	$(CXX) $< $(CXXFLAGS) $(INCLUDES) $(LDLIBS) $(LDFLAGS) -o $@

expr-simplify: expr-simplify.cpp
	$(CXX) $< $(CXXFLAGS) $(INCLUDES) $(LDLIBS) $(LDFLAGS) -o $@

binop-reorder: binop-reorder.cpp
	$(CXX) $< $(CXXFLAGS) $(INCLUDES) $(LDLIBS) $(LDFLAGS) -o $@

unop-simplify: unop-simplify.cpp
	$(CXX) $< $(CXXFLAGS) $(INCLUDES) $(LDLIBS) $(LDFLAGS) -o $@

type-simplify: type-simplify.cpp
	$(CXX) $< $(CXXFLAGS) $(INCLUDES) $(LDLIBS) $(LDFLAGS) -o $@

implicit-cast-reveal: implicit-cast-reveal.cpp
	$(CXX) $< $(CXXFLAGS) $(INCLUDES) $(LDLIBS) $(LDFLAGS) -o $@

size-check: size-check.cpp
	$(CXX) $< $(CXXFLAGS) $(INCLUDES) $(LDLIBS) $(LDFLAGS) -o $@

clean:
	$(RM) macro-expand expr-simplify binop-reorder unop-simplify type-simplify implicit-cast-reveal size-check
