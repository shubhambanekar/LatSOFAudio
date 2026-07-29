//
// latsof-setrate — pin the AppleHDA analog output to a fixed sample rate.
//
// WHY: on ALC laptops the analog output crackles/hisses at 44.1 kHz; 48 kHz
// is clean. macOS re-derives the rate whenever the output engine is rebuilt —
// every headphone plug or unplug, and some wake paths — and on the reference
// machine it settles back on 48 kHz by itself. Two cases do not: a cold boot
// with the jack already occupied, and an engine left genuinely stuck at 44.1.
// This tool exists for those two cases: run it by hand, once, then rebuild the
// engine (replug the jack, or sleep/wake). Nothing here belongs in a startup
// item.
//
// Usage:
//   latsof-setrate                  report the current rate and exit
//   latsof-setrate 48000            pin once and exit
//   latsof-setrate --watch <rate>   RETRACTED — see the warning below
//
// The one-shot pin is quiet unless it changes something: on a change it prints
// one timestamped line — the device UID, then "44100 -> 48000 (status 0)" —
// and if the rate is already right it prints nothing. Either way it exits 0.
// "--watch" is only recognised as the first argument; anywhere else it is
// silently ignored and the tool behaves as a one-shot.
//
// DO NOT RUN --watch RESIDENT. It was written for a LaunchAgent (KeepAlive)
// and that experiment failed on the reference machine, 29 Jul 2026: a jack
// plug makes AppleHDA pass through 44.1 kHz while it reprograms the codec, the
// watcher writes 48 kHz into the middle of that, and stream and codec end up
// disagreeing — harsh static on headphones AND speakers, curable only by
// physically replugging (a coreaudiod restart does not clear it). The mode is
// kept only so the negative result stays reproducible. See INSTALL.md §7.
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

// A rebuilt engine may accept the rate and then revert it while it finishes
// initialising, so follow every event with a couple of delayed re-pins.
static void settle_fired(CFRunLoopTimerRef t, void *info) {
    (void)t; (void)info;
    pin_all(0);
}

static void schedule_settle(void) {
    const CFTimeInterval delays[] = { 0.4, 1.5, 4.0 };
    for (unsigned i = 0; i < sizeof(delays) / sizeof(delays[0]); i++) {
        CFRunLoopTimerRef t = CFRunLoopTimerCreate(
            kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + delays[i],
            0, 0, 0, settle_fired, NULL);
        if (!t) continue;
        CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopDefaultMode);
        CFRelease(t);
    }
}

static void watch_devices(void);   // fwd

static OSStatus on_change(AudioObjectID obj, UInt32 nAddr,
                          const AudioObjectPropertyAddress *addrs, void *ctx) {
    (void)obj; (void)nAddr; (void)addrs; (void)ctx;
    watch_devices();      // device set may have changed — refresh listeners
    pin_all(0);
    schedule_settle();
    return noErr;
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
    int watch = 0, argi = 1;
    if (argc > argi && strcmp(argv[argi], "--watch") == 0) { watch = 1; argi++; }
    if (argc > argi) gWant = atof(argv[argi]);

    if (!watch) {                 // one-shot: report, and pin if asked
        pin_all(0);
        return 0;
    }
    if (gWant <= 0.0) {
        fprintf(stderr, "usage: latsof-setrate --watch <rate>\n");
        return 2;
    }

    stamp();
    printf("watching %s, pinning %.0f Hz\n", kTargetUID, gWant);
    fflush(stdout);

    pin_all(1);
    watch_devices();
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &kDevListAddr,
                                   on_change, NULL);
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &kDefOutAddr,
                                   on_change, NULL);
    CFRunLoopRun();
    return 0;
}
