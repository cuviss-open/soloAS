# SPDX-License-Identifier: MPL-2.0
# Copyright Cuviss Studio. All Rights Reserved.
"""Unmodified static-JIT collection and reversible release-copy integration."""
from __future__ import annotations

import hashlib
import json
from pathlib import Path
import re
import shutil

import config

MODULE = 'SoloASJitConsumer'
TEMPLATES = Path(__file__).parent / 'Templates'


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            digest.update(block)
    return digest.hexdigest()


def inventory(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): {'sha256': sha256(p), 'bytes': p.stat().st_size}
            for p in sorted(root.rglob('*')) if p.is_file()}


class Transaction:
    """Restore only bytes still owned by this job, including after executor restart."""
    def __init__(self, root: Path, directory: Path):
        self.root, self.directory = root.resolve(), directory
        directory.mkdir(parents=True, exist_ok=True)
        self.path = directory / 'transaction.json'
        self.entries = config.read_json(self.path)['files'] if self.path.exists() else []

    def save(self):
        temporary = self.path.with_suffix('.tmp')
        temporary.write_text(json.dumps({'schemaVersion': 1, 'files': self.entries}, indent=2) + '\n', encoding='utf-8')
        temporary.replace(self.path)

    def write(self, path: Path, content: bytes):
        relative = path.resolve().relative_to(self.root).as_posix()
        if any(e['path'] == relative for e in self.entries):
            raise config.ConfigError('transaction_duplicate', f'Already changed: {relative}')
        before = sha256(path) if path.exists() else None
        backup = f'{len(self.entries)}.original' if before else None
        if backup:
            shutil.copyfile(path, self.directory / backup)
        directories = []
        parent = path.parent
        while parent != self.root and not parent.exists():
            directories.append(parent.relative_to(self.root).as_posix())
            parent = parent.parent
        entry = {'path': relative, 'before': before, 'after': hashlib.sha256(content).hexdigest(),
                 'backup': backup, 'createdDirectories': directories}
        self.entries.append(entry)
        self.save()  # Intent and backup are durable before the target changes.
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)

    def restore(self) -> list[str]:
        conflicts = []
        for entry in reversed(self.entries):
            path = self.root / entry['path']
            actual = sha256(path) if path.is_file() else None
            if actual == entry['before']:
                continue
            if actual != entry['after']:
                conflicts.append(entry['path'])
                continue
            if entry['backup']:
                path.write_bytes((self.directory / entry['backup']).read_bytes())
            else:
                path.unlink()
        for entry in reversed(self.entries):
            for relative in entry.get('createdDirectories', []):
                directory = self.root / relative
                if directory.is_dir() and not any(directory.iterdir()):
                    directory.rmdir()
        return conflicts


