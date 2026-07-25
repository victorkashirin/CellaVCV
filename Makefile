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
SOURCES += $(wildcard src/frequency_analyzer/*.cpp)
SOURCES += $(wildcard src/spectrum/*.cpp)

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

SPECTRUM_TEST := build/spectrum_dsp_test
ifdef ARCH_MAC
SPECTRUM_TEST_RACK := build/libRack.dylib
SPECTRUM_TEST_RACK_SOURCE := $(RACK_DIR)/libRack.dylib
else ifdef ARCH_LIN
SPECTRUM_TEST_RACK := build/libRack.so
SPECTRUM_TEST_RACK_SOURCE := $(RACK_DIR)/libRack.so
else
SPECTRUM_TEST_RACK := build/libRack.dll
SPECTRUM_TEST_RACK_SOURCE := $(RACK_DIR)/libRack.dll
endif
ifndef ARCH_WIN
SPECTRUM_TEST_RPATH := -Wl,-rpath,$(abspath build)
endif

$(SPECTRUM_TEST_RACK): $(SPECTRUM_TEST_RACK_SOURCE)
	@mkdir -p build
ifdef ARCH_MAC
	cp -X $(SPECTRUM_TEST_RACK_SOURCE) $@
	xattr -c $@
	codesign -f -s - $@
else
	cp $(SPECTRUM_TEST_RACK_SOURCE) $@
endif

$(SPECTRUM_TEST): $(SPECTRUM_TEST_RACK) test/spectrum_dsp.cpp src/spectrum/SpectrumAnalyzer.cpp \
		src/spectrum/HistoryTimeline.cpp src/spectrum/SpectrumPresentation.cpp \
		src/spectrum/SpectrumAnalyzer.hpp src/spectrum/HistoryTimeline.hpp \
		src/spectrum/SpectrumPresentation.hpp src/spectrum/SpectrumTypes.hpp
	@mkdir -p build
	$(CXX) $(CXXFLAGS) -o $@ test/spectrum_dsp.cpp src/spectrum/SpectrumAnalyzer.cpp \
		src/spectrum/HistoryTimeline.cpp src/spectrum/SpectrumPresentation.cpp \
		-Lbuild -lRack $(SPECTRUM_TEST_RPATH)
ifdef ARCH_MAC
	xattr -c $@
	codesign -f -s - $@
endif

spectrum-test: $(SPECTRUM_TEST)
ifdef ARCH_MAC
	DYLD_LIBRARY_PATH="$(abspath build)" LD_LIBRARY_PATH="$(abspath build)" ./$(SPECTRUM_TEST)
else ifdef ARCH_LIN
	LD_LIBRARY_PATH="$(abspath build)" ./$(SPECTRUM_TEST)
else
	PATH="$(abspath build):$$PATH" ./$(SPECTRUM_TEST)
endif

.PHONY: spectrum-test
