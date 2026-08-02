#!/bin/zsh
#
# Build a custom AppleALC for the Latitude 3410 (ALC236) carrying SIX extra
# layouts, switchable by boot-arg alone — no kext swap needed to A/B.
#
# THE 2x2 (tested on hardware 1 Aug, all four confirmed):
#
#   alcid | layout XML keys | Platforms ADC | ghost mics | headphones
#   ------+-----------------+---------------+------------+---------------------
#     92  | kept            | kept          | present    | clean   <- shipping
#   90/91 | dropped         | dropped       | none       | STATIC
#     93  | dropped         | kept          | -          | ZERO DEVICES, thrash
#     94  | kept            | dropped       | none       | STATIC
#
# Conclusions: the ghost mics come from the Platforms ADC PATHS (94 kept every
# layout key and they still vanished), and deleting those paths is exactly what
# breaks the headphone amp — at a verified 48 kHz and verified D0, so neither
# the rate fault nor the D3 fault. Orphan paths without owning declarations (93)
# make AppleHDA build nothing at all. So the ghost mics cannot be removed by
# editing the path map: the one edit that removes them is the one that breaks
# the amp.
#
#     95  | kept            | kept          | ?          | ?   <- the new idea
#
# 95 attacks the other end: every path is left intact and the CODEC is told the
# ghost pins are absent (ConfigData 0x411111f0 on 0x12 Mic-In and 0x19 Line-In,
# leaving 0x14 Speaker and 0x21 HP-Out untouched). If AppleHDA declines to build
# an input device for a pin with no physical connection, the ghost mics go away
# with the output path never modified — no amp fault.
#
# 90 is kept byte-identical as the historical reference; all others carry the
# 48 kHz rate fix.
#
# Precedent for the key: stock ALC256/Platforms99.xml (sibling codec) sets
# MinimumSampleRate 48000 on its path nodes; ALC668, ALC260 and several
# ALC269 platforms do the same. Verified against this machine's Sequoia
# 15.7.7 KC by disassembly: AppleHDAPath::initPathFromXML parses the key and
# AppleHDAPath::isAudioStreamSupported hard-rejects any rate below it, so
# 44.1 disappears from the published formats — CoreAudio cannot select it.
#
set -e
W="$1"                      # workdir — REUSED across runs, see below
OUT="$HOME/Desktop/LatSOF/latsof-attempt2"
mkdir -p "$W" && cd "$W"

# BE KIND TO THE MACHINE. Learned the hard way on 1 Aug: this build drove the
# load average past 200 twice and forced a hard reboot, because
# ResourceConverter/generate.sh reformats every plist of every codec in the
# repo with `xargs -P $(getconf _NPROCESSORS_ONLN)` — 8-way parallel, thousands
# of short-lived perl processes. That churn, not CPU or RAM, is what makes the
# desktop unusable. Three mitigations, in order of how much they save:
#
#   1. REUSE the workdir. A fresh clone has no .md5 caches, so every one of
#      ~2600 files is reprocessed (~60 min). Reusing skips all unchanged ones.
#   2. Cap that parallelism to 2 so the machine stays responsive.
#   3. nice the whole build.
#
# Pass a second argument "fresh" to force a clean re-clone.
if [ "$2" = "fresh" ] || [ ! -d "$W/AppleALC/.git" ]; then
    echo "=== fresh clone (slow: no resource caches) ==="
    rm -rf AppleALC Lilu MacKernelSDK
    git clone -q --depth 30 https://github.com/acidanthera/AppleALC.git
    git clone -q --depth 30 https://github.com/acidanthera/Lilu.git
    git clone -q --depth 5  https://github.com/acidanthera/MacKernelSDK.git
    ( cd AppleALC && git checkout -q "$(git describe --tags --abbrev=0)" )
    ( cd Lilu     && git checkout -q "$(git describe --tags --abbrev=0)" )
    cp -R MacKernelSDK AppleALC/MacKernelSDK
    cp -R MacKernelSDK Lilu/MacKernelSDK
