//
// LatSOFAudioDevice.cpp — part of LatSOFAudio, the internal-microphone driver for the
// Dell Latitude 3410 hackintosh: https://github.com/shubhambanekar/LatSOFAudio
//
// Forked from CmlSOFAudio by DexterSLamb (HP Chromebook C1030):
//   https://github.com/DexterSLamb/CmlSOFAudio
// Copyright (c) 2026 DexterSLamb
// Copyright (c) 2026 Shubham Banekar
// SPDX-License-Identifier: BSD-3-Clause — see LICENSE and NOTICE.
//

#include "LatSOFAudioDevice.hpp"
#include <IOKit/IOLib.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <libkern/libkern.h>

// DMA buffer with bus address (via IODMACommand)
struct DmaBuf {
    IOBufferMemoryDescriptor *md;
    IODMACommand *cmd;
    IOMemoryMap *map;
    UInt64 physAddr;
    void *virtAddr;
    UInt32 size;
};

static DmaBuf *allocDma(UInt32 sz, UInt32 align) {
    mach_vm_address_t mask = ~((UInt64)align - 1);
    auto *md = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, kIOMemoryPhysicallyContiguous | kIOMapInhibitCache, sz, mask);
    if (!md) return nullptr;
    if (md->prepare() != kIOReturnSuccess) { md->release(); return nullptr; }
    auto *cmd = IODMACommand::withSpecification(
        kIODMACommandOutputHost64, 64, sz, IODMACommand::kMapped, 0, 1);
    if (!cmd) { md->complete(); md->release(); return nullptr; }
    if (cmd->setMemoryDescriptor(md) != kIOReturnSuccess) {
        cmd->release(); md->complete(); md->release(); return nullptr;
    }
    IODMACommand::Segment64 seg; UInt32 numSeg = 1; UInt64 offset = 0;
    if (cmd->gen64IOVMSegments(&offset, &seg, &numSeg) != kIOReturnSuccess) {
        cmd->clearMemoryDescriptor(); cmd->release(); md->complete(); md->release(); return nullptr;
    }
    auto *map = md->map(kIOMapInhibitCache);
    if (!map) {
        cmd->clearMemoryDescriptor(); cmd->release(); md->complete(); md->release(); return nullptr;
    }
    auto *b = (DmaBuf *)IOMalloc(sizeof(DmaBuf));
    b->md = md; b->cmd = cmd; b->map = map;
    b->physAddr = seg.fIOVMAddr;
    b->virtAddr = (void *)map->getVirtualAddress();
    b->size = sz;
    bzero(b->virtAddr, sz);
    return b;
}

static void freeDma(DmaBuf *b) {
    if (!b) return;
    if (b->map) b->map->release();
    if (b->cmd) { b->cmd->clearMemoryDescriptor(); b->cmd->release(); }
    if (b->md) { b->md->complete(); b->md->release(); }
    IOFree(b, sizeof(DmaBuf));
}

// Kext module entry
extern "C" kern_return_t LatSOFAudio_start(kmod_info_t *ki, void *data);
extern "C" kern_return_t LatSOFAudio_stop(kmod_info_t *ki, void *data);
__attribute__((visibility("default")))
KMOD_EXPLICIT_DECL(com.hackintosh.LatSOFAudio, "1.1.5", LatSOFAudio_start, LatSOFAudio_stop)
kern_return_t LatSOFAudio_start(kmod_info_t *ki, void *data) { return KERN_SUCCESS; }
kern_return_t LatSOFAudio_stop(kmod_info_t *ki, void *data) { return KERN_SUCCESS; }

// Embedded firmware
extern "C" const unsigned char sof_fw_data[];
extern "C" const unsigned long long sof_fw_size;

// ========== HDA / DSP Register Definitions ==========

// BAR0 (HDA controller)
#define HDA_GCAP            0x00
#define HDA_GCTL            0x08
#define HDA_INTCTL          0x20
#define SD_BASE             0x80
#define SD_SIZE             0x20
#define SD_CTL_SRST         0x01
#define SD_CTL_RUN          0x02
#define SD_CTL_IOCE         0x04
#define SD_REG_STS          0x03
#define SD_REG_CBL          0x08
#define SD_REG_LVI          0x0C
#define SD_REG_FMT          0x12
#define SD_REG_BDLPL        0x18
#define SD_REG_BDLPU        0x1C
#define HDA_CL_STREAM_FMT   0x40

// PP/SPIB/ML capability IDs
#define HDA_CAP_ML_ID       0x02    // Multi-Link capability
#define HDA_CAP_PP_ID       0x03
#define HDA_CAP_SPIB_ID     0x04
#define PP_PPCTL            0x04
#define PP_PPCTL_GPROCEN    (1U << 30)

// Multi-Link registers (from HDA spec, Multi-Link capability)
#define ML_MLCD             0x04    // Multi-Link Count (minus 1)
#define ML_LINK_BASE        0x40    // First link entry offset from mlcap
#define ML_LINK_INTERVAL    0x40    // Size of each link entry
#define ML_LCAP             0x00    // Link Capability
#define ML_LCTL             0x04    // Link Control
#define ML_LOSIDV           0x08    // Link Output Stream ID Validation

// BAR4 (DSP)
#define DSP_ADSPCS          0x04
#define DSP_ADSPIC          0x08
#define ADSPCS_CRST(cm)     ((cm) << 0)
#define ADSPCS_CSTALL(cm)   ((cm) << 8)
#define ADSPCS_SPA(cm)      ((cm) << 16)
#define ADSPCS_CPA(cm)      ((cm) << 24)

// CNL IPC registers (BAR4)
#define IPC_HIPCTDR         0xC0
#define IPC_HIPCTDA         0xC4
#define IPC_HIPCIDR         0xD0
#define IPC_HIPCIDA         0xD4
#define IPC_HIPCCTL         0xE8
#define IPC_BUSY            (1U << 31)
#define IPC_DONE            (1U << 31)

// Vendor Specific Registers
#define HDA_VS_SDXDPIB_XBASE    0x1084  // DPIB register base (playback position)
#define HDA_VS_SDXDPIB_XINTERVAL 0x20   // DPIB register interval per stream
#define HDA_VS_EM2          0x1030  // Extended Mode 2

// PCI Config registers
#define PCI_TCSEL           0x44    // Traffic Class Select

// DesignWare I2C controller registers (BAR0 of PCI 8086:02c5)
#define DW_IC_CON           0x00
#define DW_IC_TAR           0x04
#define DW_IC_DATA_CMD      0x10
#define DW_IC_FS_SCL_HCNT   0x1C
#define DW_IC_FS_SCL_LCNT   0x20
#define DW_IC_INTR_MASK     0x30
#define DW_IC_RAW_INTR_STAT 0x34
#define DW_IC_CLR_INTR      0x40
#define DW_IC_CLR_TX_ABRT   0x54
#define DW_IC_ENABLE        0x6C
#define DW_IC_STATUS        0x70
#define DW_IC_TXFLR         0x74
#define DW_IC_RXFLR         0x78
#define DW_IC_ENABLE_STATUS 0x9C
#define DW_IC_STATUS_TFNF   (1U << 1)  // TX FIFO not full
#define DW_IC_STATUS_RFNE   (1U << 3)  // RX FIFO not empty
#define DW_IC_STATUS_TFE    (1U << 2)  // TX FIFO empty
#define DW_IC_DATA_CMD_READ    0x0100   // read command
#define DW_IC_DATA_CMD_STOP    0x0200   // stop after this byte
#define DW_IC_DATA_CMD_RESTART 0x0400   // restart before this byte

// RT5682 I2C address and key registers
#define RT5682_I2C_ADDR     0x1A
#define RT5682_DEVICE_ID    0x00FF
#define RT5682_RESET        0x0000

// SRAM
#define SRAM_WIN(n)         (0x80000 + (n) * 0x20000)
#define MBOX_UPLINK         0x81000
#define ROM_STATUS          0x80000
#define FSR_FW_ENTERED      0x5
#define FSR_INIT_DONE       0x1

// ROM IPC
#define ROM_IPC_CONTROL     0x01000000
#define ROM_IPC_PURGE_FW    0x00004000

// IPC commands
#define SOF_IPC_FW_READY    0x70000000
#define SOF_IPC_EXT_WINDOW  1
#define SOF_IPC_REGION_DOWNBOX  0
#define SOF_IPC_REGION_UPBOX    1

// SOF IPC message structs (matching SOF ABI 3.x, include/sound/sof/stream.h)
struct sof_ipc_host_buffer {
    UInt32 hdr_size;
    UInt32 phy_addr;
    UInt32 pages;
    UInt32 size;
    UInt32 reserved[3];
} __attribute__((packed));

struct sof_ipc_pcm_params {
    UInt32 hdr_size;      // = sizeof(sof_ipc_pcm_params)
    UInt32 hdr_cmd;       // = 0x60010000 (STREAM_PCM_PARAMS)
    UInt32 comp_id;
    UInt32 flags;
    UInt32 reserved[2];
    // stream_params:
    UInt32 params_size;   // = 84
    struct sof_ipc_host_buffer buffer;
    UInt32 direction;
    UInt32 frame_fmt;
    UInt32 buffer_fmt;
    UInt32 rate;
    UInt16 stream_tag;
    UInt16 channels;
    UInt16 sample_valid_bytes;
    UInt16 sample_container_bytes;
    UInt32 host_period_bytes;
    UInt16 no_stream_position;
    UInt8  cont_update_posn;
    UInt8  reserved0;
    SInt16 ext_data_length;
    UInt8  reserved1[2];
    UInt16 chmap[8];
} __attribute__((packed));

// Pipeline component IDs
#define COMP_HOST    1
#define COMP_DAI     2
#define COMP_BUFFER  12
#define FRAME_S16    0
#define FRAME_S24    1
#define FRAME_S32    2
#define DAI_SSP      1
#define DAI_DMIC     2
#define DIR_PLAYBACK 0
#define DIR_CAPTURE  1
#define TIME_TIMER   1

// Pipeline 7: SSP1 playback (MAX98357A speaker)
#define PIPE7_SCHED_ID   70
#define PIPE7_HOST_ID    75
#define PIPE7_BUF0_ID    71
#define PIPE7_DAI_ID     74
#define PIPE7_ID         7

// Pipeline 3: DMIC0 capture (internal microphone)
#define PIPE3_SCHED_ID   30
#define PIPE3_HOST_ID    35
#define PIPE3_BUF0_ID    31
#define PIPE3_DAI_ID     34
#define PIPE3_ID         3

// Pipeline 1: SSP0 playback (RT5682 headphone output)
// Using Linux kprobe comp_ids for Pipeline 1
#define PIPE1_SCHED_ID   5     // Linux: scheduler comp_id=5
#define PIPE1_HOST_ID    0     // Linux: host comp_id=0
#define PIPE1_BUF0_ID    2     // Linux: first buffer comp_id=2
#define PIPE1_DAI_ID     4     // Linux: DAI comp_id=4
#define PIPE1_BUF1_ID    3     // unused (no PGA), kept for compat
#define PIPE1_PGA_ID     1     // unused (no PGA), kept for compat
#define PIPE1_ID         1
#define COMP_VOLUME      5     // SOF_COMP_VOLUME

// Linux topology comp_ids (from kprobe IPC dump - auto-incremented from 0)
#define LINUX_PIPE7_HOST_ID  34   // Pipeline 7 (speaker) host
#define LINUX_PIPE1_HOST_ID  0    // Pipeline 1 (HP playback) host
#define LINUX_PIPE2_HOST_ID  6    // Pipeline 2 (HP capture) host
#define LINUX_PIPE3_HOST_ID  12   // Pipeline 3 (DMIC capture) host

// Pipeline 2: SSP0 capture (RT5682 headset mic via SSP0)
// Full chain: SSP0.IN(dai) → BUF2.0 → PGA2.0 → BUF2.1 → PCM0C(host)
#define PIPE2_SCHED_ID   20
#define PIPE2_HOST_ID    25
#define PIPE2_BUF0_ID    21
#define PIPE2_BUF1_ID    22
#define PIPE2_PGA_ID     23
#define PIPE2_DAI_ID     24
#define PIPE2_ID         2

struct HdaBdlEntry {
    UInt32 addrLow, addrHigh, size, ioc;
} __attribute__((packed));

OSDefineMetaClassAndStructors(LatSOFAudioDevice, IOService)

// ========== Register helpers ==========
static inline UInt32 rd32(volatile UInt8 *b, UInt32 o) { return *(volatile UInt32*)(b+o); }
static inline void   wr32(volatile UInt8 *b, UInt32 o, UInt32 v) { *(volatile UInt32*)(b+o) = v; }
static inline UInt16 rd16(volatile UInt8 *b, UInt32 o) { return *(volatile UInt16*)(b+o); }
static inline void   wr16(volatile UInt8 *b, UInt32 o, UInt16 v) { *(volatile UInt16*)(b+o) = v; }
static inline UInt8  rd8(volatile UInt8 *b, UInt32 o)  { return *(volatile UInt8*)(b+o); }
static inline void   wr8(volatile UInt8 *b, UInt32 o, UInt8 v)  { *(volatile UInt8*)(b+o) = v; }

// LATITUDE FORK patch-22: wake re-init retry state. File-scope statics so
// no header change is needed; every access happens on the workloop
// (jackPoll and the gated PM path), so no locking is required.
static bool gWakeReinitPending = false;
static int  gWakeTries = 0;
static int  gWakeTickDivider = 0;

static bool poll32(volatile UInt8 *b, UInt32 o, UInt32 mask, UInt32 val, UInt32 usec) {
    for (UInt32 t = 0; t < usec; t += 500) {
        if ((rd32(b, o) & mask) == val) return true;
        IODelay(500);
    }
    return false;
}

// Stream reset helper
static void streamReset(volatile UInt8 *hda, UInt32 sd) {
    wr8(hda, sd, 0); wr8(hda, sd + 2, 0); IODelay(100);
    wr8(hda, sd, SD_CTL_SRST);
    for (int t = 0; t < 300; t++) { if (rd8(hda, sd) & SD_CTL_SRST) break; IODelay(10); }
    wr8(hda, sd, 0);
    for (int t = 0; t < 300; t++) { if (!(rd8(hda, sd) & SD_CTL_SRST)) break; IODelay(10); }
    wr8(hda, sd + SD_REG_STS, 0x1C);
}

