#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright Cuviss Studio. All Rights Reserved.
"""SoloAS packaging CLI contract. Real packaging execution belongs to packaging.py."""
from __future__ import annotations

import argparse
import codecs
import importlib.util
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import sys
import subprocess
import uuid

import config

STATES = {
    'queued': {'prechecking', 'cancelled'},
    'prechecking': {'running', 'failed', 'cancelled'},
    'running': {'succeeded', 'failed', 'cancelling'},
    'cancelling': {'cancelled', 'failed'},
    'succeeded': set(), 'failed': set(), 'cancelled': set(),
}
CONTRACT_VERSION = 3
TERMINAL_STATES = ('succeeded', 'failed', 'cancelled')
STEP_COMMANDS = ('clean-cache', 'generate-precompiled', 'sync-jit')

PHASES = ('precheck', 'prepare', 'generate', 'collect', 'jit_sync', 'build', 'stage', 'package_check')


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise config.ConfigError('invalid_arguments', message)


def timestamp():
    return datetime.now(timezone.utc).isoformat()


def job_directory(project: Path, job_id: str) -> Path:
    if not re.fullmatch(r'[0-9a-f]{32}', job_id):
        raise config.ConfigError('invalid_job_id', 'Job IDs contain 32 lowercase hexadecimal characters.')
    return project.parent / 'Saved/SoloASDeveloperTools/Packaging' / job_id


