//
// latsof-afgwake — keep the ALC236 Audio Function Group out of D3 while audio
// is actually playing, so the headphone path can never be driven powered-down.
//
// WHY: measured 1 Aug 2026. Plug headphones while the output engine is IDLE,
// then press play, and you get static that only a replug clears. A codec dump
// taken during the static, diffed against one taken after the curing replug,
// differed in exactly two lines: HP pin 0x21 and HP DAC 0x03 read
// GET_POWER_STATE = 0x30 — requested D0, ACTUALLY still D3. Everything else
// (pin control 0xc0, EAPD, amp gains, connect-sel, stream format) was
// byte-identical, which rules out the sample rate, the amp, and the routing.
//
// The blocker is one level up: node 0x01, the Audio Function Group, sat at
// 0x233 (requested D3 / actual D3). In HDA the AFG is the parent of every
// widget, so while it is down no child can reach D0 — direct writes to
// 0x21/0x03 return success and change nothing. AppleHDA drops the AFG when the
// engine idles and, on the idle-jack-insert path, starts streaming without
// restoring it. Replugging forces a full re-init, which is the entire reason
// replugging "fixes" the static.
//
// WHY THIS IS EVENT-DRIVEN BUT STILL SAFE. This project shipped a resident
// tool once that wrote on every change notification (a sample-rate pinner) and
// it made static WORSE, because it wrote into the window where AppleHDA was
// mid-reprogramming the codec. The rule learned there was "act on a wrong
// state, never on a transition". This tool keeps that rule: a CoreAudio
// notification is only ever a hint to go and LOOK. The write happens solely
// when the AFG is observed outside D0 while the device is running — a state
// that is unambiguously wrong and cannot occur legitimately. And unlike a rate
// write, SET_POWER_STATE D0 only ever powers UP: it is idempotent and cannot
// leave two pieces of state disagreeing. When the device is genuinely idle,
// D3 is CORRECT and is left alone, so idle power saving is preserved.
//
// This replaces the shell version (latsof-afgwake.sh): no polling loop, no
// ioreg/alc-verb subprocesses. It sleeps in CFRunLoop until CoreAudio says a
// device started, and talks to the codec directly through AppleALC's user
// client — the same path alc-verb uses.
//
// Requires boot-arg alcverbs=1 (AppleALC exposes ALCUserClientProvider).
//
// Usage:
//   latsof-afgwake              watch forever (what the LaunchAgent runs)
//   latsof-afgwake --once       check once, correct if wrong, exit
//   latsof-afgwake --status     report and exit, never write
//
// Build:
//   clang -O2 -framework IOKit -framework CoreAudio -framework CoreFoundation \
//         -o latsof-afgwake contrib/latsof-afgwake.c
//
// Part of LatSOFAudio: https://github.com/shubhambanekar/LatSOFAudio
// Copyright (c) 2026 Shubham Banekar — BSD-3-Clause.
//

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Must match AppleALC's UserKernelShared.h and its IOClass name.
#define kALCUserClientProvider  "ALCUserClientProvider"
#define kMethodExecuteVerb      0

#define AFG_NID                 0x01     // Audio Function Group
#define HP_PIN_NID              0x21     // headphone pin (diagnostics only)
#define HP_DAC_NID              0x03     // its DAC          (diagnostics only)
#define VERB_GET_POWER_STATE    0x0f05
#define VERB_SET_POWER_STATE    0x0705
#define PS_D0                   0x00

// A backstop in case a start slips past the notifications entirely. Long on
// purpose: the notification path is the mechanism, this is only a safety net.
#define kBackstopSecs           15.0

static int gVerbose = 1;

static void stamp(void) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    printf("%02d:%02d:%02d ", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

// ==================== codec access (AppleALC user client) ====================

static io_connect_t gPort = IO_OBJECT_NULL;

// Open the first ALCUserClientProvider. Returns false when AppleALC did not
// publish one — i.e. the alcverbs=1 boot-arg is missing.
static bool codec_open(void) {
    if (gPort != IO_OBJECT_NULL) return true;

    CFMutableDictionaryRef dict = IOServiceMatching(kALCUserClientProvider);
    if (!dict) return false;

    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, dict, &it) != KERN_SUCCESS)
        return false;

    io_service_t svc = IOIteratorNext(it);
    IOObjectRelease(it);
    if (!svc) return false;

    kern_return_t kr = IOServiceOpen(svc, mach_task_self(), 0, &gPort);
    IOObjectRelease(svc);
    if (kr != kIOReturnSuccess) { gPort = IO_OBJECT_NULL; return false; }
    return true;
}