// Power states for sleep/wake
static IOPMPowerState sPowerStates[2] = {
    { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
    { 1, kIOPMDeviceUsable | kIOPMPowerOn, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 }
};

// ========== IOService ==========

bool LatSOFAudioDevice::init(OSDictionary *d) {
    if (!IOService::init(d)) return false;
    pciDevice = nullptr; hdaBarMap = nullptr; dspBarMap = nullptr;
    hdaBase = nullptr; dspBase = nullptr;
    ppCap = 0; spibCap = 0;
    sIdx = 0; sTag = 0; sd = 0;
    capIdx = 0; capTag = 0; capSd = 0;
    outboxOff = 0;
    hwReady = false; isPlaying = false; isCapturing = false;
    lastJackState = false; jackTimer = nullptr;
    commandGate = nullptr; rootDomain = nullptr; powerNotifier = nullptr;
    sharedDmaBuf = nullptr; sharedBdlBuf = nullptr;
    capDmaBuf = nullptr; capBdlBuf = nullptr;
    flagsBuf = nullptr;
    i2cPciDevice = nullptr; i2cBarMap = nullptr; i2cBase = nullptr;
    return true;
}

void LatSOFAudioDevice::free() {
    IOService::free();
}

IOService *LatSOFAudioDevice::probe(IOService *provider, SInt32 *score) {
    // LATITUDE FORK: the provider is IOResources, not the PCI device, because
    // AppleHDA owns 8086:02c8 and must keep owning it. The original body cast
    // the provider to IOPCIDevice and bailed out, so start() was never called.
    // Device discovery and validation now happen in start() via a registry walk.
    if (!IOService::probe(provider, score)) return nullptr;
    IOLog("LatSOF: probe() ok, provider=%s\n",
          provider ? provider->getName() : "(null)");
    return this;
}

bool LatSOFAudioDevice::start(IOService *provider) {
    IOLog("LatSOF: start() entered, provider=%s\n",
          provider ? provider->getName() : "(null)");
    if (!IOService::start(provider)) { IOLog("LatSOF: super::start failed\n"); return false; }
    // LATITUDE FORK: we are not this device's driver. AppleHDA owns it and
    // must keep owning it, so we locate the controller in the registry and
    // never call open().
    {
        OSDictionary *m = IOService::serviceMatching("IOPCIDevice");
        OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr;
        if (m) m->release();
        if (it) {
            OSObject *o;
            while ((o = it->getNextObject()) != nullptr) {
                IOPCIDevice *c = OSDynamicCast(IOPCIDevice, o);
                if (!c) continue;
                if (c->configRead16(kIOPCIConfigVendorID) == 0x8086 &&
                    c->configRead16(kIOPCIConfigDeviceID) == 0x02c8) {
                    pciDevice = c; pciDevice->retain(); break;
                }
            }
            it->release();
        }
    }
    if (!pciDevice) { setProperty("Status", "FAILED: cAVS not found"), IOLog("LatSOF: %s\n", "FAILED: cAVS not found"); return false; }
    IOLog("LatSOF: found cAVS: %s\n", pciDevice->getName());
    // NOTE: no pciDevice->open() — AppleHDA holds it.

    pciDevice->setIOEnable(true);
    pciDevice->setBusMasterEnable(true);
    pciDevice->setMemoryEnable(true);
    // LATITUDE FORK: D3->D0 power cycle removed (AppleHDA owns power state)

    // TCSEL: clear TC bits [2:0] to TC0 (like Linux snd_intel_dsp_driver_probe)
    {
        UInt8 tcsel = pciDevice->configRead8(PCI_TCSEL);
        pciDevice->configWrite8(PCI_TCSEL, tcsel & ~0x07);
    }
    // Disable PCI interrupt (CMD bit 10)
    {
        UInt16 cmd = pciDevice->configRead16(0x04);
        pciDevice->configWrite16(0x04, cmd | 0x0400);
    }

    hdaBarMap = pciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    dspBarMap = pciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress4);
    if (!hdaBarMap || !dspBarMap) {
        setProperty("Status", "FAILED: BAR map"), IOLog("LatSOF: %s\n", "FAILED: BAR map");
        goto fail;
    }
    hdaBase = (volatile UInt8 *)hdaBarMap->getVirtualAddress();
    dspBase = (volatile UInt8 *)dspBarMap->getVirtualAddress();

    if (!initDSP()) {
        setProperty("Status", "FAILED: DSP init"), IOLog("LatSOF: %s\n", "FAILED: DSP init");
        goto fail;
    }

    // Create the commandGate BEFORE registering for PM / installing event
    // sources. All hw-touching paths (setPowerState, UserClient methods,
    // WillSleep message) serialize through this gate.
    if (getWorkLoop()) {
        commandGate = IOCommandGate::commandGate(this);
        if (commandGate) {
            getWorkLoop()->addEventSource(commandGate);
        }
    }

    // Power management
    PMinit();
    registerPowerDriver(this, sPowerStates, 2);
    provider->joinPMtree(this);

    // Subscribe to system-wide power events via gIOGeneralInterest. This is
    // the ONLY way to receive kIOMessageSystemWillSleep and
    // kIOPMMessageClamshellStateChange; registerInterestedDriver only
    // delivers PM-state messages like
    // kIOMessageDeviceWillPowerOff, *not* the system-wide ones we need.
    rootDomain = getPMRootDomain();
    if (rootDomain) {
        powerNotifier = rootDomain->registerInterest(
            gIOGeneralInterest, &sPowerInterestHandler, this, nullptr);
    }

    // Jack detection polling timer (every 500ms). Runs on the same workloop
    // as commandGate, so it is naturally serialized with PM and UserClient
    // paths — no extra lock needed.
    jackTimer = IOTimerEventSource::timerEventSource(this,
        OSMemberFunctionCast(IOTimerEventSource::Action, this, &LatSOFAudioDevice::jackPoll));
    if (jackTimer && getWorkLoop()) {
        getWorkLoop()->addEventSource(jackTimer);
        jackTimer->setTimeoutMS(500);
    }

    // Coordination flag page (see kLatSOF_MemFlags). One 4 KiB page, zero
    // initialised. Page-aligned for cheap plugin-side mmap semantics.
    flagsBuf = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task, kIODirectionInOut, PAGE_SIZE, PAGE_SIZE);
    if (flagsBuf) {
        flagsBuf->prepare();
        bzero(flagsBuf->getBytesNoCopy(), PAGE_SIZE);
    }

    IOLog("LatSOF: start() completed, hwReady=%d\n", hwReady ? 1 : 0);
    registerService();
    return true;

fail:
    if (hdaBarMap) { hdaBarMap->release(); hdaBarMap = nullptr; }
    if (dspBarMap) { dspBarMap->release(); dspBarMap = nullptr; }
    if (pciDevice) { pciDevice->release(); pciDevice = nullptr; }
    return false;
}

// ==================== I2C + RT5682 ====================

bool LatSOFAudioDevice::initI2C() {
    // LATITUDE FORK: no I2S codecs on this board.
    return true;
#if 0
    // Find Intel LPSS I2C4 controller (PCI 8086:02c5)
    // This is the bus where RT5682 lives (confirmed by Linux: PCI 00:19.0)
    if (!i2cPciDevice) {
        OSDictionary *match = IOService::serviceMatching("IOPCIDevice");
        OSIterator *iter = IOService::getMatchingServices(match);
        if (iter) {
            IOService *svc;
            while ((svc = (IOService *)iter->getNextObject())) {
                IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, svc);
                if (pci && pci->configRead16(kIOPCIConfigVendorID) == 0x8086 &&
                    pci->configRead16(kIOPCIConfigDeviceID) == 0x02c5) {
                    i2cPciDevice = pci;
                    i2cPciDevice->retain();
                    break;
                }
            }
            iter->release();
        }
        if (match) match->release();
    }
    if (!i2cPciDevice) { setProperty("I2C", "PCI 8086:02c5 not found"); return false; }

    // Power up I2C controller: D0 state, bus master, memory enable
    i2cPciDevice->setBusMasterEnable(true);
    i2cPciDevice->setMemoryEnable(true);
    i2cPciDevice->setIOEnable(true);
    // Force D0 power state (PMCSR offset varies, try 0x80 for LPSS)
    { UInt16 pmcsr = i2cPciDevice->configRead16(0x80);
      if (pmcsr & 0x3) { // if not D0
          i2cPciDevice->configWrite16(0x80, pmcsr & ~0x3); // set D0
          IOSleep(10);
      }
    }
    if (!i2cBarMap) i2cBarMap = i2cPciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
    if (!i2cBarMap) { setProperty("I2C", "BAR0 map failed"); return false; }
    i2cBase = (volatile UInt8 *)i2cBarMap->getVirtualAddress();

    // LPSS private registers: reset and unreset (offset 0x200 + 0x04)
    wr32(i2cBase, 0x204, 0);         // Assert reset
    IODelay(1000);
    wr32(i2cBase, 0x204, 3);         // Deassertion: FUNC + IDMA
    IODelay(1000);

    // Disable controller
    wr32(i2cBase, DW_IC_ENABLE, 0);
    for (int t = 0; t < 100; t++) { if (!(rd32(i2cBase, DW_IC_ENABLE_STATUS) & 1)) break; IODelay(100); }

    // Configure: fast mode (400kHz), master, 7-bit addr, restart enable
    wr32(i2cBase, DW_IC_CON, 0x65);
    // Fast mode SCL timing (for ~120MHz LPSS clock)
    wr32(i2cBase, DW_IC_FS_SCL_HCNT, 0x003C);
    wr32(i2cBase, DW_IC_FS_SCL_LCNT, 0x0082);
    // Target address
    wr32(i2cBase, DW_IC_TAR, RT5682_I2C_ADDR);
    // Disable all interrupts
    wr32(i2cBase, DW_IC_INTR_MASK, 0);

    // Enable controller
    wr32(i2cBase, DW_IC_ENABLE, 1);
    for (int t = 0; t < 100; t++) { if (rd32(i2cBase, DW_IC_ENABLE_STATUS) & 1) break; IODelay(100); }

    // Diagnostic: dump I2C controller state
    { char d[128]; snprintf(d, sizeof(d), "CON=0x%x TAR=0x%x STAT=0x%x EN=0x%x",
        rd32(i2cBase, DW_IC_CON), rd32(i2cBase, DW_IC_TAR),
        rd32(i2cBase, DW_IC_STATUS), rd32(i2cBase, DW_IC_ENABLE_STATUS));
      setProperty("I2C-Regs", d); }
    setProperty("I2C", "OK");
    return true;
#endif
}

bool LatSOFAudioDevice::i2cWrite16(UInt16 reg, UInt16 val) {
    if (!i2cBase) return false;
    // Clear any pending errors
    rd32(i2cBase, DW_IC_CLR_TX_ABRT);
    rd32(i2cBase, DW_IC_CLR_INTR);

    // Write 4 bytes: reg_hi, reg_lo, val_hi, val_lo
    UInt8 bytes[4] = { (UInt8)(reg >> 8), (UInt8)(reg & 0xFF),
                       (UInt8)(val >> 8), (UInt8)(val & 0xFF) };
    for (int i = 0; i < 4; i++) {
        UInt32 cmd = bytes[i];
        if (i == 3) cmd |= DW_IC_DATA_CMD_STOP;
        // Wait for TX FIFO not full
        for (int t = 0; t < 1000; t++) { if (rd32(i2cBase, DW_IC_STATUS) & DW_IC_STATUS_TFNF) break; IODelay(10); }
        wr32(i2cBase, DW_IC_DATA_CMD, cmd);
    }
    // Wait for TX complete
    for (int t = 0; t < 1000; t++) { if (rd32(i2cBase, DW_IC_STATUS) & DW_IC_STATUS_TFE) break; IODelay(10); }
    IODelay(100);
    bool ok = (rd32(i2cBase, DW_IC_RAW_INTR_STAT) & (1U << 6)) == 0;
    if (!ok) rd32(i2cBase, DW_IC_CLR_TX_ABRT); // clear abort for next transaction
    return ok;
}

UInt16 LatSOFAudioDevice::i2cRead16(UInt16 reg) {
    if (!i2cBase) return 0;
    rd32(i2cBase, DW_IC_CLR_TX_ABRT);
    rd32(i2cBase, DW_IC_CLR_INTR);

    // Drain any stale RX FIFO data (prevents stale reads after write bursts)
    for (int d = 0; d < 16 && rd32(i2cBase, DW_IC_RXFLR) > 0; d++)
        rd32(i2cBase, DW_IC_DATA_CMD);

    // Wait for bus idle (TX FIFO empty + bus not busy)
    for (int t = 0; t < 1000; t++) { if (rd32(i2cBase, DW_IC_STATUS) & DW_IC_STATUS_TFE) break; IODelay(10); }

    // Write register address (2 bytes), then read 2 bytes
    UInt8 addr[2] = { (UInt8)(reg >> 8), (UInt8)(reg & 0xFF) };
    for (int i = 0; i < 2; i++) {
        for (int t = 0; t < 1000; t++) { if (rd32(i2cBase, DW_IC_STATUS) & DW_IC_STATUS_TFNF) break; IODelay(10); }
        wr32(i2cBase, DW_IC_DATA_CMD, addr[i]);
    }
    // Issue 2 read commands: first with RESTART, last with STOP
    for (int t = 0; t < 1000; t++) { if (rd32(i2cBase, DW_IC_STATUS) & DW_IC_STATUS_TFNF) break; IODelay(10); }
    wr32(i2cBase, DW_IC_DATA_CMD, DW_IC_DATA_CMD_READ | DW_IC_DATA_CMD_RESTART);
    for (int t = 0; t < 1000; t++) { if (rd32(i2cBase, DW_IC_STATUS) & DW_IC_STATUS_TFNF) break; IODelay(10); }
    wr32(i2cBase, DW_IC_DATA_CMD, DW_IC_DATA_CMD_READ | DW_IC_DATA_CMD_STOP);

    // Read 2 bytes from RX FIFO
    UInt8 hi = 0, lo = 0;
    for (int t = 0; t < 1000; t++) { if (rd32(i2cBase, DW_IC_RXFLR) > 0) break; IODelay(10); }
    hi = (UInt8)rd32(i2cBase, DW_IC_DATA_CMD);
    for (int t = 0; t < 1000; t++) { if (rd32(i2cBase, DW_IC_RXFLR) > 0) break; IODelay(10); }
    lo = (UInt8)rd32(i2cBase, DW_IC_DATA_CMD);
    return ((UInt16)hi << 8) | lo;
}

