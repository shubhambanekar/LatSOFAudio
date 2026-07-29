//
// latsof-setrate — pin the AppleHDA analog output to a fixed sample rate.
//
// WHY: on ALC laptops the analog output crackles/hisses at 44.1 kHz; 48 kHz
// is clean. macOS re-derives the rate whenever the output engine is rebuilt —
// every headphone plug or unplug, and some wake paths — and on the reference
// machine it settles back on 48 kHz by itself. Three cases do not: a cold boot
// with the jack already occupied, an engine left genuinely stuck at 44.1, and
// a call app that deliberately holds the output at 44.1 for the duration of a
// call (FaceTime does this) — which, against a 48 kHz-only microphone, makes
// CoreAudio bridge two mismatched clocks until its IO thread starts dropping
// buffers. For the first two, run this by hand once and then rebuild the engine
// (replug the jack, or sleep/wake). For the third, the rate change alone is the
// whole fix, and --enforce automates it.
//
// Usage:
//   latsof-setrate                    report the current rate and exit
//   latsof-setrate 48000              pin once and exit
//   latsof-setrate --enforce 48000    stay resident and keep it there (safely)
//
// The one-shot pin is quiet unless it changes something: on a change it prints
// one timestamped line — the device UID, then "44100 -> 48000 (status 0)" —
// and if the rate is already right it prints nothing. Either way it exits 0.
// The mode flag is only recognised as the first argument; anywhere else it is
// silently ignored and the tool behaves as a one-shot. "--watch" is accepted
// as an alias for "--enforce".
//
// A WARNING ABOUT --enforce, AND WHY IT IS SHAPED THE WAY IT IS.
// The first resident version of this tool wrote the rate immediately on every
// change notification, and it made things worse rather than better (reference
// machine, 29 Jul 2026): a jack plug makes AppleHDA pass through 44.1 kHz
// while it reprograms the codec, the watcher wrote 48 kHz into the middle of
// that, and stream and codec ended up disagreeing — harsh static on headphones
// AND speakers, curable only by physically replugging. That mode was retracted.
//
// --enforce is the rewrite, and its single safety property is that it reacts to
// QUIET rather than to change: notifications only restart a timer, and the rate
// is corrected solely after it has been wrong CONTINUOUSLY for kSettleSecs with
// no device activity in between. A jack plug settles by itself long before that
// window expires, so the dangerous case never fires; a call app holding the
// output at 44.1 does expire it, and gets corrected once. Verified both ways
// before shipping. If you modify this file, keep that property: never write the
// rate in a notification callback. See INSTALL.md §7.
//
// Note: a live rate change is not by itself a valid test of the crackle fix —
// the codec path is only fully reprogrammed when the engine is rebuilt, so
// sleep/wake (or reboot) after changing the rate before judging the result.
//
// Part of LatSOFAudio: https://github.com/shubhambanekar/LatSOFAudio
// Copyright (c) 2026 Shubham Banekar — BSD-3-Clause.
//
// Build and install (from the repo root):
//   clang -O2 -framework CoreAudio -framework CoreFoundation \
//         -o latsof-setrate contrib/latsof-setrate.c
//   sudo install -d -m 755 /usr/local/bin
//   sudo install -m 755 latsof-setrate /usr/local/bin/
//   rm latsof-setrate          # not covered by .gitignore — do not commit it
//
// /usr/local/bin is on the default PATH and is not SIP-protected, so the
// commands under "Usage" above work from any shell and survive macOS updates.
// No LaunchAgent, and nothing to keep running.
//

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define kMaxDevices 64
#define kTargetUID  "AppleHDAEngineOutput"   // the analog output engine

static Float64      gWant = 0.0;
static AudioDeviceID gWatched[kMaxDevices];
static int          gWatchedCount = 0;

