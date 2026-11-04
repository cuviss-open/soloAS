#!/usr/bin/env python3
"""Generate the fixed native SDK companion modules and descriptor entries."""
import argparse
import json
import gzip
import ast
import collections
import hashlib
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PREFIX = 'SAC_'


def outputs():
    manifest = json.loads((ROOT / 'Config/NativeCallerModules.json').read_text())
    assert manifest['schemaVersion'] == 1
    rows = sorted(manifest['modules'], key=lambda row: row['module'])
    policies = manifest.get('sourceBuildPolicies', [])
    skipped = collections.defaultdict(list)
    for policy in policies:
        for row in policy['skippedModules']:
            skipped[row['module']].append(policy['engineMinor'])
    aliases = [row['callerModule'] for row in rows]
    assert len(set(aliases)) == len(aliases) and all(re.fullmatch(r'SAC_[0-9a-f]{8}', name) for name in aliases)
    assert all(row['callerModule'] == PREFIX + hashlib.sha256(row['module'].encode()).hexdigest()[:8] for row in rows)
    result = {}
    for row in rows:
        name = row['callerModule']
                                                                                                                                              
        directory = ROOT / 'Source/SoloASCallersPacks' / name
        body = '{}'
        if row.get('includePaths'):
            body = '{\n' + ''.join(
                '        if (Target.Version.MinorVersion == ' + str(include['engineMinor']) + ')\n'
                '            PrivateIncludePaths.Add(System.IO.Path.Combine(GetModuleDirectory("' + row['module']
                + '"), "' + include['relativePath'] + '"));\n' for include in row['includePaths']) + '    }'
        result[directory / (name + '.Build.cs')] = (
            'using UnrealBuildTool;\n\npublic sealed class ' + name + ' : SoloASNativeCallerRules\n{\n'
            '    public ' + name + '(ReadOnlyTargetRules Target)\n'
            '        : base(Target, "' + row['module'] + '", "' + row['plugin'] + '", "'
            + row['source'] + '", "' + row['type'] + '"'
            + (', ' + ' || '.join('Target.Version.MinorVersion == ' + str(minor) for minor in skipped[row['module']])
               if skipped[row['module']] else '') + ') ' + body + '\n}\n')
        # SOLOAS-COMPAT-END
        if skipped[row['module']] or row.get('includePaths'):
            ident = 'CS.Compatibility.SoloASBridge.' + name + '_Build_cs.CallerAvailability'
            begin = ('    // SOLOAS-' + 'COMPAT-BEGIN(5.4-5.8) SDK choice'
                     ' [test: F01] [id: ' + ident + ']\n')
            end = '    // SOLOAS-' + 'COMPAT-END\n'
            path = directory / (name + '.Build.cs')
            content = result[path]
            constructor = content.index('    public ')
            closing = content.rindex('\n}\n')
            result[path] = content[:constructor] + begin + content[constructor:closing+1] + end + content[closing+1:]
        result[directory / 'Private' / (name + 'Module.cpp')] = (
            '#include "Modules/ModuleManager.h"\n\nvoid ' + name + '_Register();\nvoid ' + name + '_Unregister();\n\n'
            'class F' + name + 'Module : public IModuleInterface\n{\npublic:\n'
            '    void StartupModule() override { ' + name + '_Register(); }\n'
            '    void ShutdownModule() override { ' + name + '_Unregister(); }\n'
            '    bool SupportsDynamicReloading() override { return false; }\n};\n\n'
            'IMPLEMENT_MODULE(F' + name + 'Module, ' + name + ')\n')
        result[directory / 'Private' / (name + 'UhtAnchor.h')] = (
            '#pragma once\n\n#include "CoreMinimal.h"\n#include "' + name + 'UhtAnchor.generated.h"\n\n'
            'UCLASS(meta = (NotInAngelscript))\nclass U' + name + 'UhtAnchor : public UObject\n{\n'
            '    GENERATED_BODY()\n};\n')
    descriptor = ROOT / 'SoloASBridge.uplugin'
    document = json.loads(descriptor.read_text())
    document['Modules'] = [row for row in document['Modules'] if not row['Name'].startswith((PREFIX, 'SoloASCallers_'))]
    document['Modules'] += [dict(Name=row['callerModule'],
        Type='DeveloperTool' if row['type'] == 'Developer' else row['type'], LoadingPhase='None') for row in rows]
    result[descriptor] = json.dumps(document, indent=2) + '\n'
    result[ROOT / 'Source/SoloASBridgeCallers/Private/NativeCallerModules.inl'] = ''.join(
        '    { TEXT("' + row['module'] + '"), TEXT("' + row['plugin'] + '"), TEXT("'
        + row['callerModule'] + '") },\n' for row in rows)
    result[ROOT / 'Config/NativeCallerSkips.json'] = json.dumps(
        dict(schemaVersion=1, versions=policies), indent=2) + '\n'
    return result