bool LatSOFAudioDevice::initRT5682() {
    // LATITUDE FORK: no I2S codecs on this board.
    return true;
#if 0
    if (!initI2C()) return false;

    // Step 0: Enable I2C mode (RT5682 requires this before any register access)
    // Linux: regmap_write(regmap, RT5682_I2C_MODE, 0x1); usleep_range(10000,15000);
    IOSleep(300); // power-on settle time
    i2cWrite16(0xFFFF, 0x0001); // RT5682_I2C_MODE = 1
    IOSleep(15);

    // Step 1: Verify Device ID (with diagnostic)
    UInt16 devId = i2cRead16(RT5682_DEVICE_ID);
    { UInt32 rawStat = rd32(i2cBase, DW_IC_RAW_INTR_STAT);
      UInt32 rxflr = rd32(i2cBase, DW_IC_RXFLR);
      char d[64]; snprintf(d, sizeof(d), "0x%04x (raw_intr=0x%x rxflr=%u)", devId, rawStat, rxflr);
      setProperty("RT5682-ID", d); }
    if (devId != 0x6530) { setProperty("RT5682", "Wrong device ID"); return false; }

    // Step 2: Calibration (from rt5682_calibrate in Linux)
    // rt5682_reset(): RESET + re-enable I2C_MODE (mandatory after reset!)
    i2cWrite16(0x0000, 0x0000); // RESET
    IOSleep(300);
    i2cWrite16(0xFFFF, 0x0001); // I2C_MODE = 1 (Linux rt5682.c:819)
    IOSleep(15);

    i2cWrite16(0x0008, 0x000F); // I2C_CTRL
    i2cWrite16(0x0063, 0xA2AF); // PWR_ANLG_1
    IOSleep(15);
    i2cWrite16(0x0063, 0xF2AF); // PWR_ANLG_1 + fast VREF
    i2cWrite16(0x0094, 0x0300); // MICBIAS_2
    i2cWrite16(0x0080, 0x8000); // GLB_CLK = RCCLK
    i2cWrite16(0x0061, 0x0100); // PWR_DIG_1: LDO
    i2cWrite16(0x01C1, 0x3800); // HP_IMP_SENS_CTRL_19
    i2cWrite16(0x013A, 0x3000); // CHOP_DAC
    i2cWrite16(0x013C, 0x7005); // CALIB_ADC_CTRL
    i2cWrite16(0x0026, 0x686C); // STO1_ADC_MIXER
    i2cWrite16(0x0044, 0x0D0D); // CAL_REC
    i2cWrite16(0x01DF, 0x0321); // HP_CALIB_CTRL_2
    i2cWrite16(0x01DB, 0x0004); // HP_LOGIC_CTRL_2
    i2cWrite16(0x01DE, 0x7C00); // HP_CALIB_CTRL_1
    i2cWrite16(0x01E0, 0x06A1); // HP_CALIB_CTRL_3
    i2cWrite16(0x002B, 0x0311); // A_DAC1_MUX
    i2cWrite16(0x01DE, 0x7C00); // HP_CALIB_CTRL_1
    i2cWrite16(0x01DE, 0xFC00); // HP_CALIB_CTRL_1: start calibration

    // Poll calibration complete (bit 15 of 0x01EA clears)
    for (int t = 0; t < 60; t++) {
        if (!(i2cRead16(0x01EA) & 0x8000)) break;
        IOSleep(10);
    }

    // Restore defaults after calibration
    i2cWrite16(0x0063, 0x002F); // PWR_ANLG_1
    i2cWrite16(0x0094, 0x0080); // MICBIAS_2
    i2cWrite16(0x0080, 0x0000); // GLB_CLK
    i2cWrite16(0x0061, 0x0000); // PWR_DIG_1
    i2cWrite16(0x013A, 0x2000); // CHOP_DAC
    i2cWrite16(0x013C, 0x2005); // CALIB_ADC_CTRL
    i2cWrite16(0x0026, 0xC0C4); // STO1_ADC_MIXER
    i2cWrite16(0x0044, 0x0C0C); // CAL_REC

    // Step 3: Apply patch list (from rt5682_apply_patch_list)
    i2cWrite16(0x01C1, 0x1000);
    i2cWrite16(0x0100, 0xA020);
    i2cWrite16(0x0008, 0x000F);
    i2cWrite16(0x0156, 0x8266);
    i2cWrite16(0x0210, 0x22B7);
    i2cWrite16(0x0212, 0x0365);
    i2cWrite16(0x0215, 0x0110);
    i2cWrite16(0x0125, 0x0210);
    i2cWrite16(0x01DB, 0x0007);
    i2cWrite16(0x0211, 0xAC00);
    i2cWrite16(0x0016, 0x0104);

    // Step 4: Post-probe config (from rt5682_i2c_probe)
    i2cWrite16(0x008E, 0x0000); // DEPOP_1
    i2cWrite16(0x0063, 0x002C); // PWR_ANLG_1: LDO1_12V + HP_5X
    i2cWrite16(0x0094, 0x0080); // MICBIAS_2
    i2cWrite16(0x00C0, 0x6960); // GPIO_CTRL_1 (matches Linux)
    i2cWrite16(0x0145, 0x0000); // TEST_MODE_CTRL_1
    i2cWrite16(0x0125, 0x0220); // CHARGE_PUMP_1: CP_CLK_HP_300KHz
    i2cWrite16(0x006E, 0x1000); // DMIC_CTRL_1: FIFO_CLK_DIV_2

    // Steps 5+9 (PLL + HP power) DEFERRED to startPlayback()
    // Reason: MCLK only exists when SSP0 is streaming (confirmed on Linux:
    // CLK_DET=0x8001 during playback, 0x0000 after stop).
    // PLL needs MCLK to lock, so must configure after TRIG_START.

    // Step 6: I2S1 format (24-bit I2S, matches Linux 0xA220)
    i2cWrite16(0x0070, 0xA220); // I2S1_SDP: I2S, 24-bit

    // Step 7: TDM/ADDA registers (from Linux regmap - control DAC data routing)
    i2cWrite16(0x0073, 0x1001); // ADDA_CLK_1
    i2cWrite16(0x0075, 0x0002); // I2S1_F_DIV_1
    i2cWrite16(0x0076, 0x0001); // I2S1_F_DIV_2
    i2cWrite16(0x007b, 0x0080); // TDM_ADDA_CTRL_2
    i2cWrite16(0x007c, 0x0100); // TDM_ADDA_CTRL_3
    i2cWrite16(0x007e, 0x0020); // TDM_ADDA_CTRL_5
    i2cWrite16(0x0071, 0xC000); // I2S2_SDP
    i2cWrite16(0x008f, 0x1000); // DEPOP_2
    i2cWrite16(0x008c, 0x0003); // PLL_TRACK_11
    i2cWrite16(0x0083, 0x3100); // PLL_TRACK_1
    i2cWrite16(0x0084, 0x1100); // PLL_TRACK_2
    i2cWrite16(0x0085, 0x1000); // PLL_TRACK_3
    i2cWrite16(0x0086, 0x0005); // PLL_TRACK_4

    // Step 8: DAC volume
    i2cWrite16(0x0019, 0xAFAF); // DAC1_DIG_VOL: 0dB both channels

    // Step 9: Jack detection init (from Linux rt5682_set_jack_detect)
    i2cWrite16(0x009F, 0xD000); // RC_CLK_CTRL: POW_IRQ | POW_JDH | POW_ANA
    i2cWrite16(0x0064, (i2cRead16(0x0064) | 0x0008)); // PWR_ANLG_2: enable PWR_JDH (bit 3)
    i2cWrite16(0x00B7, 0x8000); // IRQ_CTRL_2: JD1_EN=1, JD1_POL=normal
    i2cWrite16(0x00F6, 0x0100); // JD_CTRL_1: enable JD1

    setProperty("RT5682", "Init OK");
    return true;
#endif
}

// ==================== DSP Init (called from start and wake) ====================

