# SPDX-License-Identifier: MPL-2.0
# Copyright Cuviss Studio. All Rights Reserved.
"""Version 1 packaging configuration. Standard library only; targeted project writes."""
from __future__ import annotations

import copy
import difflib
import hashlib
import json
from pathlib import Path
import re
import tempfile

SCHEMA_VERSION = 1
PROFILE_PATH = Path('Config/SoloASPackagingProfiles.json')
PACKAGING_SECTION = '/Script/UnrealEd.ProjectPackagingSettings'
SOLOAS_SECTION = '/Script/SoloASDeveloperToolsEditor.SoloASPackagingSettings'
DELIVERY_ENUM = {'Source': 'source', 'Bytecode': 'bytecode', 'SemiAot': 'semi_aot'}
BASE_CACHES = ('Binds.Cache', 'Binds.Cache.Headers', 'PrecompiledScript.Cache')
DEFAULTS = {
    'platform': 'Win64', 'target': '', 'configuration': 'Shipping', 'deliveryMode': 'source',
    'maps': [], 'cookDirectories': [], 'extraFiles': [], 'pak': True, 'ioStore': False,
    'outputDirectory': 'Saved/Packages', 'requiredProviders': [],
}


class ConfigError(ValueError):
    def __init__(self, code: str, message: str):
        super().__init__(message)
        self.code = code


def read_json(path: Path):
    try:
        return json.loads(path.read_text(encoding='utf-8-sig'))
    except (OSError, ValueError) as exc:
        raise ConfigError('invalid_json', f'{path.name}: {exc}') from exc


def project_file(value: str | Path) -> Path:
    path = Path(value).resolve()
    if path.is_dir():
        candidates = list(path.glob('*.uproject'))
        if len(candidates) != 1:
            raise ConfigError('invalid_project', 'Project directory must contain exactly one .uproject.')
        path = candidates[0]
    if path.suffix != '.uproject' or not path.is_file():
        raise ConfigError('invalid_project', 'Expected an existing .uproject file.')
    if not isinstance(read_json(path), dict):
        raise ConfigError('invalid_project', '.uproject must be a JSON object.')
    return path


def validate_overlay(value: dict) -> dict:
    if not isinstance(value, dict):
        raise ConfigError('invalid_config', 'Configuration overrides must be an object.')
    for key, item in value.items():
        if key not in DEFAULTS:
            raise ConfigError('unknown_setting', f'Unknown setting: {key}')
        expected = type(DEFAULTS[key])
        if type(item) is not expected:
            raise ConfigError('invalid_config', f'{key} must have type {expected.__name__}.')
        if expected is list and any(not isinstance(entry, str) or not entry for entry in item):
            raise ConfigError('invalid_config', f'{key} must contain nonempty strings.')
        if expected is str and (not item.strip() or '\x00' in item):
            raise ConfigError('invalid_config', f'{key} must be nonempty and contain no NUL.')
    if 'deliveryMode' in value and value['deliveryMode'] not in ('source', 'bytecode', 'semi_aot'):
        raise ConfigError('invalid_config', 'deliveryMode must be source, bytecode or semi_aot.')
    if 'configuration' in value and value['configuration'] not in ('Debug', 'DebugGame', 'Development', 'Test', 'Shipping'):
        raise ConfigError('invalid_config', 'Unknown UE build configuration.')
    if 'platform' in value and value['platform'] not in ('Win64', 'Mac', 'Linux'):
        raise ConfigError('invalid_config', 'platform must be Win64, Mac or Linux.')
    if 'target' in value and not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', value['target']):
        raise ConfigError('invalid_config', 'target must be a UE target identifier.')
    if 'outputDirectory' in value:
        path = value['outputDirectory']
        if path.startswith(('/', '\\')) or re.match(r'^[A-Za-z]:', path) or '\\' in path or '..' in Path(path).parts:
            raise ConfigError('invalid_config', 'outputDirectory must be a project relative POSIX path without traversal.')
    for path in value.get('extraFiles', []):
        # Validate both UE separator forms, but preserve the user's spelling for ini round trips.
        if (not path.strip() or any(char in path for char in ('\x00', '"', '\r', '\n'))
                or path.startswith(('/', '\\')) or re.match(r'^[A-Za-z]:', path)
                or '..' in path.replace('\\', '/').split('/')):
            raise ConfigError('invalid_config', 'extraFiles must contain Content-relative paths without absolute paths or parent traversal.')
    if any(not re.fullmatch(r'[a-z0-9_-]+', entry) or entry == 'soloas' for entry in value.get('requiredProviders', [])):
        raise ConfigError('invalid_config', 'requiredProviders must contain provider IDs.')
    return copy.deepcopy(value)


