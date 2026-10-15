# SPDX-License-Identifier: MIT
# Copyright (c) 2026 the MYIOSDECK author
"""Fail the integration build if a replacement is omitted or a legacy source returns."""
from pathlib import Path
import subprocess, json, hashlib
root=Path(__file__).resolve().parents[1]
project=root/'MYIOSDECK.xcodeproj/project.pbxproj'
project_json=subprocess.check_output(['plutil','-convert','json','-o','-',str(project)])
objects=json.loads(project_json)['objects']
targets=[o for o in objects.values() if o.get('isa')=='PBXNativeTarget' and o.get('name')=='MYIOSDECK']
assert len(targets)==1
phases=[objects[key] for key in targets[0]['buildPhases'] if objects[key].get('isa')=='PBXSourcesBuildPhase']
refs=[objects[objects[key]['fileRef']] for phase in phases for key in phase['files']]
names=[Path(ref['path']).name for ref in refs]
expected=[]
for spec in ['01-input','02-wine-bridge','03-steam-client','04-steam-client-host']:
    expected += [p.name for p in (root/'replacements/track-b'/spec).iterdir() if p.suffix in ['.swift','.c','.m']]
for name in expected: assert names.count(name)==1, f'Replacement membership mismatch: {name}'
for name in ['WineProcessBridge.m','WineServerBridge.m','IOSDisplayShim.m','PadOutput.m','PrefixExtractor.c','wine_stubs.c','Winios.m','WiniosGamepad.c','WiniosCursor.c','wine_host.c','lzma_shim.c','zstd_edu.c','zstd_decompress.c','SteamRuntime.swift','MadeiraDock.swift','DockCMCache.swift','SteamCMList.swift','SteamHarness.swift','HostHarness.swift','test_logic.c','test_stores.c']:
    assert name not in names, f'Excluded source was compiled: {name}'
assert all(not name.endswith('Tests.swift') for name in names)
baseline=json.loads((root/'replacements/track-b/PACKAGE-SHA256.json').read_text())
modified=[]
for name, sha in baseline.items():
    path=root/'replacements/track-b'/name
    current=hashlib.sha256(path.read_bytes()).hexdigest()
    if current!=sha: modified.append(name)
for name in ['03-steam-client/vendor/zstd_decompress.c','03-steam-client/vendor/zstd_decompress.h','03-steam-client/vendor/LICENSE','02-wine-bridge/COPYING.LIB']:
    assert name not in modified, f'Vendor/license changed: {name}'
report={'track':'B','packageCommit':'1ed04c742ad1a1f22750697ad8465aa318a64b6d','packageSHA256':'1c05b0eaf64a283746fe7df4cfbb0424cf60b5970cb546ff817f8d7f3002d936','integrationCommit':subprocess.check_output(['git','rev-parse','HEAD'],cwd=root,text=True).strip(),'compiledReplacementFiles':sorted(expected),'compiledReplacementCount':len(expected),'modifiedPackageFiles':modified,'standaloneTestsExcluded':True,'vendorDecoderCompiledOnlyThroughWrapper':True}
(root/'build').mkdir(exist_ok=True)
(root/'build/track-b-membership.json').write_text(json.dumps(report,indent=2)+'\n')
print(f'Track B: all {len(expected)} replacement compilation units included once; excluded sources absent.')