def source_build_policy(path, caller_sources=None):
    raw = path.read_bytes()
    census = json.loads(raw)
    modules = {row['module']: row for row in census['modules']}
    names = set(modules)
    dependencies = {name: sorted(set(re.findall(r'"([A-Za-z][A-Za-z0-9_]*)"', row['text'])) & names - {name})
                    for name, row in modules.items()}
    blocked = {name for name, row in modules.items() if row['privateSourcesPresent']
               and re.search(r'bUsePrecompiled\s*=\s*![A-Za-z_][A-Za-z0-9_]*\s*;', row['text'])
               and 'Directory.Exists' in row['text'] and '"Private"' in row['text']}
    sources = {row['module']: row['text'] for row in json.loads(caller_sources.read_text())['modules']} if caller_sources else {}
    skipped = []
    manifest = json.loads((ROOT / 'Config/NativeCallerModules.json').read_text())
    for row in sorted(manifest['modules'], key=lambda row: row['module']):
        name = row['module']
        pending = collections.deque([(name, [name])]); seen = set(); paths = []
        while pending:
            current, chain = pending.popleft()
            if current in seen: continue
            seen.add(current)
            if current in blocked: paths.append(chain)
            pending.extend((child, chain + [child]) for child in dependencies.get(current, []))
        if not paths: continue
        text = sources.get(name, '')
        pattern = r'\{\s*("(?:\\.|[^"\\])*")\s*,\s*("(?:\\.|[^"\\])*")\s*,\s*static_cast<ESoloASBridgeFunctionTrait>\(\d+U\),\s*(?:#if\s+\w+\s*)?SoloASNativeReference_'
        functions = [dict(owner=ast.literal_eval(owner), function=ast.literal_eval(function))
                     for owner, function in re.findall(pattern, text)]
        skipped.append(dict(module=name, dependencyPaths=paths, functions=functions,
                            callerSourceSha256=hashlib.sha256(text.encode()).hexdigest()))
    return dict(engineMinor=census['engineMinor'], rulesSha256=hashlib.sha256(raw).hexdigest(),
                sourceBuildModules=[dict(module=name, path=modules[name]['path'],
                    sha256=hashlib.sha256(modules[name]['text'].encode()).hexdigest()) for name in sorted(blocked)],
                skippedModules=skipped)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    parser.add_argument('--metadata', type=Path, help='UHT SoloASBridgeNativeMetadata.json output for a release SDK')
    parser.add_argument('--engine-minor', type=int, choices=range(4, 9))
    parser.add_argument('--engine-rules', type=Path, help='Frozen installed-engine Build.cs census')
    parser.add_argument('--caller-sources', type=Path, help='Frozen UHT companion sources for affected-function reporting')
    args = parser.parse_args()
    if args.engine_rules:
        policy = source_build_policy(args.engine_rules, args.caller_sources)
        path = ROOT / 'Config/NativeCallerModules.json'
        manifest = json.loads(path.read_text())
        policies = manifest.get('sourceBuildPolicies', [])
        old = next((row for row in policies if row['engineMinor'] == policy['engineMinor']), None)
        if args.check:
            if old != policy:
                parser.error('frozen source-build policy differs')
        else:
            manifest['sourceBuildPolicies'] = sorted(
                [row for row in policies if row['engineMinor'] != policy['engineMinor']] + [policy],
                key=lambda row: row['engineMinor'])
            path.write_text(json.dumps(manifest, indent=2) + '\n')
    expected = outputs()
    if args.metadata:
        if args.engine_minor is None:
            parser.error('--metadata requires --engine-minor')
        raw = args.metadata.read_bytes()
        document = json.loads(raw)
        if not all(isinstance(document.get(key), list) for key in
                ('functions', 'defaultArguments', 'propertyTraits', 'functionTraits')):
            parser.error('native metadata arrays are missing')
        path = ROOT / 'Config/NativeMetadata' / ('UE5' + str(args.engine_minor) + '.json.gz')
        compressed = gzip.compress(raw, compresslevel=9, mtime=0)
        if args.check:
            if not path.exists() or gzip.decompress(path.read_bytes()) != raw:
                print(str(path.relative_to(ROOT)) + ': native metadata differs')
                raise SystemExit(1)
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(compressed)
    failures = [str(path.relative_to(ROOT)) + ': companion module must be under Source/SoloASCallersPacks'
        for path in sorted((ROOT / 'Source').glob(PREFIX + '*')) if path.is_dir()]
    actual = {path for path in (ROOT / 'Source/SoloASCallersPacks').rglob('*') if path.is_file()}
    failures += [str(path.relative_to(ROOT)) + ': unexpected generated file' for path in sorted(actual - expected.keys())]
    for path, text in expected.items():
        if path.exists() and path.read_text() == text:
            continue
        if args.check:
            failures.append(str(path.relative_to(ROOT)) + ': generation differs')
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text)
    for failure in failures:
        print(failure)
    if failures:
        raise SystemExit(1)
    print(('Checked' if args.check else 'Generated') + ' ' + str(len(expected)) + ' files')


if __name__ == '__main__':
    main()
