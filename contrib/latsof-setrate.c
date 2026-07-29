#include <CoreAudio/CoreAudio.h>
#include <stdio.h>
#include <string.h>
int main(int argc, char **argv) {
    AudioObjectPropertyAddress la = { kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
    UInt32 lsz = 0; AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &la, 0, NULL, &lsz);
    int n = lsz / sizeof(AudioDeviceID); AudioDeviceID devs[64];
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &la, 0, NULL, &lsz, devs);
    for (int i = 0; i < n; i++) {
        CFStringRef uid = NULL; UInt32 sz = sizeof(uid);
        AudioObjectPropertyAddress u = { kAudioDevicePropertyDeviceUID, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectGetPropertyData(devs[i], &u, 0, NULL, &sz, &uid);
        char ub[256] = "?";
        if (uid) { CFStringGetCString(uid, ub, 256, kCFStringEncodingUTF8); CFRelease(uid); }
        if (!strstr(ub, "AppleHDAEngineOutput")) continue;
        Float64 rate = 0; sz = sizeof(rate);
        AudioObjectPropertyAddress r = { kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain };
        AudioObjectGetPropertyData(devs[i], &r, 0, NULL, &sz, &rate);
        printf("%s  current=%.0f", ub, rate);
        if (argc > 1) {
            Float64 want = atof(argv[1]);
            OSStatus s = AudioObjectSetPropertyData(devs[i], &r, 0, NULL, sizeof(want), &want);
            sz = sizeof(rate);
            AudioObjectGetPropertyData(devs[i], &r, 0, NULL, &sz, &rate);
            printf("  set->%.0f (status %d)", rate, (int)s);
        }
        printf("\n");
    }
    return 0;
}