else
    echo "=== reusing $W (resource caches intact) ==="
    # Discard only OUR edits; keep every .md5 and .zlib so the slow reformat
    # pass has nothing to redo. Untracked generated files are ours by
    # definition — the transforms below regenerate them.
    ( cd AppleALC && git checkout -q -- Resources 2>/dev/null || true )
fi

# Cap the reformat parallelism (mitigation 2). Idempotent.
sed -i '' 's/xargs -P \$(getconf _NPROCESSORS_ONLN)/xargs -P 2/' \
    "$W/AppleALC/ResourceConverter/generate.sh" 2>/dev/null || true

echo "=== transforms ==="
cd "$W/AppleALC/Resources/ALC236"
python3 - <<'PYEOF'
import plistlib, re, copy
WRAP_A = b'<?xml version="1.0"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "d.dtd"><plist version="1.0">'
WRAP_B = b'</plist>'
def load_frag(p): return plistlib.loads(WRAP_A + open(p,'rb').read() + WRAP_B)
def dump_frag(obj, p):
    txt = plistlib.dumps(obj).decode()
    txt = re.sub(r'^.*?<plist version="1.0">\s*', '', txt, flags=re.S)
    txt = re.sub(r'\s*</plist>\s*$', '\n', txt, flags=re.S)
    open(p,'w').write(txt)

def nodes_in(x, acc):
    if isinstance(x, dict):
        if 'NodeID' in x: acc.append(x['NodeID'])
        for v in x.values(): nodes_in(v, acc)
    elif isinstance(x, list):
        for v in x: nodes_in(v, acc)
    return acc

def add_minrate(x, acc):
    if isinstance(x, dict):
        if 'NodeID' in x:
            x['MinimumSampleRate'] = 48000
            acc.append(x['NodeID'])
        for v in x.values(): add_minrate(v, acc)
    elif isinstance(x, list):
        for v in x: add_minrate(v, acc)
    return acc

OUT_PATH_NODES = [20, 2, 33, 3]   # speaker pin<-DAC + headphone pin<-DAC

# layouts that KEEP the stock layout XML (inputs intact): 92 and 94
lay = load_frag('layout15.xml')
lay['LayoutID'] = 92; dump_frag(lay, 'layout92.xml')
lay['LayoutID'] = 94; dump_frag(lay, 'layout94.xml')
# — then strip the ghost inputs and emit the layouts that drop them
for ref in lay['PathMapRef']:
    for k in ('Inputs','Mic','LineIn'): ref.pop(k, None)
lay['LayoutID'] = 90; dump_frag(lay, 'layout90.xml')
lay['LayoutID'] = 91; dump_frag(lay, 'layout91.xml')
lay['LayoutID'] = 93; dump_frag(lay, 'layout93.xml')

# Platforms 90: drop the ADC paths (nodes 7/8/9), keep the output path
plat = load_frag('Platforms15.xml')
for pm in plat['PathMaps']:
    pm['PathMap'] = [p for p in pm['PathMap'] if not any(n in (7,8,9) for n in nodes_in(p, []))]
dump_frag(plat, 'Platforms90.xml')

# Platforms 91: same, plus MinimumSampleRate on every surviving path node
tagged = []
for pm in plat['PathMaps']:
    add_minrate(pm['PathMap'], tagged)
dump_frag(plat, 'Platforms91.xml')
print('91 MinimumSampleRate nodes:', tagged)
assert tagged == OUT_PATH_NODES, tagged

# Platforms 92: stock paths KEPT (ADC paths and all); MinimumSampleRate goes
# only on the output path, identified as the one free of nodes 7/8/9
plat = load_frag('Platforms15.xml')
tagged92 = []
for pm in plat['PathMaps']:
    for p in pm['PathMap']:
        if not any(n in (7,8,9) for n in nodes_in(p, [])):
            add_minrate(p, tagged92)