static const AudioObjectPropertyAddress kRateAddr = {
    kAudioDevicePropertyNominalSampleRate,
    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
static const AudioObjectPropertyAddress kDevListAddr = {
    kAudioHardwarePropertyDevices,
    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
static const AudioObjectPropertyAddress kDefOutAddr = {
    kAudioHardwarePropertyDefaultOutputDevice,
    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
static const AudioObjectPropertyAddress kSourceAddr = {
    kAudioDevicePropertyDataSource,
    kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };

static void stamp(void) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    printf("%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static int device_uid(AudioDeviceID dev, char *out, size_t n) {
    CFStringRef uid = NULL; UInt32 sz = sizeof(uid);
    AudioObjectPropertyAddress u = { kAudioDevicePropertyDeviceUID,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    if (AudioObjectGetPropertyData(dev, &u, 0, NULL, &sz, &uid) != noErr || !uid)
        return 0;
    int ok = CFStringGetCString(uid, out, (CFIndex)n, kCFStringEncodingUTF8);
    CFRelease(uid);
    return ok;
}

static int list_devices(AudioDeviceID *devs, int max) {
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &kDevListAddr,
                                       0, NULL, &sz) != noErr) return 0;
    int n = (int)(sz / sizeof(AudioDeviceID));
    if (n > max) { n = max; sz = (UInt32)(n * sizeof(AudioDeviceID)); }
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &kDevListAddr,
                                   0, NULL, &sz, devs) != noErr) return 0;
    return n;
}

// Report, and (when gWant != 0) pin. Only writes when the rate actually
// differs, so the resulting change notification terminates instead of looping.
static void pin_all(int verbose) {
    AudioDeviceID devs[kMaxDevices];
    int n = list_devices(devs, kMaxDevices);
    for (int i = 0; i < n; i++) {
        char uid[256] = "?";
        if (!device_uid(devs[i], uid, sizeof(uid))) continue;
        if (!strstr(uid, kTargetUID)) continue;

        Float64 rate = 0; UInt32 sz = sizeof(rate);
        if (AudioObjectGetPropertyData(devs[i], &kRateAddr, 0, NULL, &sz, &rate)
            != noErr) continue;

        if (gWant == 0.0) {                      // report-only mode
            printf("%s  current=%.0f\n", uid, rate);
            continue;
        }
        if (rate == gWant) {
            if (verbose) { stamp(); printf("%s  already %.0f\n", uid, rate); }
            continue;
        }
        OSStatus s = AudioObjectSetPropertyData(devs[i], &kRateAddr, 0, NULL,
                                                sizeof(gWant), &gWant);
        stamp();
        printf("%s  %.0f -> %.0f (status %d)\n", uid, rate, gWant, (int)s);
    }
    fflush(stdout);
}

// ==================== --enforce: the debounced resident mode ====================
//
// The whole safety of this mode is one rule: NEVER write the rate while the
// audio engine might be rebuilding. The previous resident implementation wrote
// immediately on every change notification, which is precisely when AppleHDA
// is mid-way through reprogramming the codec after a jack plug — stream and
// codec then disagree and you get static that only another replug clears.
//
// So instead of reacting to changes, we react to *quiet*. Every device
// notification merely records "something moved just now". A timer checks once
// a second, and only corrects a rate that has been wrong CONTINUOUSLY for
// kSettleSecs with nothing else happening. The two cases separate cleanly:
//
//   jack plug   — 44.1 appears and AppleHDA settles back to 48 within ~1 s,
//                 well inside the settle window, so we never fire at all
//   call app    — FaceTime holds the output at 44.1 for the whole call, the
//                 window expires, and we correct it once
//
// A live rate change is safe here because nothing depends on reprogramming the
// codec path: the fault being fixed is a full-duplex rate MISMATCH between a
// 48 kHz input engine and a 44.1 kHz output engine, and it disappears the
// moment the two agree. See INSTALL.md §7.

#define kSettleSecs     8.0    // rate must be wrong this long, quietly, first
#define kPollSecs       1.0
#define kBurstLimit     5      // corrections per kBurstWindow before backing off
#define kBurstWindow   60.0
#define kBackoffSecs   60.0

static CFAbsoluteTime gLastActivity = 0;   // last device/rate notification
static CFAbsoluteTime gBurstStart   = 0;
static int            gBurstCount   = 0;
static CFAbsoluteTime gSettle       = kSettleSecs;

static void watch_devices(void);   // fwd

// Notifications do NOT act. They only note that the audio world just moved,
// which restarts the settle window.
static OSStatus on_change(AudioObjectID obj, UInt32 nAddr,
                          const AudioObjectPropertyAddress *addrs, void *ctx) {
    (void)obj; (void)nAddr; (void)addrs; (void)ctx;
    gLastActivity = CFAbsoluteTimeGetCurrent();
    watch_devices();      // the device set may have changed — refresh listeners
    return noErr;
}

