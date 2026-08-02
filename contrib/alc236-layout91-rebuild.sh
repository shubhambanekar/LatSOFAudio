#!/bin/zsh
#
# Build a custom AppleALC for the Latitude 3410 (ALC236) carrying FIVE extra
# layouts, switchable by boot-arg alone — no kext swap needed to A/B.
#
# The input-strip has two independent halves, and 90/91 applied BOTH. Since
# they hiss replug-proof at a verified 48 kHz while stock-based 92 does not,
# one of the halves wakes an amp fault. 93 and 94 split them, so the 2x2 is:
#
#            layout XML keys | Platforms ADC paths | ghost mics | amp fault
#   alcid=91     dropped     |      dropped        |    none    |   YES
#   alcid=92     kept        |      kept           |  present   |   no
#   alcid=93     dropped     |      kept           |     ?      |    ?
#   alcid=94     kept        |      dropped        |     ?      |    ?
#
# If 93 or 94 removes the ghost mics without the amp fault, that is the ideal
# config. All five carry the 48 kHz rate fix except 90, kept byte-identical
# as the historical reference.
#
#   alcid=90  output-only fork of layout 15 (ghost codec inputs stripped) —
#             unchanged from alc236-layout90-rebuild.sh, kept as the rollback
#   alcid=91  layout 90 PLUS MinimumSampleRate=48000 on every node of the
#             surviving output path, so AppleHDA never advertises 44.1 kHz.
#             44.1 is the static rate (INSTALL.md §7); making it unpublishable
#             kills the cold-boot-stuck case and the FaceTime rate-drag case
#             structurally, with no resident enforcer.
#   alcid=92  stock layout 15 with ONLY MinimumSampleRate added (inputs and
#             ghost mic devices left intact). The pre-agreed fallback: log
#             archaeology (1 Aug) showed the static under layout 90 persisted
#             at an enforced 48 kHz and a control experiment concluded the 90
#             transform itself harms the headphone jack — so if 91 still
#             hisses at 48 kHz, 92 isolates the rate fix from the input-strip.
#
# Precedent for the key: stock ALC256/Platforms99.xml (sibling codec) sets
# MinimumSampleRate 48000 on its path nodes; ALC668, ALC260 and several
# ALC269 platforms do the same. Verified against this machine's Sequoia
# 15.7.7 KC by disassembly: AppleHDAPath::initPathFromXML parses the key and
# AppleHDAPath::isAudioStreamSupported hard-rejects any rate below it, so
# 44.1 disappears from the published formats — CoreAudio cannot select it.
#
set -e
W="$1"                      # volatile workdir
OUT="$HOME/Desktop/latsof-attempt2"
mkdir -p "$W" && cd "$W"
rm -rf AppleALC Lilu MacKernelSDK
git clone -q --depth 30 https://github.com/acidanthera/AppleALC.git
git clone -q --depth 30 https://github.com/acidanthera/Lilu.git
git clone -q --depth 5  https://github.com/acidanthera/MacKernelSDK.git
( cd AppleALC && git checkout -q "$(git describe --tags --abbrev=0)" )
( cd Lilu     && git checkout -q "$(git describe --tags --abbrev=0)" )
cp -R MacKernelSDK AppleALC/MacKernelSDK
cp -R MacKernelSDK Lilu/MacKernelSDK
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

info = plistlib.load(open('Info.plist','rb'))
files = info['Files']
for lid in (90, 91, 92, 93, 94):
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
                 (94,'48k-min, layout-keys kept, ADC paths dropped')):
    if not any(e.get('CodecID')==283902518 and e.get('LayoutID')==lid for e in entries):
        dup = copy.deepcopy(src[0]); dup['LayoutID']=lid
        dup['Codec'] = str(src[0].get('Codec')) + f' (layout {lid}, {tag})'
        entries.append(dup); changed = True
if changed: plistlib.dump(pc, open(pc_path,'wb'))
print('transforms ok, pinconfigs:', len(entries))
PYEOF
echo "=== build Lilu ==="
cd "$W/Lilu" && xcodebuild -configuration Debug -quiet 2>&1 | grep -E "error" || true
[ -d build/Debug/Lilu.kext ] || { echo "LILU FAILED"; exit 1; }
echo "=== build AppleALC ==="
cd "$W/AppleALC" && cp -R ../Lilu/build/Debug/Lilu.kext .
xcodebuild -configuration Release CODE_SIGNING_ALLOWED=NO 2>&1 | tail -2
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
gate("662 pinconfigs", len(e)==662, str(len(e)))
for lid in (90, 91, 92, 93, 94):
    hit = [x for x in e if x.get('CodecID')==283902518 and x.get('LayoutID')==lid]
    gate(f"(ALC236,{lid}) present with ConfigData", len(hit)==1 and bool(hit[0].get('ConfigData')))
blob = open(K+'/Contents/MacOS/AppleALC','rb').read()
for lid in (90, 91, 92, 93, 94):
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
gate("22/22 tables", m and m.group(2)=='22' and m.group(4)=='22',
     m and f"{m.group(2)}/{m.group(4)}" or "no match")
gate("Platforms93 4x MinimumSampleRate", pinflate(93).count(b'MinimumSampleRate')==4)
gate("Platforms94 4x MinimumSampleRate", pinflate(94).count(b'MinimumSampleRate')==4)
l93 = zlib.decompress(open(W+'/AppleALC/Resources/ALC236/layout93.xml.zlib','rb').read())
l94 = zlib.decompress(open(W+'/AppleALC/Resources/ALC236/layout94.xml.zlib','rb').read())
gate("layout93 inputs stripped", b'<key>Inputs</key>' not in l93)
gate("layout94 inputs intact",  b'<key>Inputs</key>' in l94)
print("ALL GATES PASS" if ok else "GATE FAILURE")
sys.exit(0 if ok else 1)
PYEOF
rm -rf "$OUT/AppleALC-multilayout.kext"
cp -R "$W/AppleALC/build/Release/AppleALC.kext" "$OUT/AppleALC-multilayout.kext"
echo "STAGED PERSISTENT: $OUT/AppleALC-multilayout.kext (alcid 90 / 91 / 92 / 93 / 94)"
md5 -q "$OUT/AppleALC-multilayout.kext/Contents/MacOS/AppleALC"