dump_frag(plat, 'Platforms92.xml')
print('92 MinimumSampleRate nodes:', tagged92)
assert tagged92 == OUT_PATH_NODES, tagged92

# 93 and 94 SPLIT the input-strip, to find which half wakes the amp fault
# that makes 90/91 hiss replug-proof at a verified 48 kHz:
#   93 = layout XML keys dropped, Platforms ADC paths KEPT
#   94 = layout XML keys kept,    Platforms ADC paths DROPPED
# Both carry the rate fix. If either kills the ghost mics without the amp
# fault, that is the ideal config: no ghost mics AND clean headphones.
# Platforms93 == Platforms92 (stock paths + minrate); only its layout differs.
dump_frag(plat, 'Platforms93.xml')

plat = load_frag('Platforms15.xml')
for pm in plat['PathMaps']:
    pm['PathMap'] = [p for p in pm['PathMap'] if not any(n in (7,8,9) for n in nodes_in(p, []))]
tagged94 = []
for pm in plat['PathMaps']:
    add_minrate(pm['PathMap'], tagged94)
dump_frag(plat, 'Platforms94.xml')
print('94 MinimumSampleRate nodes:', tagged94)
assert tagged94 == OUT_PATH_NODES, tagged94

# layout 95 = layout 92 exactly (stock paths + minrate, inputs declared), but
# the ghost mic PINS are disabled in the codec's own pin configuration rather
# than by editing the path map. Decoded from the stock ConfigData:
#   0x12 = 0x90a60100  fixed internal  Mic In   <- ghost "Built-in Microphone"
#   0x14 = 0x90100110  fixed internal  Speaker      keep
#   0x19 = 0x008b1020  jack            Line In  <- ghost "Built-in Line Input"
#   0x21 = 0x002b1030  jack            HP Out       keep
# The 2x2 proved the ghost mics come from the Platforms ADC paths, and that
# deleting those paths is what breaks the headphone amp. This attacks the
# other end: leave every path intact and have the CODEC report 0x12/0x19 as
# absent (0x411111f0), so AppleHDA has no pin to build those inputs on. The
# output pins are untouched, which is why the amp fault should not follow.
#
# layout 96 = the same idea as 95 but disabling ONLY pin 0x12.
#
# WHY 95 FAILED, and why this is not a repeat. Decoding the stock pin configs
# shows 0x19 (Line In) and 0x21 (HP Out) both carry connection type 0xB =
# COMBINATION: they are the two halves of ONE physical combo headset jack.
# Telling AppleHDA that 0x19 does not exist therefore breaks the jack's
# detect/switch logic, and the headphone half dies with it — which is exactly
# what 91, 94 and 95 each did by a different route, and why all three hissed.
#
# The two "ghost" inputs are NOT equivalent:
#   0x12  Fixed internal, Mic In, connection "Other Digital" = the internal
#         DMIC. Genuinely phantom — that mic is wired to the SOF DSP, not the
#         codec (the whole reason this project's kext exists). Nothing else
#         depends on it, and it is not part of any jack.
#   0x19  a REAL function of the combo jack (headset mic). Not a ghost, and
#         load-bearing for the headphone output.
#
# So 96 disables 0x12 only. Expected: "Built-in Microphone" disappears,
# "Built-in Line Input" remains (correctly — it is the headset mic), and the
# headphone path is untouched in every respect.
lay = load_frag('layout15.xml')
lay['LayoutID'] = 96; dump_frag(lay, 'layout96.xml')
plat96 = load_frag('Platforms15.xml')
t96 = []
for pm in plat96['PathMaps']:
    for p_ in pm['PathMap']:
        if not any(n in (7,8,9) for n in nodes_in(p_, [])):
            add_minrate(p_, t96)
dump_frag(plat96, 'Platforms96.xml')
assert t96 == OUT_PATH_NODES, t96