def collect(generated: Path, caches: Path, frozen: Path, capabilities=None, companions=None) -> dict:
    errors = config.check_caches(caches, capabilities)
    if errors:
        raise config.ConfigError('invalid_cache_set', json.dumps(errors))
    companions = config.companion_names(companions or [])
    for name in companions:
        path = caches / name
        if path.is_symlink() or not path.is_file() or not path.stat().st_size:
            raise config.ConfigError('companion_missing_or_empty', 'Required companion missing or empty: ' + name)
    if not generated.is_dir() or not (generated / 'AngelscriptJitInfo.jit.cpp').is_file():
        raise config.ConfigError('jit_output_missing', 'This producer did not write AS_JITTED_CODE/AngelscriptJitInfo.jit.cpp.')
    files = sorted(generated.rglob('*'))
    if any(p.is_symlink() or (p.is_file() and p.suffix not in ('.h', '.hpp', '.cpp')) for p in files):
        raise config.ConfigError('jit_output_unknown', 'Generated directory contains unsupported files or links.')
    if not any(p.name != 'AngelscriptJitInfo.jit.cpp' and p.suffix == '.cpp' for p in files):
        raise config.ConfigError('jit_output_empty', 'No generated script compilation unit.')
    names = list(config.BASE_CACHES) + companions
    unknown = [p.name for p in caches.iterdir() if p.is_file() and ('cache' in p.name.lower()) and p.name not in names]
    if unknown:
        raise config.ConfigError('cache_output_contract_required', 'Unowned generated cache outputs: ' + ', '.join(unknown))
    shutil.copytree(generated, frozen / 'AS_JITTED_CODE')
    (frozen / 'Script').mkdir()
    for name in names:
        shutil.copyfile(caches / name, frozen / 'Script' / name)
    result = inventory(frozen)
    (frozen / 'artifacts.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def verify_collected(frozen: Path) -> None:
    expected = config.read_json(frozen / 'artifacts.json')
    actual = inventory(frozen)
    actual.pop('artifacts.json', None)
    if actual != expected:
        raise config.ConfigError('generated_artifacts_changed', 'Frozen generation is missing or has changed.')


def dependencies(generated: Path, engine: Path, project: Path) -> list[str]:
    """Resolve actual native include owners; never add example GAS/EI unconditionally."""
    modules = {'Core', 'AngelscriptCode'}
    public_roots = None
    for path in generated.rglob('*'):
        if not path.is_file():
            continue
        for include in re.findall(r'^\s*#include\s+"([^"\r\n]+)"', path.read_text(encoding='utf-8'), re.M):
            local = generated / include
            if (not Path(include).is_absolute() and local.is_file() and
                    local.resolve().is_relative_to(generated.resolve())) or include.startswith('StaticJIT/'):
                continue
            engine_prefix = (engine.resolve() / 'Engine').as_posix().rstrip('/') + '/'
            normalized_include = include.replace('\\', '/')
            if normalized_include.startswith('../../../Engine/'):
                normalized_include = (engine / 'Engine' / normalized_include[len('../../../Engine/'):]).as_posix()
            if normalized_include.casefold().startswith(engine_prefix.casefold()):
                header = engine / 'Engine' / normalized_include[len(engine_prefix):]
            else:
                header = Path(include)
                if not header.is_absolute():
                    if public_roots is None:
                        module_roots = (project / 'Plugins', engine / 'Engine/Source',
                                        engine / 'Engine/Plugins')
                        public_roots = [rule.parent / directory
                                        for root in module_roots
                                        for rule in root.rglob('*.Build.cs')
                                        for directory in ('Public', 'Classes')]
                    matches = {(root / include).resolve() for root in public_roots
                               if (root / include).is_file()}
                    if len(matches) != 1:
                        raise config.ConfigError('jit_dependency_unresolved', f'Cannot resolve generated include: {include}')
                    header = next(iter(matches))
            if not header.is_file():
                raise config.ConfigError('jit_dependency_unresolved', f'Generated include does not exist: {include}')
            owners = []
            for parent in header.parents:
                if parent == engine.parent or parent == project.parent:
                    break
                rules = list(parent.glob('*.Build.cs'))
                if rules:
                    owners = [p.name.removesuffix('.Build.cs') for p in rules]
                    break
            if len(owners) != 1:
                raise config.ConfigError('jit_dependency_unresolved', f'Cannot identify one module owner: {include}')
            modules.add(owners[0])
    return sorted(modules)


def sync(project: Path, target: str, generated: Path, engine: Path, transaction: Transaction) -> dict:
    root = project.parent
    destination = root / 'Source' / MODULE
    if destination.exists():
        raise config.ConfigError('jit_module_conflict', f'{MODULE} already exists in this release copy.')
    descriptor = config.read_json(project)
    modules = descriptor.setdefault('Modules', [])
    if any(item.get('Name') == MODULE for item in modules):
        raise config.ConfigError('jit_module_conflict', f'{MODULE} is already declared.')
    deps = dependencies(generated, engine, root)
    build = (TEMPLATES / 'SoloASJitConsumer.Build.cs.in').read_text(encoding='utf-8')
    build = build.replace('@DEPENDENCIES@', ', '.join(json.dumps(m) for m in deps))
    transaction.write(destination / f'{MODULE}.Build.cs', build.encode())
    transaction.write(destination / f'{MODULE}.cpp', (TEMPLATES / 'SoloASJitConsumer.cpp').read_bytes())
    for source in sorted(generated.rglob('*')):
        if source.is_file():
            transaction.write(destination / 'AS_JITTED_CODE' / source.relative_to(generated), source.read_bytes())
    modules.append({'Name': MODULE, 'Type': 'Runtime', 'LoadingPhase': 'Default', 'TargetAllowList': ['Game']})
    transaction.write(project, (json.dumps(descriptor, indent=2) + '\n').encode())
    target_file = root / 'Source' / f'{target}.Target.cs'
    if target_file.is_file():
        original = target_file.read_text(encoding='utf-8-sig')
        assignments = list(re.finditer(r'\bType\s*=\s*TargetType\.Game\s*;', original))
        # Complex/custom rules need an explicit patch reviewed by the project owner.
        if len(assignments) != 1 or re.search(r'\b(if|switch|for|foreach|while)\s*\(', original):
            raise config.ConfigError('target_edit_required', f'Add ExtraModuleNames.Add("{MODULE}") to {target_file.name}; custom target was not edited.')
        position = assignments[0].end()
        updated = original[:position] + f'\n        ExtraModuleNames.Add("{MODULE}");' + original[position:]
        transaction.write(target_file, updated.encode())
    elif not list((root / 'Source').glob('*.Target.cs')):
        for name, kind in ((target, 'Game'), (target + 'Editor', 'Editor')):
            content = (TEMPLATES / 'Consumer.Target.cs.in').read_text(encoding='utf-8')
            content = content.replace('@TARGET@', name).replace('@KIND@', kind)
            content = content.replace('@MODULES@', f'ExtraModuleNames.Add("{MODULE}");' if kind == 'Game' else '')
            transaction.write(root / 'Source' / f'{name}.Target.cs', content.encode())
    else:
        raise config.ConfigError('target_edit_required', f'No editable Game target: {target_file.name}')
    return {'module': MODULE, 'dependencies': deps, 'generated': inventory(destination / 'AS_JITTED_CODE')}


def _field_text(action: dict, field: str) -> list[str]:
    value = action.get(field)
    if value is None:
        return []
    if isinstance(value, (list, tuple)):
        return [str(item) for item in value]
    return [str(value)]


def _response_file_contents(action: dict, graph: Path) -> tuple[bool, list[str]]:
    contents = _field_text(action, 'ResponseFileContents')
    available = action.get('ResponseFileContents') is not None
    for command in _field_text(action, 'CommandArguments'):
        for match in re.finditer(r'@(?:"([^"]+)"|\'([^\']+)\'|([^\s]+))', command):
            response = next(value for value in match.groups() if value is not None)
            path = Path(response)
            candidates = [path]
            if not path.is_absolute():
                working_directory = action.get('WorkingDirectory')
                if working_directory:
                    candidates.insert(0, Path(working_directory) / path)
                candidates.append(graph.parent / path)
            for candidate in dict.fromkeys(candidates):
                if candidate.is_file():
                    contents.append(candidate.read_text(encoding='utf-8', errors='replace'))
                    available = True
                    break
    return available, contents


def _action_text(action: dict, graph: Path) -> str:
    _, response_contents = _response_file_contents(action, graph)
    fields = ('StatusDescription', 'CommandArguments', 'ProducedItems')
    return '\n'.join([part for field in fields for part in _field_text(action, field)] + response_contents)


def compile_actions(graph: Path, generated: Path, executable: Path) -> list[dict]:
    actions = config.read_json(graph).get('Actions', [])
    normalize = lambda value: str(value).replace('\\', '/').lower()
    links = [a for a in actions if a.get('Type') == 'Link' and
             normalize(executable) in [normalize(p) for p in a.get('ProducedItems', [])]]
    if len(links) != 1:
        raise config.ConfigError('jit_link_missing', 'Expected one current link action for the final Game executable.')
    link = links[0]
    by_id = {a['Id']: a for a in actions}
    dependencies = set()
    pending = list(link.get('PrerequisiteActions', []))
    while pending:
        action_id = pending.pop()
        if action_id not in dependencies:
            if action_id not in by_id:
                raise config.ConfigError('jit_link_missing', 'The link graph references an absent action.')
            dependencies.add(action_id)
            pending.extend(by_id[action_id].get('PrerequisiteActions', []))
    has_link_inputs, link_response_contents = _response_file_contents(link, graph)
    link_inputs = normalize('\n'.join(link_response_contents))
    compile_actions = [(action, _action_text(action, graph)) for action in actions if action.get('Type') == 'Compile']
    result = []
    for source in sorted(generated.rglob('*.cpp')):
        normalized = str(source.resolve()).replace('\\', '/').lower()
        basename = re.escape(source.name.lower())
        source_name = re.compile(r'(?<![a-z0-9_])' + basename + r'(?![a-z0-9_])')
        matches = [action for action, text in compile_actions
                   if normalized in normalize(text) or source_name.search(normalize(text))]
        if len(matches) != 1:
            raise config.ConfigError('jit_compile_missing', f'Expected one current compilation action: {source.name}')
        objects = [Path(p) for p in matches[0].get('ProducedItems', []) if p.lower().endswith(('.obj', '.o'))]
        if len(objects) != 1:
            raise config.ConfigError('jit_object_missing', f'No unique compilation object: {source.name}')
        if matches[0]['Id'] not in dependencies or (has_link_inputs and normalize(objects[0]) not in link_inputs):
            raise config.ConfigError('jit_not_linked', f'Generated object is not an input to the final executable link: {source.name}')
        result.append({'source': source.name, 'sourceSha256': sha256(source), 'object': str(objects[0])})
    if not result:
        raise config.ConfigError('jit_compile_missing', 'No generated C++ compilation actions.')
    return result


def verify_objects(actions: list[dict]) -> list[dict]:
    result = []
    for action in actions:
        path = Path(action['object'])
        if not path.is_file() or path.stat().st_size == 0:
            raise config.ConfigError('jit_object_missing', 'Build did not produce: ' + action['source'])
        result.append({'source': action['source'], 'sourceSha256': action['sourceSha256'],
                       'objectSha256': sha256(path)})
    return result