bool LatSOFAudioDevice::initDSP() {
    volatile UInt8 *hda = hdaBase;
    volatile UInt8 *dsp = dspBase;
    if (!hda || !dsp) return false;

    // LATITUDE FORK patch-17: restore D0 before reading anything.
    // Our sleep path lets IOKit PM drop this function to D3 and expects wake
    // to rebuild. But AppleHDA owns the device, so nobody puts it back in D0
    // on our schedule — and a D3 function does not answer memory cycles at
    // all, so every read is 0xFFFF and no amount of waiting helps (patch-15
    // waited its full 3 s and still saw all-ones while AppleHDA's speakers
    // worked fine). setMemoryEnable/setBusMasterEnable are command-register
    // bits and cannot fix this on their own. So: find the PCI Power
    // Management capability and put the function back in D0 ourselves.
    // No-op when already in D0, which is every normal boot.
    if (pciDevice) {
        UInt8 pmCap = 0;
        if (pciDevice->configRead16(0x06) & 0x0010) {   // capability list present
            UInt8 off = pciDevice->configRead8(0x34) & 0xFC;
            for (int g = 0; g < 48 && off >= 0x40; g++) {
                if (pciDevice->configRead8(off) == 0x01) { pmCap = off; break; }
                off = pciDevice->configRead8(off + 1) & 0xFC;
            }
        }
        UInt16 pmcs = pmCap ? pciDevice->configRead16(pmCap + 4) : 0;
        if (pmCap && (pmcs & 0x3) != 0) {
            pciDevice->configWrite16(pmCap + 4, (UInt16)(pmcs & ~0x3));
            IOSleep(10);                                // PCI spec D3hot->D0 recovery
        }
        pciDevice->setMemoryEnable(true);
        pciDevice->setBusMasterEnable(true);
        { char p[72];
          snprintf(p, sizeof(p), "pmcap=0x%02x pmcs=0x%04x after=0x%04x",
                   pmCap, pmcs, pmCap ? pciDevice->configRead16(pmCap + 4) : 0);
          setProperty("PCI-Power", p); }
    }

    // LATITUDE FORK patch-15: never trust registers that read all-ones.
    // At boot the PCI family may not have enabled memory decode yet; at
    // wake, setPowerStateGated's setMemoryEnable is not enough because the
    // function can still be mid D3->D0 restore — that restore belongs to
    // AppleHDA (it owns the PCI device), so we are racing its wake path.
    // With GCAP=0xffff the code loader binds to stream 15 and every
    // "wait for bit set" check passes vacuously; the init then dies at
    // INIT_DONE. Wait, bounded, before deriving ANYTHING.
    {
        int tries = 0;
        UInt16 g = rd16(hda, HDA_GCAP);
        while ((g == 0xFFFF || g == 0x0000) && tries < 50) {   // patch-22: retries replace the long wait
            IOSleep(10);
            g = rd16(hda, HDA_GCAP);
            tries++;
        }
        { char w[48];
          snprintf(w, sizeof(w), "gcap=0x%04x tries=%d ms=%d", g, tries, tries * 10);
          setProperty("GCAP-Wait", w); }
        if (g == 0xFFFF || g == 0x0000) {
            setProperty("Status", "FAILED: controller not decoding (GCAP)"),
                IOLog("LatSOF: %s\n", "FAILED: controller not decoding (GCAP)");
            return false;   // hwReady stays false -> next wake retries
        }
    }

    // LATITUDE FORK patch-18: arrive LAST. After a wake, GCTL.CRST going to
    // 1 is AppleHDA bringing the shared link out of reset; initialising the
    // controller concurrently with its restore is how firmware loads fail.
    // Wait for CRST, then give AppleHDA a further settle window. If CRST
    // never appears (cold boot, load order undefined) degrade to the old
    // behaviour rather than fail — the pre-patch code never checked it.
    {
        int tries = 0;
        while (!(rd32(hda, HDA_GCTL) & 1U) && tries < 100) {   // patch-22: retries replace the long wait
            IOSleep(10);
            tries++;
        }
        bool linkUp = (rd32(hda, HDA_GCTL) & 1U) != 0;
        if (linkUp) IOSleep(750);
        { char h[56];
          snprintf(h, sizeof(h), "crst=%d tries=%d ms=%d",
                   linkUp ? 1 : 0, tries, tries * 10 + (linkUp ? 750 : 0));
          setProperty("HDA-Settle", h); }
    }

    {
        UInt64 dspLen = dspBarMap->getLength();
        UInt16 gcap = rd16(hda, HDA_GCAP);
        int numISS = (gcap >> 8) & 0xF;
        int numOSS = (gcap >> 12) & 0xF;
        { char d[64]; snprintf(d, sizeof(d), "GCAP=0x%04x ISS=%d OSS=%d sIdx=%d",
              gcap, numISS, numOSS, numISS);
          setProperty("HDA-Streams", d); }
        int sIdx = numISS; // first output stream
        UInt32 sTag = 1;
        UInt32 sd = SD_BASE + (UInt32)sIdx * SD_SIZE;
        UInt32 ppCap = 0, spibCap = 0;
        // LATITUDE FORK patch-20: snapshot of AppleHDA's state on the
        // stream we are about to borrow. Declared here, not at the point of
        // use, because the goto targets below would jump past the
        // initialisation otherwise.
        UInt32 savPPCTL = 0, savSPIBEn = 0, savSPIBVal = 0;
        UInt32 savCTL = 0, savCBL = 0, savBDPL = 0, savBDPU = 0;
        UInt16 savLVI = 0, savFMT = 0;
        bool   saved = false;
        UInt32 fwSize = (UInt32)sof_fw_size;
        UInt32 payloadOffset = 0, payloadSize = 0;
        IOBufferMemoryDescriptor *fwBuf = nullptr, *bdlBuf = nullptr;
        UInt32 numBdl = 0;
        bool fwLoaded = false;

        // EM2: set bit 14 to match Linux
        wr32(hda, HDA_VS_EM2, rd32(hda, HDA_VS_EM2) | 0x4000);
        IODelay(100);

        // ==================== HDA CONTROLLER INIT (match Linux hda_dsp_ctrl_init_chip) =========
        // Disable misc clock gating during init (PCI CGCTL bit 6)
        { UInt8 cgctl = pciDevice->configRead8(0x48);
          pciDevice->configWrite8(0x48, cgctl & ~(1U << 6)); }

        // Clear WAKESTS if controller not in reset
        if (rd32(hda, HDA_GCTL) & 1)
            wr32(hda, 0x0E, 0xFFFF);  // WAKESTS = SOF_HDA_WAKESTS_INT_MASK

        // LATITUDE FORK: GCTL global controller reset REMOVED.
        // AppleHDA has already brought the controller out of reset and is
        // actively using it; resetting here would drop its streams.
        // If firmware load fails, this is the first thing to revisit.

        // LATITUDE FORK: surgical replacement for the reference's global init.
        // The original cleared status for EVERY stream and blind-wrote INTCTL,
        // which would clobber AppleHDA's stream 0 and its interrupt enables.
        // We touch only our own loader stream and read-modify-write INTCTL.
        {
            const UInt32 kLoaderStream = 1;   // SD0 belongs to AppleHDA
            UInt32 sdOff = SD_BASE + kLoaderStream * SD_SIZE;
            wr32(hda, sdOff + SD_REG_STS, 0x1C);          // our stream only
            UInt32 ic = rd32(hda, HDA_INTCTL);            // preserve existing
            wr32(hda, HDA_INTCTL, ic | (1U << 31) | (1U << 30));
        }

        // Re-enable misc clock gating (PCI CGCTL bit 6)
        { UInt8 cgctl = pciDevice->configRead8(0x48);
          pciDevice->configWrite8(0x48, cgctl | (1U << 6)); }

        // ==================== FIRMWARE LOADING ====================

        UInt32 extSig = *(UInt32 *)sof_fw_data;
        if (extSig == 0x6e614d58) // 'XMan'
            payloadOffset = *(UInt32 *)(sof_fw_data + 4);
        payloadSize = fwSize - payloadOffset;

        if (payloadOffset >= fwSize || payloadSize == 0) {
            setProperty("Status", "FAILED: bad FW payload"), IOLog("LatSOF: %s\n", "FAILED: bad FW payload");
            goto done;
        }

        // DSP reset + power up
        wr32(dsp, DSP_ADSPCS, rd32(dsp, DSP_ADSPCS) | ADSPCS_CRST(0xF) | ADSPCS_CSTALL(0xF));
        IODelay(1000);
        wr32(dsp, DSP_ADSPCS, rd32(dsp, DSP_ADSPCS) | ADSPCS_SPA(0xF));
        if (!poll32(dsp, DSP_ADSPCS, ADSPCS_CPA(0xF), ADSPCS_CPA(0xF), 50000)) {
            setProperty("Status", "FAILED: core power"), IOLog("LatSOF: %s\n", "FAILED: core power");
            goto done;
        }

        // Allocate FW DMA buffers
        fwBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
            kIOMemoryPhysicallyContiguous | kIODirectionInOut, payloadSize, 0xFFFFFFFFFFFFF000ULL);
        if (!fwBuf) { setProperty("Status", "FAILED: FW alloc"), IOLog("LatSOF: %s\n", "FAILED: FW alloc"); goto done; }
        fwBuf->prepare();
        memcpy(fwBuf->getBytesNoCopy(), sof_fw_data + payloadOffset, payloadSize);

        numBdl = (payloadSize + PAGE_SIZE - 1) / PAGE_SIZE;
        if (numBdl > 256) numBdl = 256;
        {
            UInt32 bdlSize = ((numBdl * 16) + 127) & ~127U;
            bdlBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                kIOMemoryPhysicallyContiguous | kIODirectionInOut, bdlSize, 0xFFFFFFFFFFFFFF80ULL);
            if (!bdlBuf) { fwBuf->complete(); fwBuf->release(); fwBuf = nullptr;
                           setProperty("Status", "FAILED: BDL alloc"), IOLog("LatSOF: %s\n", "FAILED: BDL alloc"); goto done; }
            bdlBuf->prepare();
            HdaBdlEntry *bdl = (HdaBdlEntry *)bdlBuf->getBytesNoCopy();
            memset(bdl, 0, bdlSize);
            UInt32 rem = payloadSize; IOByteCount boff = 0;
            for (UInt32 i = 0; i < numBdl && rem > 0; i++) {
                IOByteCount segLen = 0;
                IOPhysicalAddress segP = fwBuf->getPhysicalSegment(boff, &segLen, 0);
                UInt32 chunk = (rem > PAGE_SIZE) ? PAGE_SIZE : rem;
                if (segLen < chunk) chunk = (UInt32)segLen;
                bdl[i].addrLow = (UInt32)(segP & 0xFFFFFFFF);
                bdl[i].addrHigh = (UInt32)(segP >> 32);
                bdl[i].size = chunk;
                bdl[i].ioc = (rem <= chunk) ? 1 : 0;
                rem -= chunk; boff += chunk;
            }
        }
        IOPhysicalAddress bdlPhys = bdlBuf->getPhysicalAddress();

        // Find PP, SPIB, and ML capabilities
        UInt32 mlCap = 0;
        {
            UInt32 hdaLen = (UInt32)hdaBarMap->getLength();
            for (UInt32 o = 0x500; o < hdaLen && o < 0x2000; o += 0x10) {
                UInt16 capId = (rd32(hda, o) >> 16) & 0xFFF;
                if (capId == HDA_CAP_ML_ID && !mlCap) mlCap = o;
                if (capId == HDA_CAP_PP_ID && !ppCap) ppCap = o;
                if (capId == HDA_CAP_SPIB_ID && !spibCap) spibCap = o;
            }
        }

        // PPCTL: PIE + GPROCEN + decouple the loader streams.
        // LATITUDE FORK: the reference also set (1U << capIdx) here, relying
        // on the member's constructor value — capIdx isn't assigned until
        // ~270 lines later, so this decoupled AppleHDA's SD0 and never SD1.
        // The capture stream now decouples in startCaptureGated().
        // LATITUDE FORK patch-20: SD(sIdx) is AppleHDA's first output
        // engine. Borrow it rather than seize it — snapshot first.
        savPPCTL   = ppCap   ? rd32(hda, ppCap + PP_PPCTL) : 0;
        savSPIBEn  = spibCap ? rd32(hda, spibCap + 0x04) : 0;
        savSPIBVal = spibCap ? rd32(hda, spibCap + 0x08 + (UInt32)sIdx * 0x08) : 0;
        savCTL     = rd32(hda, sd) & 0x00FFFFFF;   // CTL only, never SDSTS
        savCBL     = rd32(hda, sd + SD_REG_CBL);
        savLVI     = rd16(hda, sd + SD_REG_LVI);
        savFMT     = rd16(hda, sd + SD_REG_FMT);
        savBDPL    = rd32(hda, sd + SD_REG_BDLPL);
        savBDPU    = rd32(hda, sd + SD_REG_BDLPU);
        saved      = true;
        { char b[80];
          snprintf(b, sizeof(b), "sd%d ctl=0x%06x fmt=0x%04x bdl=0x%08x ppctl=0x%08x",
                   sIdx, savCTL, savFMT, savBDPL, savPPCTL);
          setProperty("SD-Borrow", b); }

        // PPCTL as read-modify-write: the original assigned the whole
        // register and so erased AppleHDA's decouple bits outright.
        if (ppCap)
            wr32(hda, ppCap + PP_PPCTL,
                 savPPCTL | (1U << 31) | PP_PPCTL_GPROCEN | (1U << sIdx) | (1U << (sIdx + 1)));

        // Program code loader stream
        streamReset(hda, sd);
        wr32(hda, sd + SD_REG_BDLPL, (UInt32)(bdlPhys & 0xFFFFFFFF));
        wr32(hda, sd + SD_REG_BDLPU, (UInt32)(bdlPhys >> 32));
        wr32(hda, sd + SD_REG_CBL, payloadSize);
        wr16(hda, sd + SD_REG_LVI, (UInt16)(numBdl - 1));
        wr16(hda, sd + SD_REG_FMT, HDA_CL_STREAM_FMT);
        wr8(hda, sd + 2, (UInt8)((sTag & 0xF) << 4));

        if (spibCap) {
            wr32(hda, spibCap + 0x04, rd32(hda, spibCap + 0x04) | (1U << sIdx));
            wr32(hda, spibCap + 0x08 + (UInt32)sIdx * 0x08, payloadSize);
        }

        // Set all SSPs to clock consumer/codec provider (CBP_CFP) mode
        // Linux: hda_ssp_set_cbp_cfp() in hda-loader.c — REQUIRED before FW load!
        // SSP base = BAR4 + 0x10000, each SSP = 0x1000, SSC1 offset = 0x4
        // CBP_CFP = BIT(25) | BIT(24) = 0x03000000
        for (int s = 0; s < 3; s++) {  // CNL_SSP_COUNT = 3
            UInt32 ssc1Off = 0x10000 + s * 0x1000 + 0x4;
            wr32(dsp, ssc1Off, rd32(dsp, ssc1Off) | 0x03000000);
        }

        // ROM IPC + core run
        wr32(dsp, IPC_HIPCIDR, IPC_BUSY | ROM_IPC_CONTROL | ROM_IPC_PURGE_FW | ((sTag - 1) << 9));
        {
            UInt32 a = rd32(dsp, DSP_ADSPCS);
            a &= ~ADSPCS_CRST(1); wr32(dsp, DSP_ADSPCS, a);
            poll32(dsp, DSP_ADSPCS, ADSPCS_CRST(1), 0, 50000);
            a = rd32(dsp, DSP_ADSPCS);
            a &= ~ADSPCS_CSTALL(1); wr32(dsp, DSP_ADSPCS, a);
        }

        if (!poll32(dsp, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000)) {
            setProperty("Status", "FAILED: ROM IPC timeout"), IOLog("LatSOF: %s\n", "FAILED: ROM IPC timeout"); goto cleanup;
        }
        wr32(dsp, IPC_HIPCIDA, rd32(dsp, IPC_HIPCIDA) | IPC_DONE);

        // Power down cores 1-3, enable IPC interrupts
        { UInt32 a = rd32(dsp, DSP_ADSPCS);
          a |= ADSPCS_CRST(0xE) | ADSPCS_CSTALL(0xE); a &= ~ADSPCS_SPA(0xE);
          wr32(dsp, DSP_ADSPCS, a); }
        wr32(dsp, IPC_HIPCCTL, 0x03);
        wr32(dsp, DSP_ADSPIC, rd32(dsp, DSP_ADSPIC) | 0x01);

        // Wait INIT_DONE → Start DMA → Wait FW_ENTERED
        { bool ok = false;
          for (int t = 0; t < 300; t++) {
              if ((rd32(dsp, ROM_STATUS) & 0xFFFFFF) == FSR_INIT_DONE) { ok = true; break; }
              IOSleep(1);
          }
          if (!ok) { setProperty("Status", "FAILED: INIT_DONE timeout"), IOLog("LatSOF: %s\n", "FAILED: INIT_DONE timeout"); goto cleanup; }
        }
        // LATITUDE FORK patch-18: loader runs interrupt-free (same rule as
        // patch-14 for capture). The load is verified by polling ROM_STATUS;
        // IOCE + INTCTL here just latch completions nobody services on the
        // shared line — and on a FAILED load the FW_ENTERED loop holds that
        // for a full 3 s, killing AppleHDA's playback mid-wake-restore.
        wr8(hda, sd + SD_REG_STS, 0x1C);
        wr8(hda, sd, (rd8(hda, sd) & ~(UInt8)SD_CTL_IOCE) | SD_CTL_RUN);
        IODelay(500);

        { UInt32 romSt = 0;
          for (int t = 0; t < 3000; t++) {
              romSt = rd32(dsp, ROM_STATUS) & 0xFFFFFF;
              if (romSt == FSR_FW_ENTERED) { fwLoaded = true; break; }
              IOSleep(1);
          }
          setProperty("FW-Entered", fwLoaded ? "OK" : "TIMEOUT");
        }

        // Stop code loader DMA (patch-18: also clear any latched status,
        // and make sure our SIE bit is off even though we no longer set it)
        wr8(hda, sd, rd8(hda, sd) & ~(UInt8)(SD_CTL_RUN | SD_CTL_IOCE));
        wr8(hda, sd + SD_REG_STS, 0x1C);
        // LATITUDE FORK patch-21: do NOT touch INTCTL here. patch-18 stopped
        // us setting bit sIdx, so this clear only ever reached into
        // AppleHDA's own stream interrupt enable and switched it off —
        // silencing playback after every wake-time firmware load.

        // LATITUDE FORK patch-20: give the stream back. This runs before
        // the fwLoaded check on purpose — a FAILED load used to leave
        // AppleHDA's playback engine pointing at our firmware buffer with
        // its format and BDL wiped, which is what killed the speakers.
        if (saved) {
            streamReset(hda, sd);
            wr32(hda, sd + SD_REG_BDLPL, savBDPL);
            wr32(hda, sd + SD_REG_BDLPU, savBDPU);
            wr32(hda, sd + SD_REG_CBL,   savCBL);
            wr16(hda, sd + SD_REG_LVI,   savLVI);
            wr16(hda, sd + SD_REG_FMT,   savFMT);
            wr16(hda, sd,     (UInt16)(savCTL & 0xFFFF));
            wr8(hda, sd + 2,  (UInt8)((savCTL >> 16) & 0xFF));
            if (spibCap) {
                wr32(hda, spibCap + 0x08 + (UInt32)sIdx * 0x08, savSPIBVal);
                wr32(hda, spibCap + 0x04, savSPIBEn);
            }
            if (ppCap) wr32(hda, ppCap + PP_PPCTL, savPPCTL);
            { char b[64];
              snprintf(b, sizeof(b), "restored ctl=0x%06x ppctl=0x%08x",
                       rd32(hda, sd) & 0x00FFFFFF, ppCap ? rd32(hda, ppCap + PP_PPCTL) : 0);
              setProperty("SD-Return", b); }
        }

        if (!fwLoaded) { setProperty("Status", "FAILED: FW load"), IOLog("LatSOF: %s\n", "FAILED: FW load"); goto cleanup; }

        // ==================== FW_READY HANDLING ====================

        { bool ready = false;
          for (int t = 0; t < 1000; t++) {
              if (rd32(dsp, IPC_HIPCTDR) & IPC_BUSY) { ready = true; break; }
              IOSleep(1);
          }
          setProperty("FW-Ready", ready ? "OK" : "TIMEOUT");
        }

        if (dspLen >= MBOX_UPLINK + 256) {
            volatile UInt8 *mb = dsp + MBOX_UPLINK;
            UInt32 hSz = rd32(mb, 0x00);
            UInt32 hCmd = rd32(mb, 0x04);

            if (hCmd == SOF_IPC_FW_READY && hSz >= 60 && hSz <= 200) {
                // Parse version
                char ver[80];
                snprintf(ver, sizeof(ver), "%d.%d.%d-%d ABI:%d.%d.%d",
                         rd16(mb, 0x1C), rd16(mb, 0x1E), rd16(mb, 0x20), rd16(mb, 0x22),
                         (rd32(mb, 0x40) >> 24) & 0xFF, (rd32(mb, 0x40) >> 12) & 0xFFF,
                         rd32(mb, 0x40) & 0xFF);
                setProperty("FW-Version", ver);
                // LATITUDE FORK: the fw_ready struct carries the authoritative
                // mailbox offsets/sizes at 0x08..0x14; the reference ignores
                // them. Report them so we can verify the hardcoded fallbacks.
                {
                    char mbx[128];
                    snprintf(mbx, sizeof(mbx),
                             "dspbox=0x%x hostbox=0x%x dspsz=%u hostsz=%u hdrsz=%u",
                             rd32(mb, 0x08), rd32(mb, 0x0C),
                             rd32(mb, 0x10), rd32(mb, 0x14), hSz);
                    setProperty("FW-Mailbox", mbx);

                    // tag is a 6-byte string at 0x3A; Linux prints this as the
                    // trailing field of its version line (expect "57864")
                    char tag[8];
                    for (int i = 0; i < 6; i++) tag[i] = (char)*(volatile UInt8 *)(mb + 0x3A + i);
                    tag[6] = 0; tag[7] = 0;
                    setProperty("FW-Tag", tag);

                    char raw[48];
                    snprintf(raw, sizeof(raw), "abi_raw=0x%08x build=%u",
                             rd32(mb, 0x40), rd16(mb, 0x22));
                    setProperty("FW-Raw", raw);
                }

                // Parse ext_data for mailbox offsets
                UInt32 inboxOff = 0, outboxOff = 0;
                UInt32 extOff = MBOX_UPLINK + hSz;
                for (int i = 0; i < 20; i++) {
                    if (extOff + 12 > dspLen) break;
                    UInt32 eSize = rd32(dsp, extOff), eCmd = rd32(dsp, extOff + 4);
                    UInt32 eType = rd32(dsp, extOff + 8);
                    if (eCmd != SOF_IPC_FW_READY || eSize < 12 || eSize > 4096) break;
                    if (eType == SOF_IPC_EXT_WINDOW) {
                        UInt32 nw = rd32(dsp, extOff + 12);
                        for (UInt32 w = 0; w < nw && w < 8; w++) {
                            UInt32 wb = extOff + 16 + w * 24;
                            if (wb + 24 > dspLen) break;
                            UInt32 wType = rd32(dsp, wb + 4), wId = rd32(dsp, wb + 8);
                            UInt32 wOff = rd32(dsp, wb + 20);
                            if (wType == SOF_IPC_REGION_UPBOX) inboxOff = SRAM_WIN(wId) + wOff;
                            if (wType == SOF_IPC_REGION_DOWNBOX) outboxOff = SRAM_WIN(wId) + wOff;
                        }
                    }
                    extOff += eSize;
                }
                if (!inboxOff) inboxOff = 0x81000;
                // Default outbox 0x80000 is ROM status area — WRONG!
                // Linux SRAM dump shows IPC messages at 0x82000
                // CML SOF mailbox: inbox=SRAM_WIN0+0x1000, outbox=SRAM_WIN0+0x2000
                if (!outboxOff) outboxOff = 0x82000;
                {
                    char ch[96];
                    snprintf(ch, sizeof(ch), "inbox=0x%x outbox=0x%x (%s)",
                             inboxOff, outboxOff,
                             (inboxOff == 0x81000 && outboxOff == 0x82000)
                                 ? "hardcoded fallback" : "from ext_data");
                    setProperty("FW-MailboxUsed", ch);
                }

                // ACK FW_READY
                wr32(dsp, IPC_HIPCTDR, rd32(dsp, IPC_HIPCTDR) | IPC_BUSY);
                wr32(dsp, IPC_HIPCTDA, IPC_DONE);

                // ==================== IPC HELPER ====================
                auto sendIpc = [&](const void *msg, UInt32 msgSize) -> UInt32 {
                    volatile UInt8 *ob = dsp + outboxOff;
                    UInt8 *src = (UInt8 *)msg;
                    for (UInt32 i = 0; i < msgSize; i += 4)
                        wr32(ob, i, *(UInt32 *)(src + i));
                    wr32(dsp, IPC_HIPCIDR, IPC_BUSY);
                    if (!poll32(dsp, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000))
                        return 0xFFFFFFFF;
                    UInt32 replyErr = rd32(ob, 8);
                    wr32(dsp, IPC_HIPCIDA, rd32(dsp, IPC_HIPCIDA) | IPC_DONE);
                    wr32(dsp, IPC_HIPCCTL, rd32(dsp, IPC_HIPCCTL) | 0x02);
                    return (replyErr != 0) ? replyErr : 0;
                };

                // Clean up FW loader state — NO GCTL reset (would break DSP DMA)
                if (spibCap) wr32(hda, spibCap + 0x04, 0);
                streamReset(hda, sd);

                // LATITUDE FORK: diagnose ext_data, then prove the downlink mailbox.
                {
                    UInt32 eo = MBOX_UPLINK + hSz;
                    char dmp[96];
                    snprintf(dmp, sizeof(dmp), "@0x%x: %08x %08x %08x %08x",
                             eo, rd32(dsp, eo), rd32(dsp, eo + 4),
                             rd32(dsp, eo + 8), rd32(dsp, eo + 12));
                    setProperty("FW-ExtData", dmp);

                    // Unknown global command. A well-behaved DSP rejects it,
                    // but still rings HIPCIDA — which is what we are testing.
                    struct { UInt32 size; UInt32 cmd; } probeMsg = { 8, 0xF0000000 };
                    UInt32 r = sendIpc(&probeMsg, 8);
                    char res[96];
                    snprintf(res, sizeof(res), "%s ret=0x%x outbox=0x%x",
                             (r == 0xFFFFFFFF) ? "TIMEOUT (downlink wrong?)"
                                               : "ROUND-TRIP OK",
                             r, outboxOff);
                    setProperty("IPC-Test", res);
                }
                // LATITUDE FORK: PIPELINE 1: SSP0 Headphone removed (no I2S codecs)
                // LATITUDE FORK: PIPELINE 7: SSP1 Speaker removed (no I2S codecs)


                // LATITUDE FORK: IPC counters — declared in the removed PIPELINE 1 block.
                int ipcOk = 0, ipcFail = 0;
                auto countIpc = [&](UInt32 r) { if (r == 0) ipcOk++; else ipcFail++; };

                // ==================== PIPELINE 3: DMIC Capture ====================
                { struct { UInt32 size, cmd, comp_id, pipeline_id, sched_id, core,
                           period, priority, period_mips, frames_per_sched,
                           xrun_limit_usecs, time_domain; } __attribute__((packed)) m = {
                      48, 0x30100000, PIPE3_SCHED_ID, PIPE3_ID, PIPE3_HOST_ID, 0,
                      1000, 0, 5000, 0, 0, TIME_TIMER };
                  countIpc(sendIpc(&m, sizeof(m))); }
                { struct { UInt32 size, cmd, id, type, pipeline_id, core, ext,
                           buf_size, caps, flags, reserved; } __attribute__((packed)) m = {
                      44, 0x30200000, PIPE3_BUF0_ID, COMP_BUFFER, PIPE3_ID, 0, 0,
                      9600, 0x71, 0, 0 };
                  countIpc(sendIpc(&m, sizeof(m))); }
                { UInt32 m[76/4] = {}; m[0]=76; m[1]=0x30010000; m[2]=PIPE3_HOST_ID;
                  m[3]=COMP_HOST; m[4]=PIPE3_ID; m[7]=36; m[9]=2; m[10]=2;
                  m[12]=FRAME_S32; m[16]=DIR_CAPTURE;
                  countIpc(sendIpc(m, 76)); }
                { UInt32 m[80/4] = {}; m[0]=80; m[1]=0x30010000; m[2]=PIPE3_DAI_ID;
                  m[3]=COMP_DAI; m[4]=PIPE3_ID; m[7]=36; m[10]=2;
                  m[12]=FRAME_S32; m[16]=DIR_CAPTURE;
                  m[17]=0; m[18]=DAI_DMIC;
                  countIpc(sendIpc(m, 80)); }
                // DAI_CONFIG for DMIC0 — match Linux kprobe [42] exactly
                { UInt8 m[216] = {};
                  *(UInt32*)(m+0)=216; *(UInt32*)(m+4)=0x80010000;
                  *(UInt32*)(m+8)=DAI_DMIC; *(UInt32*)(m+12)=0;
                  *(UInt16*)(m+16)=6; // SOF_DAI_FMT_PDM
                  UInt8 *d=m+52;
                  // Match Linux: hdr.size=0, pdmclk_min=2400000
                  *(UInt32*)(d+0)=0;         // hdr.size (Linux=0, was 164)
                  *(UInt32*)(d+4)=1;         // driver_ipc_version
                  *(UInt32*)(d+8)=2400000;   // pdmclk_min (Linux=2400000, was 500000)
                  *(UInt32*)(d+12)=4800000;  // pdmclk_max
                  *(UInt32*)(d+16)=48000;    // fifo_fs
                  *(UInt16*)(d+24)=32; *(UInt16*)(d+26)=32; // fifo_bits, fifo_bits_b
                  *(UInt16*)(d+28)=40; *(UInt16*)(d+30)=60; // duty_min, duty_max
                  *(UInt32*)(d+32)=1;        // LATITUDE FORK: 1 PDM controller (2 mics)
                  *(UInt32*)(d+44)=400;      // wake_up_time (Linux data[24]=0x190)
                  // PDM0 at DMIC+72 (DAI_CONFIG offset 124)
                  UInt8 *p0=m+124;
                  *(UInt16*)(p0+0)=0; *(UInt16*)(p0+2)=1; *(UInt16*)(p0+4)=1; // id=0, mic_a=1, mic_b=1
                  // LATITUDE FORK: PDM1 removed — this board has one PDM controller
                  countIpc(sendIpc(m, 216)); }
                { struct { UInt32 s,c,src,dst; } __attribute__((packed)) conn[] = {
                      {16, 0x30030000, PIPE3_DAI_ID, PIPE3_BUF0_ID},
                      {16, 0x30030000, PIPE3_BUF0_ID, PIPE3_HOST_ID} };
                  for (int c = 0; c < 2; c++) countIpc(sendIpc(&conn[c], 16)); }
                { struct { UInt32 s,c,id; } __attribute__((packed)) m = {12, 0x30130000, PIPE3_SCHED_ID};
                  countIpc(sendIpc(&m, 12)); }

                { char r[32]; snprintf(r, sizeof(r), "%d OK, %d FAIL", ipcOk, ipcFail);
                  setProperty("Topology", r); }


                // PM_CTX_RESTORE — Linux sends this between PIPE_COMPLETE and PCM_PARAMS
                // Clears pm_prepare_D3 flag in firmware (required for IPC processing)
                { UInt32 pm[3] = {12, 0x40020000, 0};
                  UInt32 pmr = sendIpc(pm, 12);
                  char pmstr[32]; snprintf(pmstr, sizeof(pmstr), "PM_RESTORE=%u", pmr);
                  setProperty("PM", pmstr);
                }

                // Save hardware state
                hdaBase = hda; dspBase = dsp;
                this->ppCap = ppCap; this->spibCap = spibCap; this->mlCap = mlCap;
                this->sIdx = sIdx; this->sTag = sTag; this->sd = sd;
                this->capIdx = 1; this->capTag = 2;  // LATITUDE FORK: SD0 is AppleHDA's
                this->capSd = SD_BASE + SD_SIZE;  // LATITUDE FORK: descriptor 1
                this->outboxOff = outboxOff;
                hwReady = true; isPlaying = false; isCapturing = false; activePlaybackHost = PIPE1_HOST_ID;

                // Allocate DMA buffers (same as before)
                if (!sharedDmaBuf) {
                    // Audio buffer MUST be below 4GB — compressed page table uses 20-bit PFN
                    sharedDmaBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                        kernel_task, kIOMemoryPhysicallyContiguous | kIODirectionInOut,
                        kLatSOF_BufferSize, 0x00000000FFFFF000ULL);
                    if (sharedDmaBuf) sharedDmaBuf->prepare();
                }
                if (sharedDmaBuf) bzero(sharedDmaBuf->getBytesNoCopy(), kLatSOF_BufferSize);
                if (!sharedBdlBuf) {
                    UInt32 numBdl = (kLatSOF_BufferSize + PAGE_SIZE - 1) / PAGE_SIZE;
                    UInt32 bdlSz = ((numBdl * 16) + 127) & ~127U;
                    sharedBdlBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                        kIOMemoryPhysicallyContiguous | kIODirectionInOut, bdlSz, 0xFFFFFFFFFFFFFF80ULL);
                    if (sharedBdlBuf && sharedDmaBuf) {
                        sharedBdlBuf->prepare();
                        HdaBdlEntry *abdl = (HdaBdlEntry *)sharedBdlBuf->getBytesNoCopy();
                        memset(abdl, 0, bdlSz);
                        UInt64 phys = sharedDmaBuf->getPhysicalAddress();
                        UInt32 rem = kLatSOF_BufferSize;
                        for (UInt32 i = 0; i < numBdl && rem > 0; i++) {
                            UInt32 chunk = (rem > PAGE_SIZE) ? PAGE_SIZE : rem;
                            abdl[i].addrLow = (UInt32)(phys & 0xFFFFFFFF);
                            abdl[i].addrHigh = (UInt32)(phys >> 32);
                            abdl[i].size = chunk; abdl[i].ioc = 1;
                            rem -= chunk; phys += chunk;
                        }
                    }
                }
                if (!capDmaBuf) {
                    capDmaBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
                        kernel_task, kIOMemoryPhysicallyContiguous | kIODirectionInOut,
                        kLatSOF_CapBufferSize, 0xFFFFFFFFFFFFF000ULL);
                    if (capDmaBuf) capDmaBuf->prepare();
                }
                if (capDmaBuf) bzero(capDmaBuf->getBytesNoCopy(), kLatSOF_CapBufferSize);
                if (!capBdlBuf) {
                    UInt32 numBdl = (kLatSOF_CapBufferSize + PAGE_SIZE - 1) / PAGE_SIZE;
                    UInt32 bdlSz = ((numBdl * 16) + 127) & ~127U;
                    capBdlBuf = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
                        kIOMemoryPhysicallyContiguous | kIODirectionInOut, bdlSz, 0xFFFFFFFFFFFFFF80ULL);
                    if (capBdlBuf && capDmaBuf) {
                        capBdlBuf->prepare();
                        HdaBdlEntry *cbdl = (HdaBdlEntry *)capBdlBuf->getBytesNoCopy();
                        memset(cbdl, 0, bdlSz);
                        UInt64 cphys = capDmaBuf->getPhysicalAddress();
                        UInt32 crem = kLatSOF_CapBufferSize;
                        for (UInt32 i = 0; i < numBdl && crem > 0; i++) {
                            UInt32 chunk = (crem > PAGE_SIZE) ? PAGE_SIZE : crem;
                            cbdl[i].addrLow = (UInt32)(cphys & 0xFFFFFFFF);
                            cbdl[i].addrHigh = (UInt32)(cphys >> 32);
                            cbdl[i].size = chunk; cbdl[i].ioc = 1;
                            crem -= chunk; cphys += chunk;
                        }
                    }
                }
                // LATITUDE FORK: MAX98357A GPIO removed (no such amp)


                // Initialize RT5682 AFTER all pipelines (SSP0 DAI_CONFIG enables MCLK)
                initRT5682();

                // PCM_PARAMS will be sent in startPlayback() — NOT here.
                // Sending it twice causes firmware to reject the second one.