lay = load_frag('layout15.xml')
lay['LayoutID'] = 95; dump_frag(lay, 'layout95.xml')
# Platforms95 == Platforms92: stock paths, minrate on the output path only.
plat = load_frag('Platforms15.xml')
tagged95 = []
for pm in plat['PathMaps']:
    for p in pm['PathMap']:
        if not any(n in (7,8,9) for n in nodes_in(p, [])):
            add_minrate(p, tagged95)
dump_frag(plat, 'Platforms95.xml')
assert tagged95 == OUT_PATH_NODES, tagged95

# layout 97 — THE DERIVED ANSWER (see header). Remove ONLY the internal
# DMIC's input, consistently in both files, touching nothing of the combo
# jack. Node map: ADC path [8,35,18] ends at pin 0x12 (phantom DMIC);
# [9,34,25] ends at pin 0x19 (combo-jack mic — LOAD-BEARING, 4 proofs).
#   layout: Inputs ['Mic','LineIn'] -> ['LineIn']; drop the 'Mic' dict only
#   Platforms: drop only the path containing node 18; keep [9,34,25] + output
#   ConfigData: stock (pin games proved useless in 95)
lay = load_frag('layout15.xml')
for ref in lay['PathMapRef']:
    if 'Inputs' in ref:
        ref['Inputs'] = [x for x in ref['Inputs'] if x != 'Mic']
    ref.pop('Mic', None)
lay['LayoutID'] = 97; dump_frag(lay, 'layout97.xml')
plat97 = load_frag('Platforms15.xml')
for pm in plat97['PathMaps']:
    pm['PathMap'] = [p_ for p_ in pm['PathMap'] if 18 not in nodes_in(p_, [])]
t97 = []
for pm in plat97['PathMaps']:
    for p_ in pm['PathMap']:
        if not any(n in (7,8,9) for n in nodes_in(p_, [])):
            add_minrate(p_, t97)
dump_frag(plat97, 'Platforms97.xml')
assert t97 == OUT_PATH_NODES, t97
remaining = set()
for pm in plat97['PathMaps']:
    for p_ in pm['PathMap']: remaining.update(nodes_in(p_, []))
assert 18 not in remaining and 8 not in remaining and 35 not in remaining, remaining
assert {9, 34, 25} <= remaining, remaining
print('97: Mic path removed; LineIn path preserved; nodes =', sorted(remaining))

info = plistlib.load(open('Info.plist','rb'))
files = info['Files']
for lid in (90, 91, 92, 93, 94, 95, 96, 97):
    if not any(e.get('Id')==lid for e in files['Layouts']):
        files['Layouts'].append({'Id':lid,'Path':f'layout{lid}.xml.zlib'})
        files['Platforms'].append({'Id':lid,'Path':f'Platforms{lid}.xml.zlib'})
plistlib.dump(info, open('Info.plist','wb'))

pc_path = '../PinConfigs.kext/Contents/Info.plist'
pc = plistlib.load(open(pc_path,'rb'))
entries = pc['IOKitPersonalities']['as.vit9696.AppleALC']['HDAConfigDefault']
src = [e for e in entries if e.get('CodecID')==283902518 and e.get('LayoutID')==15]
assert len(src)==1
changed = False
for lid, tag in ((90,'output-only fork'), (91,'output-only + 48k-min fork'),
                 (92,'stock-15 + 48k-min fork'),
                 (93,'48k-min, layout-keys dropped, ADC paths kept'),
                 (94,'48k-min, layout-keys kept, ADC paths dropped'),
                 (95,'48k-min, pins 0x12+0x19 disabled (FAILS: 0x19 is combo-jack)'),
                 (96,'48k-min, ONLY pin 0x12 disabled (internal DMIC)'),
                 (97,'48k-min, Mic input removed consistently; combo jack stock')):
    if not any(e.get('CodecID')==283902518 and e.get('LayoutID')==lid for e in entries):
        dup = copy.deepcopy(src[0]); dup['LayoutID']=lid
        dup['Codec'] = str(src[0].get('Codec')) + f' (layout {lid}, {tag})'
        entries.append(dup); changed = True
