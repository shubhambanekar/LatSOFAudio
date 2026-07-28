#!/bin/zsh
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
lay = load_frag('layout15.xml'); lay['LayoutID'] = 90
for ref in lay['PathMapRef']:
    for k in ('Inputs','Mic','LineIn'): ref.pop(k, None)
dump_frag(lay, 'layout90.xml')
plat = load_frag('Platforms15.xml')
def nodes_in(x, acc):
    if isinstance(x, dict):
        if 'NodeID' in x: acc.append(x['NodeID'])
        for v in x.values(): nodes_in(v, acc)
    elif isinstance(x, list):
        for v in x: nodes_in(v, acc)
    return acc
for pm in plat['PathMaps']:
    pm['PathMap'] = [p for p in pm['PathMap'] if not any(n in (7,8,9) for n in nodes_in(p, []))]
dump_frag(plat, 'Platforms90.xml')
info = plistlib.load(open('Info.plist','rb'))
files = info['Files']
if not any(e.get('Id')==90 for e in files['Layouts']):
    files['Layouts'].append({'Id':90,'Path':'layout90.xml.zlib'})
    files['Platforms'].append({'Id':90,'Path':'Platforms90.xml.zlib'})
plistlib.dump(info, open('Info.plist','wb'))
pc_path = '../PinConfigs.kext/Contents/Info.plist'
pc = plistlib.load(open(pc_path,'rb'))
entries = pc['IOKitPersonalities']['as.vit9696.AppleALC']['HDAConfigDefault']
src = [e for e in entries if e.get('CodecID')==283902518 and e.get('LayoutID')==15]
assert len(src)==1
if not any(e.get('CodecID')==283902518 and e.get('LayoutID')==90 for e in entries):
    dup = copy.deepcopy(src[0]); dup['LayoutID']=90
    dup['Codec'] = str(dup.get('Codec')) + ' (layout 90, output-only fork)'
    entries.append(dup)
    plistlib.dump(pc, open(pc_path,'wb'))
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
hit = [x for x in e if x.get('CodecID')==283902518 and x.get('LayoutID')==90]
gate("658 pinconfigs", len(e)==658, str(len(e)))
gate("(ALC236,90) present with ConfigData", len(hit)==1 and bool(hit[0].get('ConfigData')))
blob = open(K+'/Contents/MacOS/AppleALC','rb').read()
l90z = open(W+'/AppleALC/Resources/ALC236/layout90.xml.zlib','rb').read()
p90z = open(W+'/AppleALC/Resources/ALC236/Platforms90.xml.zlib','rb').read()
l90r = open(W+'/AppleALC/Resources/ALC236/layout90.xml','rb').read()
gate("layout90 zlib in binary", l90z in blob, f"{len(l90z)}B {l90z[:2].hex()}")
gate("Platforms90 zlib in binary", p90z in blob, f"{len(p90z)}B {p90z[:2].hex()}")
gate("raw XML absent", l90r not in blob)
gate("inflates to LayoutID 90", b'<integer>90</integer>' in zlib.decompress(l90z))
src = open(W+'/AppleALC/AppleALC/kern_resources.cpp').read()
m = re.search(r'DEBUG_STRING\("ALC236"\),\s*0x236,\s*\w+,\s*\d+,\s*(\w+),\s*(\d+),\s*(\w+),\s*(\d+)', src)
gate("18/18 tables", m and m.group(2)=='18' and m.group(4)=='18')
print("ALL GATES PASS" if ok else "GATE FAILURE")
sys.exit(0 if ok else 1)
PYEOF
rm -rf "$OUT/AppleALC.kext"
cp -R "$W/AppleALC/build/Release/AppleALC.kext" "$OUT/AppleALC.kext"
echo "STAGED PERSISTENT: $OUT/AppleALC.kext"
md5 -q "$OUT/AppleALC.kext/Contents/MacOS/AppleALC"