static void codec_close(void) {
    if (gPort != IO_OBJECT_NULL) { IOServiceClose(gPort); gPort = IO_OBJECT_NULL; }
}

// Execute one verb. Returns the codec response, or -1 on failure.
// The 12-bit/4-bit conversion mirrors alc-verb: Apple's executeVerb discards
// the top 8 bits of param unless a 4-bit verb is passed in Intel form.
static long codec_verb(uint32_t nid, uint32_t verb, uint32_t param) {
    if (!codec_open()) return -1;

    uint64_t in[3];
    in[0] = nid;
    in[1] = (verb & 0xff) ? verb : (verb >> 8);
    in[2] = param;

    uint64_t out = 0; uint32_t outCount = 1;
    kern_return_t kr = IOConnectCallScalarMethod(gPort, kMethodExecuteVerb,
                                                 in, 3, &out, &outCount);
    if (kr != kIOReturnSuccess) {
        // The provider can disappear across sleep/kext reload — reopen next time.
        codec_close();
        return -1;
    }
    return (long)out;
}

// GET_POWER_STATE: requested state in bits 3:0, ACTUAL state in bits 7:4.
// Returns the actual state, or -1 if unreadable.
static int power_actual(uint32_t nid) {
    long r = codec_verb(nid, VERB_GET_POWER_STATE, 0);
    if (r < 0) return -1;
    return (int)((r >> 4) & 0xF);
}

// ==================== the one rule ====================
//
// Correct ONLY the state that cannot be legitimate: a device is running while
// the AFG is not in D0. Returns true when a correction was made.
static bool correct_if_wrong(void) {
    int a = power_actual(AFG_NID);
    if (a < 0 || a == 0) return false;          // unreadable, or already D0

    long w = codec_verb(AFG_NID, VERB_SET_POWER_STATE, PS_D0);
    if (w < 0) return false;

    int after = power_actual(AFG_NID);
    if (gVerbose) {
        stamp();
        printf("AFG was D%d while audio ran -> forced D0 (now D%d)\n",
               a, after < 0 ? -1 : after);
        fflush(stdout);
    }
    return true;
}

// Is any audio device actually running IO right now?
static bool any_device_running(void) {
    AudioObjectPropertyAddress addr = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };

    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, NULL, &sz)
        != noErr || sz == 0) return false;

    int n = (int)(sz / sizeof(AudioDeviceID));
    AudioDeviceID *devs = malloc(sz);
    if (!devs) return false;
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, NULL, &sz, devs)
        != noErr) { free(devs); return false; }

    bool running = false;
    for (int i = 0; i < n && !running; i++) {
        AudioObjectPropertyAddress r = {
            kAudioDevicePropertyDeviceIsRunningSomewhere,
            kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        UInt32 v = 0, vs = sizeof(v);
        if (AudioObjectGetPropertyData(devs[i], &r, 0, NULL, &vs, &v) == noErr && v)
            running = true;
    }
    free(devs);
    return running;
}

// Pre-arm on a JACK event, before anything starts playing.
//
// Waiting for kAudioDevicePropertyDeviceIsRunningSomewhere is correct but
// LATE: that property can only fire once IO has already begun, so the first
// ~0.5 s is audibly driven through the D3 path before the correction lands
// (measured 1 Aug). A jack insert happens seconds earlier, so powering the
// AFG there means playback starts on an already-live path.
//
// This is the one write NOT gated on observing a wrong state, so it is worth
// being explicit about why that is still safe. The rule this project learned
// from the retracted rate-pinner is "never write into AppleHDA's
// reprogramming window", and it exists because a rate write can leave the
// stream and the codec disagreeing. SET_POWER_STATE D0 cannot: it is
// idempotent and MONOTONIC — it only ever powers up, and there is no second
// piece of state for it to fall out of sync with. It is also bounded to one
// write per jack event, so we never sit in a loop fighting AppleHDA's idle
// policy: if no playback follows, AppleHDA idles the AFG back to D3 and we
// leave it there.
static void prearm_for_jack(void) {
    int before = power_actual(AFG_NID);
    if (before <= 0) return;                    // already D0, or unreadable
    if (codec_verb(AFG_NID, VERB_SET_POWER_STATE, PS_D0) < 0) return;
    if (gVerbose) {
        stamp();
        printf("jack event with AFG in D%d -> pre-armed D0 before playback\n",
               before);
        fflush(stdout);
    }
}