// Is every analog output engine already at the target?
static int rate_is_wrong(void) {
    AudioDeviceID devs[kMaxDevices];
    int n = list_devices(devs, kMaxDevices);
    for (int i = 0; i < n; i++) {
        char uid[256] = "?";
        if (!device_uid(devs[i], uid, sizeof(uid))) continue;
        if (!strstr(uid, kTargetUID)) continue;
        Float64 rate = 0; UInt32 sz = sizeof(rate);
        if (AudioObjectGetPropertyData(devs[i], &kRateAddr, 0, NULL, &sz, &rate)
            != noErr) continue;
        if (rate != gWant) return 1;
    }
    return 0;
}

static void enforce_tick(CFRunLoopTimerRef t, void *info) {
    (void)t; (void)info;
    CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();

    if (!rate_is_wrong()) return;                  // nothing to do
    if (now - gLastActivity < gSettle) return;     // something moved recently

    // Runaway guard: if an app keeps yanking the rate back, stop fighting it
    // every few seconds and slow down, so the log stays readable and we never
    // become the noisy party.
    if (now - gBurstStart > kBurstWindow) { gBurstStart = now; gBurstCount = 0; }
    if (++gBurstCount > kBurstLimit) {
        if (gSettle != kBackoffSecs) {
            gSettle = kBackoffSecs;
            stamp();
            printf("something keeps resetting the rate — backing off to %.0fs\n",
                   kBackoffSecs);
            fflush(stdout);
        }
    } else if (gSettle != kSettleSecs && gBurstCount <= 1) {
        gSettle = kSettleSecs;
    }

    pin_all(0);                                    // prints only if it changed
    gLastActivity = CFAbsoluteTimeGetCurrent();    // our own write counts too
}

// (Re)register per-device listeners on every analog output engine present.
static void watch_devices(void) {
    for (int i = 0; i < gWatchedCount; i++) {
        AudioObjectRemovePropertyListener(gWatched[i], &kRateAddr, on_change, NULL);
        AudioObjectRemovePropertyListener(gWatched[i], &kSourceAddr, on_change, NULL);
    }
    gWatchedCount = 0;

    AudioDeviceID devs[kMaxDevices];
    int n = list_devices(devs, kMaxDevices);
    for (int i = 0; i < n && gWatchedCount < kMaxDevices; i++) {
        char uid[256] = "?";
        if (!device_uid(devs[i], uid, sizeof(uid))) continue;
        if (!strstr(uid, kTargetUID)) continue;
        AudioObjectAddPropertyListener(devs[i], &kRateAddr, on_change, NULL);
        AudioObjectAddPropertyListener(devs[i], &kSourceAddr, on_change, NULL);
        gWatched[gWatchedCount++] = devs[i];
    }
}

int main(int argc, char **argv) {
    int enforce = 0, argi = 1;
    if (argc > argi && (strcmp(argv[argi], "--enforce") == 0 ||
                        strcmp(argv[argi], "--watch") == 0)) {
        enforce = 1; argi++;      // --watch kept as an alias; same safe mode now
    }
    if (argc > argi) gWant = atof(argv[argi]);

    if (!enforce) {               // one-shot: report, and pin if asked
        pin_all(0);
        return 0;
    }
    if (gWant <= 0.0) {
        fprintf(stderr, "usage: latsof-setrate --enforce <rate>\n");
        return 2;
    }

    stamp();
    printf("enforcing %.0f Hz on %s (settle %.0fs)\n",
           gWant, kTargetUID, (double)kSettleSecs);
    fflush(stdout);

    gLastActivity = CFAbsoluteTimeGetCurrent();
    gBurstStart   = gLastActivity;

    pin_all(1);
    watch_devices();
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &kDevListAddr,
                                   on_change, NULL);
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &kDefOutAddr,
                                   on_change, NULL);

    CFRunLoopTimerRef t = CFRunLoopTimerCreate(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + kPollSecs,
        kPollSecs, 0, 0, enforce_tick, NULL);
    if (!t) { fprintf(stderr, "timer alloc failed\n"); return 1; }
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopDefaultMode);
    CFRelease(t);

    CFRunLoopRun();
    return 0;
}