def profiles(project: Path) -> dict:
    path = project.parent / PROFILE_PATH
    if not path.exists():
        return {}
    value = read_json(path)
    if not isinstance(value, dict) or type(value.get('schemaVersion')) is not int or value.get('schemaVersion') != SCHEMA_VERSION or set(value) != {'schemaVersion', 'profiles'}:
        raise ConfigError('invalid_profiles', 'Expected schemaVersion 1 and profiles.')
    entries = value['profiles']
    if not isinstance(entries, dict):
        raise ConfigError('invalid_profiles', 'profiles must be a name-to-profile object.')
    for name, entry in entries.items():
        if not re.fullmatch(r'[A-Za-z0-9_-]+', name) or not isinstance(entry, dict) or set(entry) - {'description', 'overrides'}:
            raise ConfigError('invalid_profiles', 'Invalid profile name or fields.')
        if not isinstance(entry.get('description', ''), str):
            raise ConfigError('invalid_profiles', 'Profile description must be a string.')
        validate_overlay(entry.get('overrides', {}))
    return entries


def _ini_values(path: Path, values: dict) -> None:
    """Apply known UE scalar and array operators without rewriting the ini."""
    if not path.is_file():
        return
    scalars = {'BuildTarget': 'target', 'BuildConfiguration': 'configuration', 'UsePakFile': 'pak', 'bUseIoStore': 'ioStore'}
    arrays = {'MapsToCook': ('maps', 'FilePath'), 'DirectoriesToAlwaysCook': ('cookDirectories', 'Path'),
              'DirectoriesToAlwaysStageAsNonUFS': ('extraFiles', 'Path')}
    section = ''
    try:
        lines = path.read_text(encoding='utf-8-sig').splitlines()
    except UnicodeError as exc:
        raise ConfigError('invalid_ini', 'Project ini must be UTF-8.') from exc
    for raw in lines:
        line = _split_comment(raw)[0].strip()
        if not line or line.startswith((';', '#')):
            continue
        if line.startswith('[') and line.endswith(']'):
            section = line[1:-1]
            continue
        if section not in (PACKAGING_SECTION, SOLOAS_SECTION) or '=' not in line:
            continue
        key, text = (item.strip() for item in line.split('=', 1))
        operator = key[0] if key[0] in '+-.!' else ''
        key = key[1:] if operator else key
        if section == SOLOAS_SECTION:
            if key == 'DeliveryMode' and not operator:
                mode = text.strip('"').split('::')[-1]
                if mode not in DELIVERY_ENUM:
                    raise ConfigError('invalid_ini', 'Unknown soloAS script delivery mode.')
                values['deliveryMode'] = DELIVERY_ENUM[mode]
            elif key == 'OutputDirectory' and not operator:
                match = re.fullmatch(r'\(Path="([^"\r\n]*)"\)', text)
                if not match:
                    raise ConfigError('invalid_ini', 'Expected OutputDirectory=(Path="project-relative-path").')
                values['outputDirectory'] = match.group(1)
            elif key == 'RequiredProviders':
                entry = text.strip('"')
                if operator == '!':
                    values['requiredProviders'] = []
                elif operator == '-':
                    values['requiredProviders'] = [item for item in values['requiredProviders'] if item != entry]
                elif operator == '.':
                    values['requiredProviders'].append(entry)
                elif operator == '+':
                    if entry not in values['requiredProviders']:
                        values['requiredProviders'].append(entry)
                else:
                    values['requiredProviders'] = [entry]
            continue
        if key in scalars and not operator:
            field = scalars[key]
            if field in ('pak', 'ioStore'):
                if text.lower() not in ('true', 'false'):
                    raise ConfigError('invalid_ini', f'{key} must be True or False.')
                values[field] = text.lower() == 'true'
            else:
                text = text.strip('"')
                if field != 'target' or text:
                    values[field] = text.removeprefix('PPBC_') if field == 'configuration' else text
        elif key in arrays:
            field, member = arrays[key]
            if operator == '!':
                values[field] = []
                continue
            match = re.fullmatch(r'\(' + member + r'="([^"]*)"\)', text)
            if not match:
                raise ConfigError('invalid_ini', f'Unsupported {key} value: {text}')
            entry = match.group(1)
            if operator == '-':
                values[field] = [item for item in values[field] if item != entry]
            elif operator == '.':
                values[field].append(entry)
            elif operator == '+':
                if entry not in values[field]:
                    values[field].append(entry)
            else:
                values[field] = [entry]