// A notification is only a hint to LOOK. Apart from the jack pre-arm above,
// nothing is written unless the state is wrong — see the header comment.
static OSStatus on_change(AudioObjectID obj, UInt32 n,
                          const AudioObjectPropertyAddress *addrs, void *ctx) {
    (void)obj; (void)ctx;

    bool jack = false;
    for (UInt32 i = 0; i < n; i++) {
        if (addrs[i].mSelector == kAudioDevicePropertyJackIsConnected ||
            addrs[i].mSelector == kAudioDevicePropertyDataSource)
            jack = true;
    }

    if (jack) prearm_for_jack();      // get ahead of the stream
    if (any_device_running()) correct_if_wrong();
    return noErr;
}

static void backstop(CFRunLoopTimerRef t, void *info) {
    (void)t; (void)info;
    if (any_device_running()) correct_if_wrong();
}

static void listen_all(void) {
    AudioObjectPropertyAddress devs = {
        kAudioHardwarePropertyDevices,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &devs, on_change, NULL);

    AudioObjectPropertyAddress defOut = {
        kAudioHardwarePropertyDefaultOutputDevice,
        kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    AudioObjectAddPropertyListener(kAudioObjectSystemObject, &defOut, on_change, NULL);

    // The signal that matters: a device started or stopped doing IO.
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &devs, 0, NULL, &sz)
        == noErr && sz) {
        int n = (int)(sz / sizeof(AudioDeviceID));
        AudioDeviceID *list = malloc(sz);
        if (list && AudioObjectGetPropertyData(kAudioObjectSystemObject, &devs,
                                               0, NULL, &sz, list) == noErr) {
            for (int i = 0; i < n; i++) {
                AudioObjectPropertyAddress run = {
                    kAudioDevicePropertyDeviceIsRunningSomewhere,
                    kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
                AudioObjectAddPropertyListener(list[i], &run, on_change, NULL);

                // Jack signals — these arrive BEFORE playback starts, which is
                // what removes the audible head of the fault. DataSource is the
                // one this codec actually moves (ispk <-> hdpn); JackIsConnected
                // is registered too because it is the semantically right one
                // where a driver publishes it.
                AudioObjectPropertyAddress jack = {
                    kAudioDevicePropertyJackIsConnected,
                    kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
                AudioObjectAddPropertyListener(list[i], &jack, on_change, NULL);

                AudioObjectPropertyAddress src = {
                    kAudioDevicePropertyDataSource,
                    kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain };
                AudioObjectAddPropertyListener(list[i], &src, on_change, NULL);
            }
        }
        free(list);
    }
}

int main(int argc, char **argv) {
    bool once = (argc > 1 && strcmp(argv[1], "--once") == 0);
    bool status = (argc > 1 && strcmp(argv[1], "--status") == 0);

    if (status) {
        gVerbose = 0;
        if (!codec_open()) {
            printf("cannot open %s — is boot-arg alcverbs=1 set?\n",
                   kALCUserClientProvider);
            return 1;
        }
        printf("audio running : %s\n", any_device_running() ? "yes" : "no");
        long afg = codec_verb(AFG_NID, VERB_GET_POWER_STATE, 0);
        printf("AFG   0x01    : 0x%08lx  (actual D%d)\n", afg, (int)((afg >> 4) & 0xF));
        long pin = codec_verb(HP_PIN_NID, VERB_GET_POWER_STATE, 0);
        printf("HP pin 0x21   : 0x%08lx  (actual D%d)\n", pin, (int)((pin >> 4) & 0xF));
        long dac = codec_verb(HP_DAC_NID, VERB_GET_POWER_STATE, 0);
        printf("HP DAC 0x03   : 0x%08lx  (actual D%d)\n", dac, (int)((dac >> 4) & 0xF));
        printf("\nD0 everywhere while audio runs = healthy.\n"
               "AFG in D3 while audio runs = the fault this tool corrects.\n");
        codec_close();
        return 0;
    }

    if (!codec_open()) {
        fprintf(stderr, "latsof-afgwake: cannot open %s — is boot-arg "
                        "alcverbs=1 set?\n", kALCUserClientProvider);
        return 1;
    }

    if (once) {
        bool did = any_device_running() ? correct_if_wrong() : false;
        if (!did) printf("nothing to do\n");
        codec_close();
        return 0;
    }

    stamp();
    printf("watching: force AFG D0 whenever audio is running "
           "(event-driven, %.0fs backstop)\n", kBackstopSecs);
    fflush(stdout);

    if (any_device_running()) correct_if_wrong();   // catch a start we missed
    listen_all();

    CFRunLoopTimerRef t = CFRunLoopTimerCreate(
        kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + kBackstopSecs,
        kBackstopSecs, 0, 0, backstop, NULL);
    if (t) { CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopDefaultMode);
             CFRelease(t); }

    CFRunLoopRun();
    codec_close();
    return 0;
}