setProperty("Status", "Ready for UserClient"), IOLog("LatSOF: %s\n", "Ready for UserClient");

            done_inner: ;
            } else {
                wr32(dsp, IPC_HIPCTDR, rd32(dsp, IPC_HIPCTDR) | IPC_BUSY);
                wr32(dsp, IPC_HIPCTDA, IPC_DONE);
            }
        }

        setProperty("Status", "OK"), IOLog("LatSOF: %s\n", "OK");

    cleanup:
        if (spibCap) wr32(hda, spibCap + 0x04, rd32(hda, spibCap + 0x04) & ~(1U << sIdx));
        wr32(hda, sd + SD_REG_BDLPL, 0); wr32(hda, sd + SD_REG_BDLPU, 0);
        wr32(hda, sd + SD_REG_CBL, 0); wr16(hda, sd + SD_REG_LVI, 0);
        if (bdlBuf) { bdlBuf->complete(); bdlBuf->release(); }
        if (fwBuf) { fwBuf->complete(); fwBuf->release(); }
    }

done:
    return hwReady;
}

// ==================== Power Management ====================

// Fast mute: cheap, idempotent. Two layers — codec mute (headphone only)
// and DMA-buffer bzero.
//
// Why we don't hard-stop DMA here:
//   Hard-stopping the stream (`wr8(sd, ~SD_CTL_RUN)`) makes silence air-tight
//   against coreaudiod's WriteMix re-filling the buffer, but has an
//   unacceptable side effect: with DMA stopped, the CoreAudio HAL detects a
//   dead stream and calls StopIO→StartIO, which tears down and rebuilds the
//   SOF pipeline. PCM_PARAMS IPC then times out (the abrupt DMA halt leaves
//   DSP state inconsistent), error recovery tries initDSP(), and DSP core
//   power-up fails too ("FAILED: core power") — leaving the kext unusable
//   until reboot.
//
// Keeping only bzero + codec-mute. Trade-off: coreaudiod may still fill
// the ring once or twice after fastMute() before it's throttled, so a
// very short loop-tone (a few hundred ms) can be audible on short
// clamshell. Acceptable — no deadlocks, no FAILED state, and on actual
// system sleep shutdownDSPGated() does a proper graceful teardown.
void LatSOFAudioDevice::fastMute() {
    if (i2cBase && isPlaying && activePlaybackHost == PIPE1_HOST_ID) {
        i2cWrite16(0x0002, 0x8080);   // HP_CTRL_1: mute L+R
    }
    if (sharedDmaBuf) {
        bzero(sharedDmaBuf->getBytesNoCopy(), kLatSOF_BufferSize);
    }
}

