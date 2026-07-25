# If RACK_DIR is not defined when calling the Makefile, default to two directories above
RACK_DIR ?= ../..

# FLAGS will be passed to both the C and C++ compiler
FLAGS += -I./deps/ebur128
FLAGS += -Ideps/ebur128/queue

CFLAGS +=
CXXFLAGS +=

# Careful about linking to shared libraries, since you can't assume much about the user's environment and library search path.
# Static libraries are fine, but they should be added to this plugin's build system.
LDFLAGS +=

# Add .cpp files to the build
SOURCES += $(wildcard src/*.cpp)
SOURCES += $(wildcard deps/ebur128/*.c)
SOURCES += $(wildcard src/spectrum/*.cpp)
SOURCES += $(wildcard src/waterfall/*.cpp)

# Add files to the ZIP package when running `make dist`
# The compiled plugin and "plugin.json" are automatically added.
DISTRIBUTABLES += res
DISTRIBUTABLES += $(wildcard LICENSE*)
DISTRIBUTABLES += $(wildcard presets)

include $(RACK_DIR)/arch.mk

ifdef ARCH_WIN
LDFLAGS += -lopengl32
endif

# Include the Rack plugin Makefile framework
include $(RACK_DIR)/plugin.mk

WATERFALL_TEST := build/waterfall_dsp_test
ifdef ARCH_MAC
WATERFALL_TEST_RACK := build/libRack.dylib
WATERFALL_TEST_RACK_SOURCE := $(RACK_DIR)/libRack.dylib
else ifdef ARCH_LIN
WATERFALL_TEST_RACK := build/libRack.so
WATERFALL_TEST_RACK_SOURCE := $(RACK_DIR)/libRack.so
else
WATERFALL_TEST_RACK := build/libRack.dll
WATERFALL_TEST_RACK_SOURCE := $(RACK_DIR)/libRack.dll
endif
ifndef ARCH_WIN
WATERFALL_TEST_RPATH := -Wl,-rpath,$(abspath build)
endif

$(WATERFALL_TEST_RACK): $(WATERFALL_TEST_RACK_SOURCE)
	@mkdir -p build
ifdef ARCH_MAC
	cp -X $(WATERFALL_TEST_RACK_SOURCE) $@
	xattr -c $@
	codesign -f -s - $@
else
	cp $(WATERFALL_TEST_RACK_SOURCE) $@
endif

$(WATERFALL_TEST): $(WATERFALL_TEST_RACK) test/waterfall_dsp.cpp src/waterfall/WaterfallAnalyzer.cpp \
		src/waterfall/WaterfallAnalyzer.hpp src/waterfall/WaterfallTypes.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ test/waterfall_dsp.cpp src/waterfall/WaterfallAnalyzer.cpp \
		-Lbuild -lRack $(WATERFALL_TEST_RPATH)
ifdef ARCH_MAC
	xattr -c $@
	codesign -f -s - $@
endif

waterfall-test: $(WATERFALL_TEST)
ifdef ARCH_MAC
	DYLD_LIBRARY_PATH="$(abspath build)" LD_LIBRARY_PATH="$(abspath build)" ./$(WATERFALL_TEST)
else ifdef ARCH_LIN
	LD_LIBRARY_PATH="$(abspath build)" ./$(WATERFALL_TEST)
else
	PATH="$(abspath build):$$PATH" ./$(WATERFALL_TEST)
endif

.PHONY: waterfall-test