def write_job(directory: Path, job: dict):
    # Only one executor owns a job; readers see either the previous or complete next state.
    temporary = directory / 'job.json.tmp'
    temporary.write_text(json.dumps(job, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    temporary.replace(directory / 'job.json')


def transition(directory: Path, job: dict, state: str, *, phase: str | None = None, error: dict | None = None):
    if phase is not None and phase not in PHASES:
        raise config.ConfigError('invalid_phase', f'Unknown phase: {phase}')
    if state not in STATES[job['state']] and not (state == job['state'] == 'running' and phase is not None):
        raise config.ConfigError('invalid_transition', f"Cannot change {job['state']} to {state}.")
    job.update(state=state, phase=phase, error=error, updatedAt=timestamp())
    job['history'].append({'state': state, 'phase': phase, 'at': job['updatedAt']})
    write_job(directory, job)


def precheck(project: Path, resolved: dict, editor: str | None = None) -> dict:
    errors = []
    for provider in resolved['effective']['requiredProviders']:
        errors.append({'code': 'provider_discovery_required', 'providerId': provider,
                       'message': 'An Editor commandlet must resolve this required provider before execution.'})
    if toolchain_module_available():
        toolchain = probe_toolchain(project, editor) if editor else None
    else:
        toolchain = {'status': 'unavailable', 'reason': 'module-missing'}
    toolchain_probed = toolchain is not None and 'selection' in toolchain
    if toolchain_probed and toolchain['selection'] == 'missing':
        errors.append({'code': 'toolchain_unavailable', 'message': 'No compatible Win64 toolchain was selected.'})
    return {'toolchain': toolchain, 'scope': 'configuration', 'configurationValid': not errors, 'readyToPackage': False,
            'checks': [{'name': 'project', 'state': 'passed'}, {'name': 'configuration', 'state': 'passed'}],
            'pendingChecks': ([] if toolchain_probed else ['toolchain']) + ['target', 'disk_space', 'providers', 'native_inputs'],
            'errors': errors, 'config': resolved, 'cacheRequirements': config.cache_requirements()}


def toolchain_module_available() -> bool:
    descriptor = config.read_json(Path(__file__).resolve().parents[2] / 'SoloASDeveloperTools.uplugin')
    return any(module.get('Name') == 'SoloASDeveloperToolsToolchainEditor' for module in descriptor.get('Modules', []))


def probe_toolchain(project: Path, editor: str) -> dict:
    executable = Path(editor)
    if not executable.is_absolute() or not executable.is_file():
        raise config.ConfigError('invalid_editor', '--editor must name an existing absolute Editor executable.')
    directory = project.parent / 'Saved/SoloASDeveloperTools/Logs' / uuid.uuid4().hex
    directory.mkdir(parents=True)
    response = directory / 'response.json'
    with (directory / 'probe.log').open('wb') as log:
        try:
            process = subprocess.run([str(executable), str(project), '-run=SoloASToolchain',
                                      '-ResponseFile=' + str(response), '-unattended', '-nocrashreports',
                                      '-NoMessaging', '-NoASDebugServer', '-NullRHI'],
                                     stdout=log, stderr=subprocess.STDOUT, timeout=420)
        except subprocess.TimeoutExpired as exc:
            raise config.ConfigError('toolchain_timeout', 'Read-only toolchain probe exceeded 420 seconds.') from exc
    if process.returncode not in (0, 3):
        raise config.ConfigError('toolchain_probe_failed', f'Toolchain probe exit {process.returncode}; expected 0 (ready), 3 (unavailable) or 2 (input/IO error).')
    value = config.read_json(response)
    if not isinstance(value, dict) or type(value.get('schemaVersion')) is not int or value['schemaVersion'] != 1 or value.get('platform') != 'Win64' or value.get('preference') not in ('Auto', 'ForceOwn', 'ForceManaged') or value.get('selection') not in ('own', 'managed', 'missing') or type(value.get('restartRequiredForSettings')) is not bool:
        raise config.ConfigError('invalid_toolchain_response', 'Expected Win64 toolchain schemaVersion=1, preference, selection and restartRequiredForSettings.')
    if (value['selection'] != 'missing') != (process.returncode == 0):
        raise config.ConfigError('invalid_toolchain_response', 'Toolchain selection disagrees with process exit code.')
    return value


def backend_call(name: str, *args):
    # Load only our sibling backend, never the unrelated PyPI "packaging" module.
    path = Path(__file__).with_name('packaging.py')
    if not path.is_file():
        raise config.ConfigError('backend_not_implemented', 'Packaging backend packaging.py is not installed; install the C-line executor.')
    spec = importlib.util.spec_from_file_location('soloas_packaging_backend', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    try:
        spec.loader.exec_module(module)
    except ImportError as exc:
        raise config.ConfigError('backend_import_failed', f'Cannot import packaging.py: {exc}') from exc
    function = getattr(module, name, None)
    if not callable(function):
        raise config.ConfigError('backend_interface_missing', f'packaging.py must export {name}().')
    return function(*args)


def create_job(project: Path, resolved: dict) -> tuple[Path, dict]:
    job_id = resolved.get('jobId') or uuid.uuid4().hex
    directory = job_directory(project, job_id)
    try:
        directory.mkdir(parents=True, exist_ok=False)
    except FileExistsError as exc:
        raise config.ConfigError('job_exists', 'Job ID already exists; use a new ID.') from exc
    config.atomic_write(directory / 'input.json', (json.dumps(resolved, ensure_ascii=False, indent=2) + '\n').encode('utf-8'))
    (directory / 'job.log').touch()
    now = timestamp()
    job = {'schemaVersion': 1, 'contractVersion': CONTRACT_VERSION, 'jobId': job_id, 'state': 'queued', 'phase': None,
           'createdAt': now, 'updatedAt': now, 'configDigest': resolved['configDigest'],
           'profile': resolved['profile'], 'input': 'input.json', 'log': 'job.log', 'result': None, 'error': None,
           'history': [{'state': 'queued', 'phase': None, 'at': now}]}
    write_job(directory, job)
    return directory, job


def run_job(project: Path, resolved: dict) -> dict:
    try:
        return backend_call('run_job', project, resolved)
    except config.ConfigError as exc:
        if exc.code not in ('backend_not_implemented', 'backend_import_failed', 'backend_interface_missing'):
            raise
        directory, job = create_job(project, resolved)
        transition(directory, job, 'prechecking', phase='precheck')
        checked = precheck(project, resolved)
        error = checked['errors'][0] if checked['errors'] else {'code': exc.code, 'message': str(exc)}
        transition(directory, job, 'failed', phase='precheck', error=error)
        return job


def cancellation_requested(directory: Path) -> bool:
    return (directory / 'cancel.request.json').is_file()


def read_log(directory: Path, offset: int, limit: int) -> dict:
    if offset < 0 or not 4 <= limit <= 1048576:
        raise config.ConfigError('invalid_log_range', 'offset must be nonnegative and limit must be 4..1048576 bytes.')
    path = directory / 'job.log'
    if not path.exists():
        return {'text': '', 'nextOffset': offset, 'eof': True}
    with path.open('rb') as stream:
        size = stream.seek(0, 2)
        if offset > size:
            raise config.ConfigError('invalid_log_range', 'offset exceeds log size.')
        stream.seek(offset)
        block = stream.read(limit)
    decoder = codecs.getincrementaldecoder('utf-8')(errors='replace')
    text = decoder.decode(block, final=(offset + len(block) == size))
    pending = decoder.getstate()[0]
    consumed = len(block) - len(pending)
    return {'text': text, 'nextOffset': offset + consumed, 'eof': offset + consumed == size}


def read_job(project: Path, job_id: str) -> tuple[Path, dict]:
    directory = job_directory(project, job_id)
    if not (directory / 'job.json').is_file():
        raise config.ConfigError('job_missing', 'Job was not found in this project.')
    job = config.read_json(directory / 'job.json')
    if not isinstance(job, dict) or job.get('schemaVersion') != 1 or job.get('jobId') != job_id or not isinstance(job.get('state'), str) or job.get('state') not in STATES:
        raise config.ConfigError('invalid_job', 'Invalid job state file.')
    return directory, job


def arguments(argv):
    common = Parser(add_help=False)
    common.add_argument('--project', help='Project .uproject or directory')
    common.add_argument('--json', action='store_true', help='Emit a single versioned JSON object')
    shared, rest = common.parse_known_args(argv)
    parser = Parser(description=__doc__, epilog='All commands require --project; --json works at any position.')
    commands = parser.add_subparsers(dest='command', required=True)
    profiles = commands.add_parser('profiles').add_subparsers(dest='action', required=True)
    profiles.add_parser('list')
    save = profiles.add_parser('save')
    save.add_argument('--name', required=True)
    save.add_argument('--description', default='')
    save.add_argument('--overrides', default='{}')
    save.add_argument('--replace', action='store_true')
    for action in ('rename', 'delete'):
        edit = profiles.add_parser(action)
        edit.add_argument('--name', required=True)
        if action == 'rename':
            edit.add_argument('--new-name', required=True)
    configs = commands.add_parser('config').add_subparsers(dest='action', required=True)
    read = configs.add_parser('read')
    write = configs.add_parser('write')
    write.add_argument('--platform', choices=('Win64', 'Mac', 'Linux'))
    write.add_argument('--overrides', required=True)
    write.add_argument('--dry-run', action='store_true')
    check = commands.add_parser('precheck')
    check.add_argument('--editor', help='Absolute Editor executable for the read-only toolchain probe')
    run = commands.add_parser('run', help='Run synchronously; use a caller-chosen ID for background polling')
    steps = [commands.add_parser(name) for name in STEP_COMMANDS]
    for item in (run, *steps):
        item.add_argument('--job-id')
    for item in (read, check, run, *steps):
        item.add_argument('--profile')
        item.add_argument('--platform', choices=('Win64', 'Mac', 'Linux'))
        item.add_argument('--overrides', default='{}', help='JSON object of invocation overrides')
    package = commands.add_parser('check-package')
    package.add_argument('--path', required=True)
    package.add_argument('--mode', choices=('auto', 'source', 'bytecode', 'semi_aot'), default='auto')
    for command in ('status', 'cancel', 'logs'):
        item = commands.add_parser(command)
        item.add_argument('--job', required=True)
        if command == 'logs':
            item.add_argument('--offset', type=int, default=0)
            item.add_argument('--limit', type=int, default=65536)
    args = parser.parse_args(rest)
    if not shared.project:
        raise config.ConfigError('invalid_arguments', '--project is required.')
    return shared, args


def decode_overrides(text):
    try:
        return config.validate_overlay(json.loads(text))
    except ValueError as exc:
        if isinstance(exc, config.ConfigError):
            raise
        raise config.ConfigError('invalid_config', '--overrides must be a JSON object.') from exc


def dispatch(shared, args) -> tuple[dict, int]:
    project = config.project_file(shared.project)
    if args.command == 'profiles':
        if args.action == 'save':
            return config.save_profile(project, args.name, decode_overrides(args.overrides), args.description, replace=args.replace), 0
        if args.action in ('rename', 'delete'):
            return config.edit_profile(project, args.name, args.new_name if args.action == 'rename' else None), 0
        entries = config.profiles(project)
        return {'profiles': [{'name': name, **entries[name]} for name in sorted(entries)]}, 0
    if args.command in ('status', 'cancel', 'logs'):
        directory, job = read_job(project, args.job)
        if args.command == 'logs':
            return read_log(directory, args.offset, args.limit), 0
        requested = False
        if args.command == 'cancel' and job['state'] not in TERMINAL_STATES:
            request = directory / 'cancel.request.json'
            if not request.exists():
                config.atomic_write(request, (json.dumps({'schemaVersion': 1, 'jobId': args.job, 'requestedAt': timestamp()}) + '\n').encode('utf-8'))
            requested = True
        return {'job': job, 'cancelRequested': requested or cancellation_requested(directory)}, 0
    if args.command == 'check-package':
        path = Path(args.path).resolve()
        if not path.is_dir():
            raise config.ConfigError('invalid_package', '--path must name an existing package directory.')
        data = backend_call('check_package', project, path, args.mode)
        return data, 0 if data['ok'] else 3
    overrides = decode_overrides(args.overrides)
    if args.command == 'config' and args.action == 'write':
        return config.save_project(project, overrides, args.platform, dry_run=args.dry_run), 0
    resolved = config.resolve(project, args.profile, args.platform, overrides)
    if args.command == 'config':
        return resolved, 0
    if args.command == 'precheck':
        checked = precheck(project, resolved, args.editor)
        return checked, 0 if checked['configurationValid'] else 3
    if args.job_id:
        job_directory(project, args.job_id)
        resolved['jobId'] = args.job_id
    if args.command in STEP_COMMANDS:
        data = backend_call(args.command.replace('-', '_'), project, resolved)
        return data, 0 if data['ok'] else 3
    job = run_job(project, resolved)
    if job['state'] == 'succeeded':
        return {'job': job}, 0
    code = (job.get('error') or {}).get('code', '')
    return {'job': job}, 4 if code.startswith('backend_') else 3


def main(argv=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    command = next((arg for arg in argv if arg in ('profiles', 'config', 'precheck', 'run', 'status', 'cancel', 'logs', 'check-package', *STEP_COMMANDS)), '')
    try:
        shared, args = arguments(argv)
        data, exit_code = dispatch(shared, args)
        error = None if exit_code == 0 else (data.get('job', {}).get('error') or data.get('error') or {
            'code': 'operation_failed', 'message': 'Operation failed; inspect data for check or job details.'})
    except config.ConfigError as exc:
        data, exit_code, error = {}, 4 if exc.code.startswith('backend_') else 2, {'code': exc.code, 'message': str(exc)}
    except OSError as exc:
        data, exit_code, error = {}, 2, {'code': 'io_error', 'message': str(exc)}
    response = {'schemaVersion': 1, 'contractVersion': CONTRACT_VERSION, 'command': command, 'ok': exit_code == 0, 'data': data, 'error': error}
    if '--json' in argv:
        print(json.dumps(response, ensure_ascii=False, separators=(',', ':')))
    else:
        print(json.dumps(response, ensure_ascii=False, indent=2))
    return exit_code


if __name__ == '__main__':
    sys.exit(main())