// Short-clamshell path: lid closes but system never reaches system sleep.
// coreaudiod gets throttled, stops filling the DMA ring, hardware keeps
// looping the tail — this is the "loop-tone" symptom. fastMute stops DMA;
// on open we restart it without re-initialising the DSP pipeline. isPlaying
// is *not* cleared, so CoreAudio's HAL doesn't know we paused and resumes
// seamlessly once DMA runs again.
// The clamshell handler no longer touches audio hardware. It only toggles a
// one-byte flag in the shared flags page. The plugin reads this flag on every
// DoIOOperation(WriteMix) cycle and, if set, bzero's its just-written output
// region — digital silence at the source, with zero risk to DMA/DSP state
// (unlike a hardware SD_CTL.RUN clear, which crashed the DSP via the HAL's
// StopIO/StartIO/PCM_PARAMS storm).
void LatSOFAudioDevice::handleClamshellChangeGated(bool closed) {
    if (flagsBuf) {
        uint8_t *f = (uint8_t *)flagsBuf->getBytesNoCopy();
        f[kLatSOF_FlagOff_ClamshellMuted] = closed ? 1 : 0;
    }
    // RT5682 HP pop-click suppression: mute/unmute codec analog path in
    // addition to the digital silence. MAX98357A (speaker amp) has no
    // I2C mute — it relies on the DMA-source digital silence above plus
    // its own datasheet-documented idle auto-mute on a DC-0 input signal.
    if (isPlaying && i2cBase && activePlaybackHost == PIPE1_HOST_ID) {
        i2cWrite16(0x0002, closed ? 0x8080 : 0x0000);   // HP_CTRL_1
    }
}

IOReturn LatSOFAudioDevice::s_handleClamshellChange(OSObject *o, void *closedArg, void *, void *, void *) {
    bool closed = ((uintptr_t)closedArg & 0x1) != 0;
    static_cast<LatSOFAudioDevice *>(o)->handleClamshellChangeGated(closed);
    return kIOReturnSuccess;
}

void LatSOFAudioDevice::shutdownDSPGated() {
    if (!hdaBase || !dspBase) return;

    // Silence first, teardown second — prevents the DMA-ring-loop artefact.
    fastMute();

    // Stop playback if running (already on workloop, call gated form directly).
    if (isPlaying) stopPlaybackGated();

    // Disable IPC interrupts
    wr32(dspBase, IPC_HIPCCTL, 0);
    wr32(dspBase, DSP_ADSPIC, 0);

    // Power down DSP cores: stall + reset + remove power
    { UInt32 a = rd32(dspBase, DSP_ADSPCS);
      a |= ADSPCS_CRST(0xF) | ADSPCS_CSTALL(0xF);
      a &= ~ADSPCS_SPA(0xF);
      wr32(dspBase, DSP_ADSPCS, a);
    }
    poll32(dspBase, DSP_ADSPCS, ADSPCS_CPA(0xF), 0, 50000);

    // Disable PPCTL
    if (ppCap) wr32(hdaBase, ppCap + PP_PPCTL, 0);

    // Stop all streams, clear INTCTL
    wr32(hdaBase, HDA_INTCTL, 0);

    // Assert HDA controller link reset
    wr32(hdaBase, HDA_GCTL, rd32(hdaBase, HDA_GCTL) & ~1U);
    for (int t = 0; t < 100; t++) { if (!(rd32(hdaBase, HDA_GCTL) & 1)) break; IODelay(1000); }

    hwReady = false;
}

// Fast, MMIO-poll-free sleep teardown. See header for rationale.
void LatSOFAudioDevice::shutdownForSleepGated() {
    // fastMute already guards sharedDmaBuf/i2cBase, safe if not alloc'd.
    fastMute();
    // State reset only. PCI D3 (driven by IOKit PM after we return) powers
    // down DSP + HDA. Wake calls initDSP() to fully rebuild — any IPC or
    // HDA-reset polls we did here would be thrown away on wake anyway.
    hwReady = false;
    isPlaying = false;
    isCapturing = false;
}

IOReturn LatSOFAudioDevice::setPowerStateGated(unsigned long powerStateOrdinal) {
    if (powerStateOrdinal == 0) {
        // Sleep — use the fast path to prevent MMIO hang-on-sleep.
        gWakeReinitPending = false;   // patch-22: cancel any retry round
        shutdownForSleepGated();
    } else {
        // Wake — full re-init (skip if already running, e.g. initial PM registration)
        if (!hwReady && pciDevice && hdaBase && dspBase) {
            // PCI power cycle
            pciDevice->setBusMasterEnable(true);
            pciDevice->setMemoryEnable(true);
            UInt8 tcsel = pciDevice->configRead8(PCI_TCSEL);
            pciDevice->configWrite8(PCI_TCSEL, tcsel & ~0x07);

            // LATITUDE FORK patch-21: rebuild the BAR mappings before any
            // register access. IOKit can tear down this device's memory
            // mappings across a power transition, leaving hdaBase/dspBase
            // pointing at nothing — which reads back as all-ones and looks
            // exactly like "the controller is not decoding yet", except no
            // amount of waiting helps (observed: GCAP 0xffff for a full
            // 3 s while AppleHDA drove the same registers happily).
            {
                volatile UInt8 *oldHda = hdaBase;
                if (hdaBarMap) { hdaBarMap->release(); hdaBarMap = nullptr; }
                if (dspBarMap) { dspBarMap->release(); dspBarMap = nullptr; }
                hdaBarMap = pciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
                dspBarMap = pciDevice->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress4);
                if (!hdaBarMap || !dspBarMap) {
                    setProperty("BAR-Remap", "FAILED"),
                        IOLog("LatSOF: %s\n", "FAILED: BAR remap on wake");
                    hdaBase = nullptr; dspBase = nullptr;
                    return kIOPMAckImplied;   // hwReady false -> next wake retries
                }
                hdaBase = (volatile UInt8 *)hdaBarMap->getVirtualAddress();
                dspBase = (volatile UInt8 *)dspBarMap->getVirtualAddress();
                { char r[48];
                  snprintf(r, sizeof(r), "OK changed=%d", (oldHda != hdaBase) ? 1 : 0);
                  setProperty("BAR-Remap", r); }
            }

            // LATITUDE FORK patch-22: do NOT init here. One synchronous
            // shot at a fixed instant is the design that failed all week.
            // Set the flag; jackPoll's 500 ms tick runs attempts with
            // preflight checks until one verifiably succeeds.
            gWakeReinitPending = true;
            gWakeTries = 0;
            gWakeTickDivider = 0;
            setProperty("Wake-Retry", "scheduled");
        }
    }
    return kIOPMAckImplied;
}

IOReturn LatSOFAudioDevice::s_setPowerState(OSObject *o, void *a0, void *, void *, void *) {
    return static_cast<LatSOFAudioDevice *>(o)->setPowerStateGated((unsigned long)(uintptr_t)a0);
}

IOReturn LatSOFAudioDevice::setPowerState(unsigned long powerStateOrdinal, IOService *whatDevice) {
    if (!commandGate) return setPowerStateGated(powerStateOrdinal);
    return commandGate->runAction(&s_setPowerState, (void *)(uintptr_t)powerStateOrdinal);
}

// System-wide power messages arrive BEFORE setPowerState — use the WillSleep
// hook to pre-mute while userspace is still alive, minimising the audible
// DMA-loop window.
void LatSOFAudioDevice::handleWillSleepGated() {
    fastMute();
}
IOReturn LatSOFAudioDevice::s_handleWillSleep(OSObject *o, void *, void *, void *, void *) {
    static_cast<LatSOFAudioDevice *>(o)->handleWillSleepGated();
    return kIOReturnSuccess;
}

// System-wide power event handler, registered via
//   rootDomain->registerInterest(gIOGeneralInterest, ...).
// This path — NOT message()/registerInterestedDriver — is the one that
// delivers kIOMessageSystemWillSleep and kIOPMMessageClamshellStateChange.
// Returning kIOReturnSuccess is required for WillSleep (otherwise PM waits
// its full timeout before proceeding).
IOReturn LatSOFAudioDevice::sPowerInterestHandler(void *target, void *refCon,
    UInt32 messageType, IOService *provider, void *messageArg, vm_size_t argSize)
{
    LatSOFAudioDevice *self = static_cast<LatSOFAudioDevice *>(target);
    if (!self) return kIOReturnSuccess;
    if (messageType == kIOMessageSystemWillSleep) {
        if (self->commandGate) self->commandGate->runAction(&s_handleWillSleep);
    } else if (messageType == kIOPMMessageClamshellStateChange) {
        // arg bitfield: bit 0 (kClamshellStateBit) = clamshell closed.
        if (self->commandGate) {
            self->commandGate->runAction(&s_handleClamshellChange, messageArg);
        }
    }
    return kIOReturnSuccess;
}

void LatSOFAudioDevice::stop(IOService *provider) {
    if (powerNotifier) {
        powerNotifier->remove();
        powerNotifier = nullptr;
    }
    rootDomain = nullptr;
    if (jackTimer) {
        jackTimer->cancelTimeout();
        if (getWorkLoop()) getWorkLoop()->removeEventSource(jackTimer);
        jackTimer->release(); jackTimer = nullptr;
    }
    PMstop();
    // PMstop already triggered setPowerState(0) → shutdownDSPGated, which
    // already stopped playback/capture. Belt-and-braces: call gated forms
    // directly (single-threaded in stop, safe without the gate).
    if (isPlaying) stopPlaybackGated();
    if (isCapturing) stopCaptureGated();
    if (commandGate) {
        if (getWorkLoop()) getWorkLoop()->removeEventSource(commandGate);
        commandGate->release();
        commandGate = nullptr;
    }
    if (sharedDmaBuf) { sharedDmaBuf->complete(); sharedDmaBuf->release(); sharedDmaBuf = nullptr; }
    if (sharedBdlBuf) { sharedBdlBuf->complete(); sharedBdlBuf->release(); sharedBdlBuf = nullptr; }
    if (capDmaBuf) { capDmaBuf->complete(); capDmaBuf->release(); capDmaBuf = nullptr; }
    if (capBdlBuf) { capBdlBuf->complete(); capBdlBuf->release(); capBdlBuf = nullptr; }
    if (flagsBuf) { flagsBuf->complete(); flagsBuf->release(); flagsBuf = nullptr; }
    if (i2cBarMap) { i2cBarMap->release(); i2cBarMap = nullptr; }
    if (i2cPciDevice) { i2cPciDevice->release(); i2cPciDevice = nullptr; }
    if (hdaBarMap) { hdaBarMap->release(); hdaBarMap = nullptr; }
    if (dspBarMap) { dspBarMap->release(); dspBarMap = nullptr; }
    if (pciDevice) { pciDevice->close(this); pciDevice->release(); pciDevice = nullptr; }
    IOService::stop(provider);
}

// ==================== Jack Detection Polling ====================

void LatSOFAudioDevice::jackPoll(IOTimerEventSource *sender) {
    // LATITUDE FORK patch-22: wake re-init retry engine. Lives here because
    // this timer already fires every 500 ms on the workloop regardless of
    // hwReady — exactly the cadence and exclusion context needed.
    if (gWakeReinitPending && !hwReady && pciDevice && hdaBase && dspBase) {
        if (++gWakeTickDivider >= 3) {            // one attempt per ~1.5 s
            gWakeTickDivider = 0;
            gWakeTries++;
            bool busy = false;
            UInt16 g = rd16(hdaBase, HDA_GCAP);
            if (g != 0xFFFF && g != 0x0000) {
                UInt32 outSd = SD_BASE + (UInt32)((g >> 8) & 0xF) * SD_SIZE;
                if (rd8(hdaBase, outSd) & SD_CTL_RUN) busy = true;  // AppleHDA playing — wait our turn
            }
            { char w[64];
              snprintf(w, sizeof(w), "n=%d gcap=0x%04x %s",
                       gWakeTries, g, busy ? "busy" : "try");
              setProperty("Wake-Retry", w); }
            if (!busy && g != 0xFFFF && g != 0x0000 && initDSP()) {
                gWakeReinitPending = false;
                setProperty("Wake-Retry-Done", "OK"),
                    IOLog("LatSOF: %s\n", "wake re-init OK");
            } else if (gWakeTries >= 12) {
                gWakeReinitPending = false;
                setProperty("Wake-Retry-Done", "GAVE UP after 12 tries"),
                    IOLog("LatSOF: %s\n", "wake re-init gave up after 12 tries");
            }
        }
    }
    if (!hwReady || !i2cBase) goto reschedule;
    {
        UInt16 ajd1 = i2cRead16(0x00F0);
        bool hpIn = (ajd1 & 0x0010) == 0;

        if (isPlaying && hpIn != lastJackState) {
            // jackPoll runs on the workloop, same as commandGate — so PM and
            // UserClient paths are already mutually excluded. No lock needed.
            lastJackState = hpIn;
            isPlaying = false;
            setProperty("Output", hpIn ? "Headphone" : "Speaker");
            // Inline stop: DMA stop + IPC TRIG_STOP + PCM_FREE
            wr8(hdaBase, sd, rd8(hdaBase, sd) & ~(UInt8)(SD_CTL_RUN | SD_CTL_IOCE | 0x08 | 0x10));
            auto sendIpc = [&](UInt32 cmd) {
                wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
                wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x02);
                IODelay(100);
                struct { UInt32 s,c,id; } __attribute__((packed)) m = {12, cmd, activePlaybackHost};
                volatile UInt8 *ob = dspBase + outboxOff;
                for (UInt32 i = 0; i < 12; i += 4) wr32(ob, i, *(UInt32*)((UInt8*)&m + i));
                wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
                poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000);
                wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
                wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x02);
            };
            sendIpc(0x60050000); // TRIG_STOP
            sendIpc(0x60030000); // PCM_FREE
            if (i2cBase && activePlaybackHost == PIPE1_HOST_ID) {
                i2cWrite16(0x0083, 0x0000);
                i2cWrite16(0x013a, 0x2000);
                i2cWrite16(0x0003, 0x0000);
                i2cWrite16(0x0002, 0x8080);
                i2cWrite16(0x0061, 0x0000);
            }
            bzero(sharedDmaBuf->getBytesNoCopy(), kLatSOF_BufferSize);

            startPlaybackGated();
        }
    }