if changed: plistlib.dump(pc, open(pc_path,'wb'))

# layout 95 only: rewrite its ConfigData so the codec reports the ghost mic
# pins as absent. Each pin default is four verbs (0x71C..0x71F) carrying one
# byte each of a 32-bit config, little-end first; a verb word is
# (nid << 20) | (verb << 8) | data. 0x411111f0 is the standard
# "no physical connection" value.
DISABLE = 0x411111f0
GHOST_PINS = (0x12, 0x19)          # 0x12 Mic In (internal), 0x19 Line In (jack)
e95 = [x for x in entries if x.get('CodecID')==283902518 and x.get('LayoutID')==95]
assert len(e95) == 1
cd = bytearray(e95[0]['ConfigData'])
patched = 0
for i in range(0, len(cd) - 3, 4):
    w = int.from_bytes(cd[i:i+4], 'big')
    nid, verb = (w >> 20) & 0xFF, (w >> 8) & 0xFFF
    if nid in GHOST_PINS and 0x71C <= verb <= 0x71F:
        byte = (DISABLE >> (8 * (verb - 0x71C))) & 0xFF
        cd[i:i+4] = ((w & 0xFFFFFF00) | byte).to_bytes(4, 'big')
        patched += 1
assert patched == 8, f'expected 8 verb writes (2 pins x 4), patched {patched}'
e95[0]['ConfigData'] = bytes(cd)
print('layout 95 pin-disable: patched', patched, 'verbs on pins',
      [hex(p) for p in GHOST_PINS])

# layout 96: ONLY 0x12 (the internal DMIC). 0x19 is half of the combo jack
# and must stay — see the block comment above layout 96.
e96 = [x for x in entries if x.get('CodecID')==283902518 and x.get('LayoutID')==96]
assert len(e96) == 1
cd6 = bytearray(e96[0]['ConfigData'])
p6 = 0
for i in range(0, len(cd6) - 3, 4):
    w = int.from_bytes(cd6[i:i+4], 'big')
    nid, verb = (w >> 20) & 0xFF, (w >> 8) & 0xFFF
    if nid == 0x12 and 0x71C <= verb <= 0x71F:
        byte = (DISABLE >> (8 * (verb - 0x71C))) & 0xFF
        cd6[i:i+4] = ((w & 0xFFFFFF00) | byte).to_bytes(4, 'big')
        p6 += 1
assert p6 == 4, f'expected 4 verb writes (1 pin x 4), patched {p6}'
e96[0]['ConfigData'] = bytes(cd6)
plistlib.dump(pc, open(pc_path,'wb'))
print('layout 96 pin-disable: patched', p6, 'verbs on pin 0x12 only')

print('transforms ok, pinconfigs:', len(entries))
PYEOF
echo "=== build Lilu ==="
cd "$W/Lilu" && nice -n 10 xcodebuild -configuration Debug -quiet 2>&1 | grep -E "error" || true
[ -d build/Debug/Lilu.kext ] || { echo "LILU FAILED"; exit 1; }
echo "=== build AppleALC ==="
cd "$W/AppleALC" && cp -R ../Lilu/build/Debug/Lilu.kext .
nice -n 10 xcodebuild -configuration Release CODE_SIGNING_ALLOWED=NO 2>&1 | tail -2
[ -d build/Release/AppleALC.kext ] || { echo "APPLEALC FAILED"; exit 1; }
echo "=== gates ==="
python3 - "$W" <<'PYEOF'
import plistlib, re, sys, zlib
W = sys.argv[1]
K = W + '/AppleALC/build/Release/AppleALC.kext'
ok = True
def gate(n, c, d=""):
    global ok; print(("PASS " if c else "FAIL ")+n+("  "+d if d else "")); ok = ok and c
