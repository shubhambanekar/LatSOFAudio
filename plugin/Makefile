PLUGIN_NAME = LatSOFAudioPlugin
BUNDLE      = $(PLUGIN_NAME).driver
HALDIR      = /Library/Audio/Plug-Ins/HAL
DEST        = $(HALDIR)/$(BUNDLE)

SDKROOT = /Library/Developer/CommandLineTools/SDKs/MacOSX.sdk

CC   = clang
ARCH = x86_64
CFLAGS = \
	-arch $(ARCH) \
	-target $(ARCH)-apple-macos14.0 \
	-isysroot $(SDKROOT) \
	-O2 -Wall \
	-framework CoreFoundation \
	-framework CoreAudio \
	-framework IOKit

all: $(BUNDLE)

# --------------------------------------------------------------------------
# prep: the lesson from the -67030 saga.
#
# coreaudiod's driver-loading service (AMCP) validates each .driver bundle by
# hashing Info.plist as an UNPRIVILEGED user. A browser-downloaded Info.plist
# often carries mode 0600. `sudo cp` PRESERVES that mode, so root could read
# the file and sign it happily while the service could not read it at all --
# surfacing as errSecCSInfoPlistFailed (-67030), which reads like a code-
# signing policy rejection and sends you chasing amfi/library-validation
# knobs that were never the problem.
#
# Fix: normalise modes and strip quarantine xattrs BEFORE building, install
# with ditto (which does not carry over a restrictive source mode the way
# cp -r does), then sign the INSTALLED copy last and refuse to proceed
# unless the seal verifies.
# --------------------------------------------------------------------------
prep:
	@echo "=== prep: normalising modes + stripping xattrs ==="
	@chmod -R u+rwX,go+rX .
	@xattr -cr . 2>/dev/null || true

$(BUNDLE): $(PLUGIN_NAME).c Info.plist | prep
	@echo "=== Building $(BUNDLE) ==="
	mkdir -p $(BUNDLE)/Contents/MacOS
	$(CC) $(CFLAGS) -bundle -o $(BUNDLE)/Contents/MacOS/$(PLUGIN_NAME) $(PLUGIN_NAME).c
	cp Info.plist $(BUNDLE)/Contents/
	chmod -R u+rwX,go+rX $(BUNDLE)
	codesign --force --sign - $(BUNDLE)
	@codesign --verify --deep --strict $(BUNDLE) && echo "=== build seal OK ===" \
		|| { echo "!!! build seal FAILED"; exit 1; }

clean:
	rm -rf $(BUNDLE)

install: $(BUNDLE)
	@echo "=== Installing to $(DEST) ==="
	sudo mkdir -p $(HALDIR)
	sudo rm -rf $(DEST)
	sudo ditto $(BUNDLE) $(DEST)
	sudo chmod -R u+rwX,go+rX $(DEST)
	sudo xattr -cr $(DEST) 2>/dev/null || true
	sudo codesign --force --sign - $(DEST)
	@echo "--- verify gate ---"
	@codesign --verify --deep --strict $(DEST) \
		&& echo "SEAL-OK" \
		|| { echo "SEAL FAILED -- refusing to restart coreaudiod."; \
		     echo "Inspect with: codesign -dvvv $(DEST)"; exit 1; }
	sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod
	@sleep 2
	@$(MAKE) --no-print-directory status

uninstall:
	sudo rm -rf $(DEST)
	sudo launchctl kickstart -kp system/com.apple.audio.coreaudiod

# Confirm CoreAudio actually published the device.
status:
	@echo "--- installed bundle ---"
	@ls -ld $(DEST) 2>/dev/null || echo "  (not installed)"
	@ls -l  $(DEST)/Contents/Info.plist 2>/dev/null \
		| awk '{print "  Info.plist mode:", $$1}'
	@echo "--- signature ---"
	@codesign --verify --deep --strict $(DEST) 2>&1 \
		&& echo "  SEAL-OK" || echo "  SEAL FAILED"
	@echo "--- CoreAudio device list ---"
	@system_profiler SPAudioDataType 2>/dev/null | grep -i latsof \
		&& echo "  device published" \
		|| echo "  NOT published -- check: log show --last 2m --predicate 'process==\"coreaudiod\"' | grep -i cmlsof"

.PHONY: all prep clean install uninstall status