reschedule:
    if (sender) sender->setTimeoutMS(500);
}

// ==================== Playback Control (called by UserClient) ====================

IOReturn LatSOFAudioDevice::startPlaybackGated() {
    if (!hwReady || !sharedDmaBuf || !sharedBdlBuf) return kIOReturnNotReady;
    // Stop first if already playing (allows jack re-detection)
    if (isPlaying) stopPlaybackGated();
    if (isPlaying) return kIOReturnSuccess;

    // Match Linux: BDL entries = buffer_size / host_period_bytes = 65536/16384 = 4
    UInt32 hostPeriodBytes = 16384;
    UInt32 numBdl = kLatSOF_BufferSize / hostPeriodBytes;  // = 4 (matching Linux LVI=3)

    // Clean startPlayback: match Linux exactly (no LOSIDV, no ML link, no HP stream)

    // Step 1: Enable INTCTL for this stream
    wr32(hdaBase, HDA_INTCTL, rd32(hdaBase, HDA_INTCTL) | (1U << 31) | (1U << 30) | (1U << sIdx));

    // Step 1b: PPCTL per-stream decouple (Linux hda-stream.c:521-523)
    // Must be set BEFORE stream setup to ensure DSP gateway is active
    if (ppCap) {
        wr32(hdaBase, ppCap + PP_PPCTL,
             rd32(hdaBase, ppCap + PP_PPCTL) | (1U << sIdx));
    }

    // Step 2: Stream reset (Linux does double reset)
    streamReset(hdaBase, sd);

    // Step 2b: Second reset (Linux hda-stream.c:568-591)
    streamReset(hdaBase, sd);

    // Step 2c: Rebuild BDL with period-sized entries (4 × 16KB, matching Linux)
    { HdaBdlEntry *bdl = (HdaBdlEntry *)sharedBdlBuf->getBytesNoCopy();
      memset(bdl, 0, sharedBdlBuf->getLength());
      UInt64 phys = sharedDmaBuf->getPhysicalAddress();
      for (UInt32 i = 0; i < numBdl; i++) {
          bdl[i].addrLow  = (UInt32)((phys + i * hostPeriodBytes) & 0xFFFFFFFF);
          bdl[i].addrHigh = (UInt32)((phys + i * hostPeriodBytes) >> 32);
          bdl[i].size = hostPeriodBytes;
          bdl[i].ioc  = 1;
      }
    }
    UInt64 bdlPhys = sharedBdlBuf->getPhysicalAddress();

    // Step 2d: Program stream tag FIRST (Linux hda-stream.c:602-605)
    wr8(hdaBase, sd + 2, (UInt8)((sTag & 0xF) << 4));

    // Step 2e: Set CBL
    wr32(hdaBase, sd + SD_REG_CBL, kLatSOF_BufferSize);

    // Step 2f: FMT with PPCTL couple/decouple quirk (Linux hda-stream.c:624-637)
    // CML requires: couple → write FMT → decouple
    if (ppCap) {
        // Temporarily COUPLE (clear per-stream bit) before writing format
        wr32(hdaBase, ppCap + PP_PPCTL,
             rd32(hdaBase, ppCap + PP_PPCTL) & ~(1U << sIdx));
    }
    wr16(hdaBase, sd + SD_REG_FMT, 0x0011); // 48kHz, 16-bit, 2ch
    if (ppCap) {
        // Re-DECOUPLE (set per-stream bit) after writing format
        wr32(hdaBase, ppCap + PP_PPCTL,
             rd32(hdaBase, ppCap + PP_PPCTL) | (1U << sIdx));
    }

    // Step 2g: LVI, BDL address
    wr16(hdaBase, sd + SD_REG_LVI, (UInt16)(numBdl - 1));
    wr32(hdaBase, sd + SD_REG_BDLPL, (UInt32)(bdlPhys & 0xFFFFFFFF));
    wr32(hdaBase, sd + SD_REG_BDLPU, (UInt32)(bdlPhys >> 32));

    // Step 2h: Position buffer enable (Linux hda-stream.c:654-662)
    { UInt32 dplbase = rd32(hdaBase, 0x70);
      if (!(dplbase & 0x01)) {
          wr32(hdaBase, 0x74, 0);           // DPUBASE = 0
          wr32(hdaBase, 0x70, 0x00000001);  // DPLBASE: enable bit only
      }
    }

    // Step 2i: Enable stream interrupts
    wr8(hdaBase, sd, rd8(hdaBase, sd) | SD_CTL_IOCE | 0x08 | 0x10);

    // Step 3: Detect headphone jack and select output pipeline
    // RT5682 AJD1_CTRL (0x00F0) bit 4: 0=plugged, 1=unplugged
    bool useHeadphone = false;
    if (i2cBase) {
        UInt16 ajd1 = i2cRead16(0x00F0);
        useHeadphone = (ajd1 & 0x0010) == 0;  // bit 4 LOW = jack inserted
        setProperty("Output", useHeadphone ? "Headphone" : "Speaker");
    }
    UInt32 activeHost = useHeadphone ? PIPE1_HOST_ID : PIPE7_HOST_ID;

    // Step 4: PCM_PARAMS IPC
    { struct sof_ipc_pcm_params pcm = {};
      pcm.hdr_size = sizeof(pcm);
      pcm.hdr_cmd  = 0x60010000;
      pcm.comp_id  = activeHost;
      pcm.params_size = 84;
      pcm.buffer.hdr_size = 0;
      pcm.buffer.phy_addr = (UInt32)(sharedDmaBuf->getPhysicalAddress() & 0xFFFFFFFF);
      pcm.buffer.pages = kLatSOF_BufferSize / PAGE_SIZE;
      pcm.buffer.size = kLatSOF_BufferSize;
      pcm.direction = DIR_PLAYBACK;
      pcm.frame_fmt = FRAME_S16;
      pcm.rate = 48000;
      pcm.stream_tag = (UInt16)sTag;
      pcm.channels = 2;
      pcm.sample_valid_bytes = 2;
      pcm.sample_container_bytes = 2;
      pcm.host_period_bytes = 16384;
      pcm.no_stream_position = 1;
      volatile UInt8 *ob = dspBase + outboxOff;
      for (UInt32 i = 0; i < sizeof(pcm); i += 4) wr32(ob, i, *(UInt32*)((UInt8*)&pcm + i));
      wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
      if (!poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000)) {
          setProperty("StartPlayback-Error", "PCM_PARAMS timeout");
          shutdownDSPGated(); initDSP();
          return kIOReturnTimeout;
      }
      wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
      wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x02);
    }

    // Step 5: Disable SPIB (match Linux default)
    if (spibCap) {
        wr32(hdaBase, spibCap + 0x04, rd32(hdaBase, spibCap + 0x04) & ~(1U << sIdx));
    }

    // Step 6: Start HDA DMA
    wr8(hdaBase, sd + SD_REG_STS, 0x1C);
    wr8(hdaBase, sd, rd8(hdaBase, sd) | SD_CTL_RUN | SD_CTL_IOCE | 0x08 | 0x10);
    IODelay(500);

    // Step 7: TRIG_START IPC
    wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
    wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x02);
    IODelay(100);
    { struct { UInt32 s,c,id; } __attribute__((packed)) m = {12, 0x60040000, activeHost};
      volatile UInt8 *ob = dspBase + outboxOff;
      for (UInt32 i = 0; i < 12; i += 4) wr32(ob, i, *(UInt32*)((UInt8*)&m + i));
      wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
      if (!poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000)) {
          setProperty("StartPlayback-Error", "TRIG timeout");
          shutdownDSPGated(); initDSP();
          return kIOReturnTimeout;
      }
      wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
      wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x02);
    }

    // Step 8: RT5682 PLL + HP power (only needed for headphone output)
    if (i2cBase && useHeadphone) {
        IOSleep(50);
        i2cWrite16(0x0081, 0x1481); i2cWrite16(0x0082, 0xC002);
        i2cWrite16(0x0083, 0x3100); // PLL_TRACK_1 (Linux sets this during playback)
        i2cWrite16(0x0080, 0x2000); IOSleep(15);
        i2cWrite16(0x013a, 0x3000); // BCLK control (Linux: 0x2000→0x3000 during playback)
        i2cWrite16(0x006B, 0x8001); i2cWrite16(0x0066, 0x0030);
        i2cWrite16(0x0065, 0x0240); i2cWrite16(0x0063, 0xF2AF); IOSleep(15);
        i2cWrite16(0x0061, 0x8D01); i2cWrite16(0x0062, 0x0400);
        i2cWrite16(0x0064, 0x0008); i2cWrite16(0x002A, 0xA0A0);
        i2cWrite16(0x002B, 0x0311); i2cWrite16(0x0029, 0x8080);
        i2cWrite16(0x0091, 0x0E26); i2cWrite16(0x0003, 0x0000);
        i2cWrite16(0x01DB, 0x0017); i2cWrite16(0x008E, 0x0069);
        i2cWrite16(0x0100, 0xA0A0); i2cWrite16(0x0003, 0x6000);
        i2cWrite16(0x0002, 0x0000); IOSleep(5);
        i2cWrite16(0x0125, 0x0420);
        // Check if MCLK is present (SSP0 active?)
        UInt16 clkDet = i2cRead16(0x006B);
        { char d[32]; snprintf(d, sizeof(d), "CLK=0x%04x", clkDet);
          setProperty("HP-CLK", d); }
    }


    isPlaying = true;
    activePlaybackHost = activeHost;
    setProperty("Status", "Playing"), IOLog("LatSOF: %s\n", "Playing");
    return kIOReturnSuccess;
}

IOReturn LatSOFAudioDevice::stopPlaybackGated() {
    if (!isPlaying) return kIOReturnSuccess;

    // Stop DMA first
    wr8(hdaBase, sd, rd8(hdaBase, sd) & ~(UInt8)(SD_CTL_RUN | SD_CTL_IOCE | 0x08 | 0x10));

    // Helper lambda for sending IPC
    auto sendSimpleIpc = [&](UInt32 cmd) {
        wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
        wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x02);
        IODelay(100);
        struct { UInt32 s,c,id; } __attribute__((packed)) m = {12, cmd, activePlaybackHost};
        volatile UInt8 *ob = dspBase + outboxOff;
        for (UInt32 i = 0; i < 12; i += 4) wr32(ob, i, *(UInt32*)((UInt8*)&m + i));
        wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
        poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000);
        wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
        wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x02);
    };

    sendSimpleIpc(0x60050000); // TRIG_STOP
    sendSimpleIpc(0x60030000); // PCM_FREE
    // NOTE: only send ONCE — double PCM_FREE crashes firmware (no guard like Linux ipc3-pcm.c:28)

    // Clear shared buffer
    // Restore RT5682 HP registers to idle state if headphone was active
    if (i2cBase && activePlaybackHost == PIPE1_HOST_ID) {
        i2cWrite16(0x0083, 0x0000); // PLL_TRACK_1: disable
        i2cWrite16(0x013a, 0x2000); // BCLK control: restore default
        i2cWrite16(0x0003, 0x0000); // HP_CTRL_2: clear DAC routing
        i2cWrite16(0x0002, 0x8080); // HP_CTRL_1: mute L+R
        i2cWrite16(0x0061, 0x0000); // PWR_DIG_1: power down
    }

    bzero(sharedDmaBuf->getBytesNoCopy(), kLatSOF_BufferSize);

    isPlaying = false;
    return kIOReturnSuccess;
}

UInt32 LatSOFAudioDevice::getSamplePosition() {
    if (!hwReady) return 0;
    // Use DPIB vendor-specific register (works in decouple mode, unlike LPIB)
    UInt32 dpib = rd32(hdaBase, HDA_VS_SDXDPIB_XBASE + HDA_VS_SDXDPIB_XINTERVAL * (UInt32)sIdx);
    return dpib / kLatSOF_BytesPerFrame;  // 4 bytes/frame (2ch S16)
}

IOReturn LatSOFAudioDevice::updateSPIB(UInt32 byteOffset) {
    // Keep SPIB at full buffer size so DMA loops continuously.
    // WriteMix writes data to the ring buffer; DMA reads it all.
    // Setting SPIB to a smaller value causes DMA to stall when it catches up.
    (void)byteOffset;
    return (hwReady && isPlaying) ? kIOReturnSuccess : kIOReturnNotReady;
}

// ==================== Capture Control ====================