d = plistlib.load(open(K+'/Contents/Info.plist','rb'))
e = d['IOKitPersonalities']['as.vit9696.AppleALC']['HDAConfigDefault']
gate("665 pinconfigs", len(e)==665, str(len(e)))
for lid in (90, 91, 92, 93, 94, 95, 96, 97):
    hit = [x for x in e if x.get('CodecID')==283902518 and x.get('LayoutID')==lid]
    gate(f"(ALC236,{lid}) present with ConfigData", len(hit)==1 and bool(hit[0].get('ConfigData')))
blob = open(K+'/Contents/MacOS/AppleALC','rb').read()
for lid in (90, 91, 92, 93, 94, 95, 96, 97):
    lz = open(W+f'/AppleALC/Resources/ALC236/layout{lid}.xml.zlib','rb').read()
    pz = open(W+f'/AppleALC/Resources/ALC236/Platforms{lid}.xml.zlib','rb').read()
    lr = open(W+f'/AppleALC/Resources/ALC236/layout{lid}.xml','rb').read()
    gate(f"layout{lid} zlib in binary", lz in blob, f"{len(lz)}B")
    gate(f"Platforms{lid} zlib in binary", pz in blob, f"{len(pz)}B")
    gate(f"layout{lid} raw XML absent", lr not in blob)
    gate(f"inflates to LayoutID {lid}", f'<integer>{lid}</integer>'.encode() in zlib.decompress(lz))
def pinflate(lid):
    return zlib.decompress(open(W+f'/AppleALC/Resources/ALC236/Platforms{lid}.xml.zlib','rb').read())
gate("Platforms90 unchanged (no MinimumSampleRate)", pinflate(90).count(b'MinimumSampleRate')==0)
gate("Platforms91 4x MinimumSampleRate", pinflate(91).count(b'MinimumSampleRate')==4)
gate("Platforms92 4x MinimumSampleRate", pinflate(92).count(b'MinimumSampleRate')==4)
l91 = zlib.decompress(open(W+'/AppleALC/Resources/ALC236/layout91.xml.zlib','rb').read())
l92 = zlib.decompress(open(W+'/AppleALC/Resources/ALC236/layout92.xml.zlib','rb').read())
gate("layout91 inputs stripped", b'<key>Inputs</key>' not in l91)
gate("layout92 inputs intact",  b'<key>Inputs</key>' in l92)
src = open(W+'/AppleALC/AppleALC/kern_resources.cpp').read()
m = re.search(r'DEBUG_STRING\("ALC236"\),\s*0x236,\s*\w+,\s*\d+,\s*(\w+),\s*(\d+),\s*(\w+),\s*(\d+)', src)
gate("25/25 tables", m and m.group(2)=='25' and m.group(4)=='25',
     m and f"{m.group(2)}/{m.group(4)}" or "no match")
gate("Platforms93 4x MinimumSampleRate", pinflate(93).count(b'MinimumSampleRate')==4)
gate("Platforms94 4x MinimumSampleRate", pinflate(94).count(b'MinimumSampleRate')==4)
l93 = zlib.decompress(open(W+'/AppleALC/Resources/ALC236/layout93.xml.zlib','rb').read())
l94 = zlib.decompress(open(W+'/AppleALC/Resources/ALC236/layout94.xml.zlib','rb').read())
gate("layout93 inputs stripped", b'<key>Inputs</key>' not in l93)
gate("layout94 inputs intact",  b'<key>Inputs</key>' in l94)
cd95 = [x for x in e if x.get('CodecID')==283902518 and x.get('LayoutID')==95][0]['ConfigData']
cd92 = [x for x in e if x.get('CodecID')==283902518 and x.get('LayoutID')==92][0]['ConfigData']
def decode(cd):
    pins={}
    for i in range(0, len(cd)-3, 4):
        w=int.from_bytes(cd[i:i+4],'big'); nid=(w>>20)&0xFF; vb=(w>>8)&0xFFF; da=w&0xFF
        if 0x71C<=vb<=0x71F: pins.setdefault(nid,[0,0,0,0])[vb-0x71C]=da
    return {n:(b[3]<<24|b[2]<<16|b[1]<<8|b[0]) for n,b in pins.items()}
