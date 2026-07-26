# LatSOFAudio kext Makefile
# Cross-compile from arm64 (M1 Mac) to x86_64 (Hackintosh)

# source dir + include path
KEXT_NAME = LatSOFAudio
KEXT_BUNDLE = $(KEXT_NAME).kext

# SDK and toolchain
SDKROOT = /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk
KERNEL_FW = $(SDKROOT)/System/Library/Frameworks/Kernel.framework

# Compiler settings
CC = clang
CXX = clang++
AS = clang
ARCH = x86_64
DEPLOYMENT_TARGET = 14.0

CXXFLAGS = \
	-arch $(ARCH) \
	-target $(ARCH)-apple-macos$(DEPLOYMENT_TARGET) \
	-isysroot $(SDKROOT) \
	-I$(KERNEL_FW)/Headers \
	-I$(KERNEL_FW)/Headers/IOKit \
	-I./$(KEXT_NAME) \
	-mkernel \
	-nostdlib \
	-std=c++17 \
	-fno-builtin \
	-fno-common \
	-fno-exceptions \
	-fno-rtti \
	-fno-stack-protector \
	-DKERNEL \
	-DKERNEL_PRIVATE \
	-DDRIVER_PRIVATE \
	-DAPPLE \
	-DNeXT \
	-D__MACOSX__ \
	-O2 \
	-Wall \
	-Wno-inconsistent-missing-override

ASFLAGS = \
	-arch $(ARCH) \
	-target $(ARCH)-apple-macos$(DEPLOYMENT_TARGET)

LDFLAGS = \
	-arch $(ARCH) \
	-target $(ARCH)-apple-macos$(DEPLOYMENT_TARGET) \
	-isysroot $(SDKROOT) \
	-Xlinker -kext \
	-nostdlib \
	-lcc_kext

# Source files
SRCDIR = $(KEXT_NAME)
SRCS = $(SRCDIR)/LatSOFAudioDevice.cpp
OBJS = $(SRCS:.cpp=.o) $(SRCDIR)/sof_firmware_embed.o

# Build targets
all: $(KEXT_BUNDLE)

$(KEXT_BUNDLE): $(OBJS)
	@echo "=== Linking $(KEXT_NAME) ==="
	mkdir -p $(KEXT_BUNDLE)/Contents/MacOS
	$(CXX) $(LDFLAGS) -o $(KEXT_BUNDLE)/Contents/MacOS/$(KEXT_NAME) $(OBJS)
	cp $(SRCDIR)/Info.plist $(KEXT_BUNDLE)/Contents/
	@echo "=== Build complete: $(KEXT_BUNDLE) ==="

%.o: %.cpp
	@echo "=== Compiling $< ==="
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(SRCDIR)/sof_firmware_embed.o: $(SRCDIR)/sof_firmware_embed.s $(SRCDIR)/Firmware/sof-cml.ri
	@echo "=== Embedding firmware ==="
	$(AS) $(ASFLAGS) -c $< -o $@

clean:
	rm -rf $(KEXT_BUNDLE) $(OBJS)

.PHONY: all clean