IOReturn LatSOFAudioDevice::startCaptureGated() {
    if (!hwReady || !capDmaBuf || !capBdlBuf) return kIOReturnNotReady;
    if (isCapturing) return kIOReturnSuccess;

    UInt32 numBdl = (kLatSOF_CapBufferSize + PAGE_SIZE - 1) / PAGE_SIZE;
    UInt32 paramsErr = 0, trigErr = 0, posnOff = 0;  // LATITUDE FORK: real IPC replies

    // Enable INTCTL for capture stream
    // LATITUDE FORK: poll-only — no stream IRQs. The original enabled
    // INTCTL GIE|CIE|SIE(capIdx) here, but there is no handler and SDSTS is
    // never serviced, so BCIS latched ~94x/sec on the line AppleHDA shares.
    // The kernel throttled it and AppleHDA lost playback. Clear our SIE bit
    // instead of setting it; leave AppleHDA's own bits untouched.
    wr32(hdaBase, HDA_INTCTL, rd32(hdaBase, HDA_INTCTL) & ~(1U << capIdx));

    // LATITUDE FORK: decouple the capture stream (PPCTL bit = global SD
    // index). The init-time PPCTL write ran while capIdx was still 0, so
    // SD1 stayed COUPLED: an input stream waiting on the HDA link for
    // tag-2 data no codec sends — RUN=1, IPCs ack, DPIB pinned at 0.
    // Mirrors startPlaybackGated Step 1b (Linux hda-stream.c:521-523).
    if (ppCap) wr32(hdaBase, ppCap + PP_PPCTL,
                    rd32(hdaBase, ppCap + PP_PPCTL) | (1U << capIdx));

    // Program HDA capture stream (double reset — Linux hda-stream.c:568-591)
    streamReset(hdaBase, capSd);
    streamReset(hdaBase, capSd);
    UInt64 bdlPhys = capBdlBuf->getPhysicalAddress();
    wr32(hdaBase, capSd + SD_REG_BDLPL, (UInt32)(bdlPhys & 0xFFFFFFFF));
    wr32(hdaBase, capSd + SD_REG_BDLPU, (UInt32)(bdlPhys >> 32));
    wr32(hdaBase, capSd + SD_REG_CBL, kLatSOF_CapBufferSize);
    wr16(hdaBase, capSd + SD_REG_LVI, (UInt16)(numBdl - 1));
    // LATITUDE FORK: CML couple -> write FMT -> decouple quirk, same as
    // startPlaybackGated Step 2f (Linux hda-stream.c:624-637).
    if (ppCap) wr32(hdaBase, ppCap + PP_PPCTL,
                    rd32(hdaBase, ppCap + PP_PPCTL) & ~(1U << capIdx));
    wr16(hdaBase, capSd + SD_REG_FMT, 0x0041); // 48kHz 32-bit 2ch
    if (ppCap) wr32(hdaBase, ppCap + PP_PPCTL,
                    rd32(hdaBase, ppCap + PP_PPCTL) | (1U << capIdx));
    wr8(hdaBase, capSd + 2, (UInt8)((capTag & 0xF) << 4));
    // LATITUDE FORK: poll-only — no stream IRQs. Mask IOCE|FEIE|DEIE here
    // instead of enabling them.
    wr8(hdaBase, capSd, rd8(hdaBase, capSd) & ~(UInt8)(SD_CTL_IOCE | 0x08 | 0x10));

    // PCM_PARAMS for capture
    { struct sof_ipc_pcm_params pcm = {};
      pcm.hdr_size = sizeof(pcm);
      pcm.hdr_cmd  = 0x60010000;
      pcm.comp_id  = PIPE3_HOST_ID;
      pcm.params_size = 84;
      pcm.buffer.hdr_size = 0;
      pcm.buffer.phy_addr = (UInt32)(capDmaBuf->getPhysicalAddress() & 0xFFFFFFFF);
      pcm.buffer.pages = numBdl;
      pcm.buffer.size = kLatSOF_CapBufferSize;
      pcm.direction = DIR_CAPTURE;
      pcm.frame_fmt = FRAME_S32;
      pcm.rate = 48000;
      pcm.stream_tag = (UInt16)capTag;
      pcm.channels = kLatSOF_CapChannels;  // LATITUDE FORK: 2 (see .hpp)
      pcm.sample_valid_bytes = 4;
      pcm.sample_container_bytes = 4;
      pcm.host_period_bytes = kLatSOF_CapBufferSize / 4;  // 4 periods per buffer
      pcm.no_stream_position = 1;  // LATITUDE FORK: we poll DPIB; don't queue unacked posn IPCs
      volatile UInt8 *ob = dspBase + outboxOff;
      for (UInt32 i = 0; i < sizeof(pcm); i += 4) wr32(ob, i, *(UInt32*)((UInt8*)&pcm + i));
      wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
      if (!poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000)) {
          return kIOReturnTimeout;
      }
      paramsErr = rd32(ob, 8);           // LATITUDE FORK: sof_ipc_reply.error
      posnOff   = rd32(ob, 12);          // LATITUDE FORK: reply word 3 is actually the comp_id echoed back (35 = DMIC host component), not a position offset
      wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
      wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x02);
    }

    // Disable SPIB for capture stream (Linux: SPIB not needed for capture)
    if (spibCap) {
        wr32(hdaBase, spibCap + 0x04, rd32(hdaBase, spibCap + 0x04) & ~(1U << capIdx));
    }

    // Start DMA
    wr8(hdaBase, capSd + SD_REG_STS, 0x1C);
    // LATITUDE FORK: poll-only — no stream IRQs. Set RUN and nothing else;
    // the interrupt enables stay masked so no completion IRQ is raised.
    wr8(hdaBase, capSd, (rd8(hdaBase, capSd)
                         & ~(UInt8)(SD_CTL_IOCE | 0x08 | 0x10))
                        | SD_CTL_RUN);
    IODelay(500);

    // TRIG_START
    wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
    wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x02);
    IODelay(100);
    { struct { UInt32 s,c,id; } __attribute__((packed)) m = {12, 0x60040000, PIPE3_HOST_ID};
      volatile UInt8 *ob = dspBase + outboxOff;
      for (UInt32 i = 0; i < 12; i += 4) wr32(ob, i, *(UInt32*)((UInt8*)&m + i));
      wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
      if (!poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000)) {
          return kIOReturnTimeout;
      }
      trigErr = rd32(ob, 8);             // LATITUDE FORK: sof_ipc_reply.error
      wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
      wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x02);
    }

    // LATITUDE FORK: one-line state snapshot — read it with
    //   ioreg -rc LatSOFAudioDevice -d 1 -w0
    { char dbg[176];
      snprintf(dbg, sizeof(dbg),
          "ppctl=0x%08x ctl=0x%06x sts=0x%02x lpib=%u dpib=%u params=0x%x trig=0x%x comp_id=0x%x",
          ppCap ? rd32(hdaBase, ppCap + PP_PPCTL) : 0xFFFFFFFFu,
          rd32(hdaBase, capSd) & 0xFFFFFF,
          (unsigned)rd8(hdaBase, capSd + SD_REG_STS),
          rd32(hdaBase, capSd + 0x04 /* LPIB */),
          rd32(hdaBase, HDA_VS_SDXDPIB_XBASE + HDA_VS_SDXDPIB_XINTERVAL * (UInt32)capIdx),
          paramsErr, trigErr, posnOff);
      setProperty("Capture-Debug", dbg); }

    isCapturing = true;
    return kIOReturnSuccess;
}

IOReturn LatSOFAudioDevice::stopCaptureGated() {
    if (!isCapturing) return kIOReturnSuccess;

    // Stop DMA
    wr8(hdaBase, capSd, rd8(hdaBase, capSd) & ~(UInt8)(SD_CTL_RUN | SD_CTL_IOCE | 0x08 | 0x10));
    // LATITUDE FORK: poll-only — no stream IRQs. Clear any latched stream
    // status (BCIS | FIFOE | DESE are write-1-to-clear) and make sure our
    // SIE bit is off, so nothing is left asserting AppleHDA's shared line.
    wr8(hdaBase, capSd + SD_REG_STS, 0x1C);
    wr32(hdaBase, HDA_INTCTL, rd32(hdaBase, HDA_INTCTL) & ~(1U << capIdx));

    // TRIG_STOP + PCM_FREE
    auto sendCapIpc = [&](UInt32 cmd) {
        wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
        wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x02);
        IODelay(100);
        struct { UInt32 s,c,id; } __attribute__((packed)) m = {12, cmd, PIPE3_HOST_ID};
        volatile UInt8 *ob = dspBase + outboxOff;
        for (UInt32 i = 0; i < 12; i += 4) wr32(ob, i, *(UInt32*)((UInt8*)&m + i));
        wr32(dspBase, IPC_HIPCIDR, IPC_BUSY);
        poll32(dspBase, IPC_HIPCIDA, IPC_DONE, IPC_DONE, 500000);
        wr32(dspBase, IPC_HIPCIDA, rd32(dspBase, IPC_HIPCIDA) | IPC_DONE);
        wr32(dspBase, IPC_HIPCCTL, rd32(dspBase, IPC_HIPCCTL) | 0x02);
    };
    sendCapIpc(0x60050000); // TRIG_STOP
    sendCapIpc(0x60030000); // PCM_FREE

    // LATITUDE FORK: post-run snapshot BEFORE the ring is cleared —
    // proves whether audio landed independently of any position register.
    { volatile UInt32 *ring = (volatile UInt32 *)capDmaBuf->getBytesNoCopy();
      char st[160];
      snprintf(st, sizeof(st),
          "dpib=%u lpib=%u sts=0x%02x ring=%08x %08x %08x %08x",
          rd32(hdaBase, HDA_VS_SDXDPIB_XBASE + HDA_VS_SDXDPIB_XINTERVAL * (UInt32)capIdx),
          rd32(hdaBase, capSd + 0x04 /* LPIB */),
          (unsigned)rd8(hdaBase, capSd + SD_REG_STS),
          (unsigned)ring[0], (unsigned)ring[1], (unsigned)ring[2], (unsigned)ring[3]);
      setProperty("Capture-Stop", st); }

    // LATITUDE FORK: re-couple SD1 so the steady-state PPCTL footprint
    // is zero again outside a capture run.
    if (ppCap) wr32(hdaBase, ppCap + PP_PPCTL,
                    rd32(hdaBase, ppCap + PP_PPCTL) & ~(1U << capIdx));

    bzero(capDmaBuf->getBytesNoCopy(), kLatSOF_CapBufferSize);
    isCapturing = false;
    return kIOReturnSuccess;
}

UInt32 LatSOFAudioDevice::getCapturePosition() {
    if (!hwReady) return 0;
    UInt32 dpib = rd32(hdaBase, HDA_VS_SDXDPIB_XBASE + HDA_VS_SDXDPIB_XINTERVAL * (UInt32)capIdx);
    return dpib / kLatSOF_CapBytesPerFrame;  // 16 bytes/frame (4ch S32)
}

// ==================== Public wrappers (serialize via commandGate) ====================
//
// Every UserClient-facing entry point and every internally-callable public
// API funnels through commandGate->runAction. Combined with setPowerState
// also running under the gate, this eliminates the PM-vs-userpath races
// that previously caused clamshell-sleep deadlocks (MMIO busy-wait hangs
// when a second thread wrote DSP/HDA registers mid-teardown).
//
// Position queries (getSamplePosition/getCapturePosition) and updateSPIB
// are intentionally NOT gated — they are high-frequency, side-effect-free
// reads (DPIB register / two-bool check) and don't touch DSP state.

IOReturn LatSOFAudioDevice::s_startPlayback(OSObject *o, void *, void *, void *, void *) {
    return static_cast<LatSOFAudioDevice *>(o)->startPlaybackGated();
}
IOReturn LatSOFAudioDevice::s_stopPlayback (OSObject *o, void *, void *, void *, void *) {
    return static_cast<LatSOFAudioDevice *>(o)->stopPlaybackGated();
}
IOReturn LatSOFAudioDevice::s_startCapture (OSObject *o, void *, void *, void *, void *) {
    return static_cast<LatSOFAudioDevice *>(o)->startCaptureGated();
}
IOReturn LatSOFAudioDevice::s_stopCapture  (OSObject *o, void *, void *, void *, void *) {
    return static_cast<LatSOFAudioDevice *>(o)->stopCaptureGated();
}

IOReturn LatSOFAudioDevice::startPlayback() {
    if (!commandGate) return kIOReturnNotReady;
    return commandGate->runAction(&s_startPlayback);
}
IOReturn LatSOFAudioDevice::stopPlayback() {
    if (!commandGate) return kIOReturnNotReady;
    return commandGate->runAction(&s_stopPlayback);
}
IOReturn LatSOFAudioDevice::startCapture() {
    if (!commandGate) return kIOReturnNotReady;
    return commandGate->runAction(&s_startCapture);
}
IOReturn LatSOFAudioDevice::stopCapture() {
    if (!commandGate) return kIOReturnNotReady;
    return commandGate->runAction(&s_stopCapture);
}

// ==================== IOUserClient ====================

OSDefineMetaClassAndStructors(LatSOFAudioUserClient, IOUserClient)

const IOExternalMethodDispatch LatSOFAudioUserClient::sMethods[kLatSOF_MethodCount] = {
    [kLatSOF_StartPlayback]  = { (IOExternalMethodAction)sStart,      0, 0, 0, 0 },
    [kLatSOF_StopPlayback]   = { (IOExternalMethodAction)sStop,       0, 0, 0, 0 },
    [kLatSOF_GetPosition]    = { (IOExternalMethodAction)sGetPos,     0, 0, 1, 0 },
    [kLatSOF_UpdateSPIB]     = { (IOExternalMethodAction)sUpdateSPIB, 1, 0, 0, 0 },
    [kLatSOF_StartCapture]   = { (IOExternalMethodAction)sStartCap,   0, 0, 0, 0 },
    [kLatSOF_StopCapture]    = { (IOExternalMethodAction)sStopCap,    0, 0, 0, 0 },
    [kLatSOF_GetCapPosition] = { (IOExternalMethodAction)sGetCapPos,  0, 0, 1, 0 },
};

bool LatSOFAudioUserClient::initWithTask(task_t owningTask, void *securityToken, UInt32 type) {
    if (!IOUserClient::initWithTask(owningTask, securityToken, type)) return false;
    clientTask = owningTask; device = nullptr;
    return true;
}

bool LatSOFAudioUserClient::start(IOService *provider) {
    if (!IOUserClient::start(provider)) return false;
    device = OSDynamicCast(LatSOFAudioDevice, provider);
    return device != nullptr;
}

void LatSOFAudioUserClient::stop(IOService *provider) { IOUserClient::stop(provider); }

IOReturn LatSOFAudioUserClient::clientClose() {
    if (device) {
        // Public API — runs under commandGate, safe from races.
        device->stopPlayback();
        device->stopCapture();
    }
    terminate(); return kIOReturnSuccess;
}

IOReturn LatSOFAudioUserClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
    if (!device) return kIOReturnBadArgument;
    IOBufferMemoryDescriptor *buf = nullptr;
    if (type == kLatSOF_MemPlayback)    buf = device->getSharedBuffer();
    else if (type == kLatSOF_MemCapture) buf = device->getCaptureBuffer();
    else if (type == kLatSOF_MemFlags)   buf = device->getFlagsBuffer();
    if (!buf) return kIOReturnBadArgument;
    buf->retain();
    *memory = buf;
    *options = 0;
    return kIOReturnSuccess;
}

IOReturn LatSOFAudioUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *arguments,
    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) {
    if (selector >= kLatSOF_MethodCount) return kIOReturnBadArgument;
    return IOUserClient::externalMethod(selector, arguments,
        (IOExternalMethodDispatch *)&sMethods[selector], this, nullptr);
}

IOReturn LatSOFAudioUserClient::sStart(LatSOFAudioUserClient *t, void *r, IOExternalMethodArguments *a) {
    return t->device ? t->device->startPlayback() : kIOReturnNotReady;
}
IOReturn LatSOFAudioUserClient::sStop(LatSOFAudioUserClient *t, void *r, IOExternalMethodArguments *a) {
    return t->device ? t->device->stopPlayback() : kIOReturnNotReady;
}
IOReturn LatSOFAudioUserClient::sGetPos(LatSOFAudioUserClient *t, void *r, IOExternalMethodArguments *a) {
    if (!t->device) return kIOReturnNotReady;
    a->scalarOutput[0] = t->device->getSamplePosition();
    return kIOReturnSuccess;
}
IOReturn LatSOFAudioUserClient::sUpdateSPIB(LatSOFAudioUserClient *t, void *r, IOExternalMethodArguments *a) {
    return t->device ? t->device->updateSPIB((UInt32)a->scalarInput[0]) : kIOReturnNotReady;
}
IOReturn LatSOFAudioUserClient::sStartCap(LatSOFAudioUserClient *t, void *r, IOExternalMethodArguments *a) {
    return t->device ? t->device->startCapture() : kIOReturnNotReady;
}
IOReturn LatSOFAudioUserClient::sStopCap(LatSOFAudioUserClient *t, void *r, IOExternalMethodArguments *a) {
    return t->device ? t->device->stopCapture() : kIOReturnNotReady;
}
IOReturn LatSOFAudioUserClient::sGetCapPos(LatSOFAudioUserClient *t, void *r, IOExternalMethodArguments *a) {
    if (!t->device) return kIOReturnNotReady;
    a->scalarOutput[0] = t->device->getCapturePosition();
    return kIOReturnSuccess;
}