p95, p92 = decode(cd95), decode(cd92)
gate("95: pin 0x12 disabled", p95.get(0x12)==0x411111f0, hex(p95.get(0x12,0)))
gate("95: pin 0x19 disabled", p95.get(0x19)==0x411111f0, hex(p95.get(0x19,0)))
gate("95: speaker 0x14 untouched", p95.get(0x14)==p92.get(0x14), hex(p95.get(0x14,0)))
gate("95: headphone 0x21 untouched", p95.get(0x21)==p92.get(0x21), hex(p95.get(0x21,0)))
gate("92 pins unchanged by the patch", p92.get(0x12)==0x90a60100 and p92.get(0x19)==0x008b1020)
cd96 = [x for x in e if x.get('CodecID')==283902518 and x.get('LayoutID')==96][0]['ConfigData']
p96 = decode(cd96)
gate("96: pin 0x12 disabled (internal DMIC)", p96.get(0x12)==0x411111f0, hex(p96.get(0x12,0)))
gate("96: pin 0x19 KEPT (combo jack half)", p96.get(0x19)==p92.get(0x19), hex(p96.get(0x19,0)))
gate("96: speaker 0x14 untouched", p96.get(0x14)==p92.get(0x14), hex(p96.get(0x14,0)))
gate("96: headphone 0x21 untouched", p96.get(0x21)==p92.get(0x21), hex(p96.get(0x21,0)))
WRAP_A = b'<?xml version="1.0"?><!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "d.dtd"><plist version="1.0">'
WRAP_B = b'</plist>'
l97 = plistlib.loads(WRAP_A + zlib.decompress(open(W+'/AppleALC/Resources/ALC236/layout97.xml.zlib','rb').read()) + WRAP_B)
ref97 = l97['PathMapRef'][0]
gate("97: Inputs == [LineIn]", ref97.get('Inputs')==['LineIn'], str(ref97.get('Inputs')))
gate("97: Mic dict removed",   'Mic' not in ref97)
gate("97: LineIn dict kept",   'LineIn' in ref97)
p97pl = plistlib.loads(WRAP_A + zlib.decompress(open(W+'/AppleALC/Resources/ALC236/Platforms97.xml.zlib','rb').read()) + WRAP_B)
def walk(x, acc):
    if isinstance(x, dict):
        if 'NodeID' in x: acc.add(x['NodeID'])
        for v in x.values(): walk(v, acc)
    elif isinstance(x, list):
        for v in x: walk(v, acc)
    return acc
n97 = set()
for pm in p97pl['PathMaps']: walk(pm['PathMap'], n97)
gate("97: DMIC path gone (no 8/35/18)", not ({8,35,18} & n97), str(sorted(n97)))
gate("97: LineIn path kept (9,34,25)", {9,34,25} <= n97)
gate("97: output path kept (20,2,33,3)", {20,2,33,3} <= n97)
cd97 = [x for x in e if x.get('CodecID')==283902518 and x.get('LayoutID')==97][0]['ConfigData']
gate("97: ConfigData stock (== 92)", cd97 == cd92)
print("ALL GATES PASS" if ok else "GATE FAILURE")
sys.exit(0 if ok else 1)
PYEOF
rm -rf "$OUT/AppleALC-multilayout.kext"
cp -R "$W/AppleALC/build/Release/AppleALC.kext" "$OUT/AppleALC-multilayout.kext"
echo "STAGED PERSISTENT: $OUT/AppleALC-multilayout.kext (alcid 90-97)"
md5 -q "$OUT/AppleALC-multilayout.kext/Contents/MacOS/AppleALC"