def resolve(project: Path, profile: str | None = None, platform: str | None = None, overrides: dict | None = None) -> dict:
    overrides = validate_overlay(overrides if overrides is not None else {})
    values = copy.deepcopy(DEFAULTS)
    values['target'] = project.stem
    sources = {key: 'default' for key in values}
    sources['target'] = 'project_name'
    files = [project.parent / 'Config/DefaultGame.ini']
    if platform:
        validate_overlay({'platform': platform})
        files.append(project.parent / f'Config/{platform}/{platform}Game.ini')
    # A profile may choose the platform whose ini layer needs to be read.
    entries = profiles(project)
    if profile and profile not in entries:
        raise ConfigError('profile_missing', f'Profile not found: {profile}')
    overlay = entries[profile].get('overrides', {}) if profile else {}
    selected_platform = platform or (overrides or {}).get('platform') or overlay.get('platform') or values['platform']
    files = [files[0], project.parent / f'Config/{selected_platform}/{selected_platform}Game.ini']
    for path in files:
        before = copy.deepcopy(values)
        _ini_values(path, values)
        sources.update({key: path.relative_to(project.parent).as_posix() for key in values if values[key] != before[key]})
    project_values = copy.deepcopy(values)
    for label, layer in (('profile', overlay), ('invocation', overrides or {}), ('invocation', {'platform': platform} if platform else {})):
        for key, value in validate_overlay(layer).items():
            values[key] = value
            sources[key] = label
    # Validate the complete effective shape, including values decoded from project ini.
    validate_overlay(values)
    digest = hashlib.sha256(json.dumps(values, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    return {'project': project.name, 'profile': profile, 'projectValues': project_values,
            'profileOverrides': copy.deepcopy(overlay), 'effective': values, 'sources': sources, 'configDigest': digest}


def cache_requirements(capabilities: list[str] | None = None) -> dict:
    capabilities = capabilities or []
    if set(capabilities) - {'core', 'core_extensions'}:
        raise ConfigError('unknown_capability', 'Unknown cache capability.')
    return {'required': list(BASE_CACHES), 'requiredCapabilities': sorted(set(capabilities))}


def companion_names(value) -> list[str]:
    if not isinstance(value, list) or any(not isinstance(name, str)
            or not re.fullmatch(r'[A-Za-z0-9_][A-Za-z0-9_.-]*', name)
            or name in BASE_CACHES for name in value) or len(value) != len(set(value)):
        raise ConfigError('invalid_companion_names', 'Provider companion names must be distinct, safe single file names.')
    return sorted(value)


def check_caches(directory: Path, capabilities: list[str] | None = None) -> list[dict]:
    requirements = cache_requirements(capabilities)
    return [{'code': 'cache_missing_or_empty', 'path': name}
            for name in requirements['required']
            if not (directory / name).is_file() or (directory / name).stat().st_size == 0]


INI_FIELDS = {
    'target': ('BuildTarget', None), 'configuration': ('BuildConfiguration', None),
    'pak': ('UsePakFile', None), 'ioStore': ('bUseIoStore', None),
    'maps': ('MapsToCook', 'FilePath'), 'cookDirectories': ('DirectoriesToAlwaysCook', 'Path'),
    'extraFiles': ('DirectoriesToAlwaysStageAsNonUFS', 'Path'),
}


def _split_comment(line: str) -> tuple[str, str]:
    quoted = False
    for index, char in enumerate(line):
        if char == '"':
            quoted = not quoted
        elif char in ';#' and not quoted:
            return line[:index], line[index:]
    return line, ''


def atomic_write(path: Path, content: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + '.', delete=False) as stream:
        temporary = Path(stream.name)
        stream.write(content)
    try:
        temporary.replace(path)
    finally:
        temporary.unlink(missing_ok=True)


def save_project(project: Path, changes: dict, platform: str | None = None, *, dry_run: bool = False) -> dict:
    changes = validate_overlay(changes)
    fields = {**INI_FIELDS, 'deliveryMode': ('DeliveryMode', None), 'outputDirectory': ('OutputDirectory', 'Path'), 'requiredProviders': ('RequiredProviders', '')}
    if set(changes) - fields.keys():
        raise ConfigError('setting_not_in_ini', 'Platform is an invocation/profile setting; other packaging defaults can be saved to project ini.')
    if platform:
        validate_overlay({'platform': platform})
    path = project.parent / (f'Config/{platform}/{platform}Game.ini' if platform else 'Config/DefaultGame.ini')
    current = copy.deepcopy(DEFAULTS)
    current['target'] = project.stem
    _ini_values(project.parent / 'Config/DefaultGame.ini', current)
    if platform:
        _ini_values(path, current)
    changes = {key: value for key, value in changes.items() if current[key] != value}
    original = path.read_bytes() if path.exists() else b''
    bom = b'\xef\xbb\xbf' if original.startswith(b'\xef\xbb\xbf') else b''
    try:
        text = original[len(bom):].decode('utf-8')
    except UnicodeError as exc:
        raise ConfigError('invalid_ini', 'Project ini must be UTF-8.') from exc
    newline = '\r\n' if '\r\n' in text else '\n'
    keys_by_section = {PACKAGING_SECTION: set(), SOLOAS_SECTION: set()}
    for field in changes:
        keys_by_section[PACKAGING_SECTION if field in INI_FIELDS else SOLOAS_SECTION].add(fields[field][0])
    output, section = [], ''
    for line in text.splitlines(keepends=True):
        body, comment = _split_comment(line.rstrip('\r\n'))
        stripped = body.strip()
        if stripped.startswith('[') and stripped.endswith(']'):
            section = stripped[1:-1]
        if section in keys_by_section and '=' in stripped and stripped.split('=', 1)[0].strip().lstrip('+-.!') in keys_by_section[section]:
            if comment:
                output.append(comment + newline)
        else:
            output.append(line)
    for target_section, keys in keys_by_section.items():
        if not keys:
            continue
        if output and not output[-1].endswith(('\n', '\r')):
            output.append(newline)
        output.append(f'[{target_section}]' + newline)
        for field, value in changes.items():
            if fields[field][0] not in keys:
                continue
            key, member = fields[field]
            if field == 'requiredProviders':
                output.append(f'!{key}=ClearArray' + newline)
                output.extend(f'.{key}={entry}' + newline for entry in value)
            elif member and isinstance(value, list):
                if any(any(char in entry for char in ('"', '\r', '\n')) for entry in value):
                    raise ConfigError('invalid_ini_value', 'Ini array values cannot contain quotes or line breaks.')
                output.append(f'!{key}=ClearArray' + newline)
                output.extend(f'.{key}=({member}="{entry}")' + newline for entry in value)
            elif field == 'outputDirectory':
                if any(char in value for char in ('"', '\r', '\n')):
                    raise ConfigError('invalid_ini_value', 'Ini paths cannot contain quotes or line breaks.')
                output.append(f'{key}=(Path="{value}")' + newline)
            else:
                encoded = ('True' if value else 'False') if type(value) is bool else ('PPBC_' + value if field == 'configuration' else value)
                if field == 'deliveryMode':
                    encoded = next(name for name, mode in DELIVERY_ENUM.items() if mode == value)
                output.append(f'{key}={encoded}' + newline)
    updated = ''.join(output)
    diff = ''.join(difflib.unified_diff(text.splitlines(keepends=True), updated.splitlines(keepends=True), fromfile=path.relative_to(project.parent).as_posix(), tofile=path.relative_to(project.parent).as_posix()))
    if not dry_run and updated != text:
        atomic_write(path, bom + updated.encode('utf-8'))
    return {'path': path.relative_to(project.parent).as_posix(), 'saved': not dry_run, 'changed': updated != text, 'diff': diff}


def save_profile(project: Path, name: str, overrides: dict, description: str = '', *, replace: bool = False) -> dict:
    if not re.fullmatch(r'[A-Za-z0-9_-]+', name) or not isinstance(description, str):
        raise ConfigError('invalid_profiles', 'Invalid profile name or description.')
    overrides = validate_overlay(overrides)
    entries = profiles(project)
    if name in entries and not replace:
        raise ConfigError('profile_exists', 'Use --replace to overwrite an existing profile.')
    entries[name] = {'description': description, 'overrides': overrides}
    atomic_write(project.parent / PROFILE_PATH, (json.dumps({'schemaVersion': SCHEMA_VERSION, 'profiles': entries}, ensure_ascii=False, indent=2) + '\n').encode('utf-8'))
    return {'name': name, **entries[name], 'saved': True, 'path': PROFILE_PATH.as_posix()}


def edit_profile(project: Path, name: str, new_name: str | None = None) -> dict:
    """Rename or delete one profile, preserving every other entry."""
    entries = profiles(project)
    if name not in entries:
        raise ConfigError('profile_missing', f'Profile not found: {name}')
    if new_name is not None:
        if not re.fullmatch(r'[A-Za-z0-9_-]+', new_name):
            raise ConfigError('invalid_profiles', 'Use letters, numbers, underscore or hyphen for the profile name.')
        if new_name in entries:
            raise ConfigError('profile_exists', 'Choose a name that is not already used.')
        entries[new_name] = entries[name]
    del entries[name]
    atomic_write(project.parent / PROFILE_PATH, (json.dumps({'schemaVersion': SCHEMA_VERSION, 'profiles': entries}, ensure_ascii=False, indent=2) + '\n').encode('utf-8'))
    return {'name': new_name or '', 'profiles': [{'name': key, **entries[key]} for key in sorted(entries)]}
