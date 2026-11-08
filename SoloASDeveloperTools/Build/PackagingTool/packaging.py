"""Release-copy packaging executor shared by CLI, window and MCP consumers."""
from __future__ import annotations

from datetime import datetime, timezone
import codecs
import csv
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile
import time
import uuid

import config
import jit

TERMINAL = ('succeeded', 'failed', 'cancelled')
RUNTIME_ARGS = ['-unattended', '-nocrashreports', '-NullRHI', '-NoMessaging', '-NoASDebugServer',
                '-NoSound', '-NoSplash', '-as-exit-on-error']
SKIP_DIRECTORIES = {'.git', '.svn', '.vs', 'Intermediate', 'Saved', 'DerivedDataCache', 'key'}


def process_is_alive(pid: int) -> bool:
    if os.name == 'nt':
        import ctypes
        from ctypes import wintypes
        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.OpenProcess.argtypes = (wintypes.DWORD, wintypes.BOOL, wintypes.DWORD)
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.GetExitCodeProcess.argtypes = (wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD))
        kernel.CloseHandle.argtypes = (wintypes.HANDLE,)
        handle = kernel.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not handle:
            return ctypes.get_last_error() != 87  # Access denied must not be treated as dead.
        try:
            code = wintypes.DWORD()
            return not kernel.GetExitCodeProcess(handle, ctypes.byref(code)) or code.value == 259
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


class ProjectLock:
    """Serialize all release and checkpoint ownership for one project."""
    def __init__(self, directory: Path):
        self.directory = directory
        self.owner = directory / 'project.lock'
        self.guard = None
        self.owned = False

    def acquire(self):
        self.directory.mkdir(parents=True, exist_ok=True)
        self.guard = (self.directory / 'project.lock.guard').open('a+b')
        try:
            self.guard.seek(0, os.SEEK_END)
            if self.guard.tell() == 0:
                self.guard.write(b'\0')
                self.guard.flush()
            self.guard.seek(0)
            try:
                if os.name == 'nt':
                    import msvcrt
                    msvcrt.locking(self.guard.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(self.guard.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            except OSError as exc:
                raise config.ConfigError('project_locked', 'Another packaging job is using this project.') from exc
            try:
                fd = os.open(self.owner, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            except FileExistsError:
                try:
                    old = json.loads(self.owner.read_text(encoding='utf-8'))
                    pid = old['pid']
                    if type(pid) is not int or pid <= 0 or not isinstance(old['startedAt'], str):
                        raise ValueError('invalid lock metadata')
                except (ValueError, KeyError, TypeError):
                    # The exclusive guard proves no holder can still be writing
                    # this incomplete file after a crash.
                    pid = None
                if pid is not None and process_is_alive(pid):
                    raise config.ConfigError('project_locked', 'Another packaging job is using this project.')
                self.owner.unlink()
                fd = os.open(self.owner, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            self.owned = True
            with os.fdopen(fd, 'w', encoding='utf-8') as stream:
                json.dump({'pid': os.getpid(), 'startedAt': timestamp()}, stream)
                stream.flush()
                os.fsync(stream.fileno())
        except BaseException:
            self.release()
            raise

    def release(self):
        try:
            if self.owned:
                self.owner.unlink(missing_ok=True)
                self.owned = False
        finally:
            if self.guard:
                self.guard.close()  # Closing releases the OS lock on every exit path.
                self.guard = None


def owned_release(project: Path, name: str) -> Path:
    if not isinstance(name, str) or not re.fullmatch(r'AS-[a-z0-9_]{8}', name):
        raise config.ConfigError('invalid_release_owner', 'Release directory is not an owned temporary copy.')
    root = project.parent.parent.resolve()
    path = root / name
    if path.is_symlink() or (hasattr(path, 'is_junction') and path.is_junction()) or path.resolve().parent != root:
        raise config.ConfigError('invalid_release_owner', 'Release directory is not a direct owned directory.')
    return path


def cleanup_release(project: Path, name: str, directory: Path):
    try:
        path = owned_release(project, name)
        if path.exists():
            # Do not resolve the leaf again: a substituted link must never turn
            # an owned path into a recursive deletion outside its parent.
            target = str(path)
            if os.name == 'nt' and not target.startswith('\\\\?\\'):
                target = '\\\\?\\UNC\\' + target[2:] if target.startswith('\\\\') else '\\\\?\\' + target
            shutil.rmtree(target)
    except (config.ConfigError, OSError) as exc:
        try:
            with (directory / 'job.log').open('a', encoding='utf-8') as log:
                log.write(f'[{timestamp()}] Release cleanup failed ({project.parent.parent / name}): {exc}\n')
        except OSError:
            pass


def checkpoint_release(project: Path, checkpoint: dict, cli) -> str:
    name = checkpoint['releaseDirectory']
    owned_release(project, name)
    _, previous = cli.read_job(project, checkpoint['jobId'])
    if previous.get('state') not in TERMINAL or previous.get('releaseDirectory') != name:
        raise config.ConfigError('invalid_release_owner', 'Checkpoint release does not belong to its terminal job.')
    return name


def timestamp():
    return datetime.now(timezone.utc).isoformat()


def write_json(path: Path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    temporary.replace(path)


def engine_root(project: Path) -> Path:
    """Resolve the project's installed-engine association, without workspace configuration."""
    association = config.read_json(project).get('EngineAssociation', '')
    if os.name != 'nt':
        raise config.ConfigError('host_unsupported', 'Packaging currently requires native Windows Python and an installed Win64 engine.')
    import winreg
    for hive, key, value in (
        (winreg.HKEY_CURRENT_USER, r'Software\Epic Games\Unreal Engine\Builds', association),
        (winreg.HKEY_LOCAL_MACHINE, rf'SOFTWARE\EpicGames\Unreal Engine\{association}', 'InstalledDirectory'),
    ):
        try:
            with winreg.OpenKey(hive, key) as handle:
                candidate = Path(winreg.QueryValueEx(handle, value)[0])
            if (candidate / 'Engine/Build/Build.version').is_file():
                return candidate.resolve()
        except OSError:
            pass
    installed = Path(os.environ.get('PROGRAMDATA', r'C:\ProgramData')) / 'Epic/UnrealEngineLauncher/LauncherInstalled.dat'
    if installed.is_file():
        for entry in config.read_json(installed).get('InstallationList', []):
            if entry.get('AppName') == 'UE_' + association:
                candidate = Path(entry['InstallLocation'])
                if (candidate / 'Engine/Build/Build.version').is_file():
                    return candidate.resolve()
    raise config.ConfigError('engine_missing', f'No installed engine registered for EngineAssociation {association!r}.')


def toolchain(engine: Path) -> tuple[Path, Path, Path]:
    runtimes = sorted((engine / 'Engine/Binaries/ThirdParty/DotNet').glob('*/win-x64/dotnet.exe'))
    ubt = engine / 'Engine/Binaries/DotNET/UnrealBuildTool/UnrealBuildTool.dll'
    uat = engine / 'Engine/Binaries/DotNET/AutomationTool/AutomationTool.dll'
    if len(runtimes) != 1 or not ubt.is_file() or not uat.is_file():
        raise config.ConfigError('toolchain_missing', 'Expected one engine-bundled dotnet, UnrealBuildTool and AutomationTool.')
    return runtimes[0], ubt, uat


def precheck(project: Path, resolved: dict) -> dict:
    errors, checks = [], []
    effective = resolved['effective']
    try:
        engine = engine_root(project)
        toolchain(engine)
        checks.append({'name': 'engine_tools', 'state': 'passed'})
        if effective['platform'] != 'Win64':
            raise config.ConfigError('platform_unsupported', 'Only Win64 packaging is implemented in this executor.')
        if effective['deliveryMode'] != 'source' and effective['configuration'] != 'Shipping':
            raise config.ConfigError('configuration_unsupported', 'Cache-only delivery currently requires Shipping staging rules.')
        if effective['ioStore'] and not effective['pak']:
            raise config.ConfigError('container_mode_invalid', 'IoStore requires Pak staging for non-asset UFS files.')
        if effective['ioStore'] and not (engine / 'Engine/Binaries/Win64/UnrealPak.exe').is_file():
            raise config.ConfigError('container_reader_required', 'Container inspection requires this engine UnrealPak.exe.')
        target = project.parent / 'Source' / (effective['target'] + '.Target.cs')
        if list((project.parent / 'Source').glob('*.Target.cs')) and not target.is_file():
            raise config.ConfigError('target_missing', f'Game target does not exist: {target.name}')
        for field in ('maps', 'cookDirectories', 'extraFiles'):
            for value in effective[field]:
                if any(c in value for c in '\r\n\x00"'):
                    raise config.ConfigError('invalid_path', f'{field} contains a control character or quote.')
        for value in effective['extraFiles']:
            safe_project_path(project.parent / 'Content', value)
        free = shutil.disk_usage(project.parent).free
        if free < 2 * 1024**3:
            raise config.ConfigError('disk_space', 'Less than 2 GiB available on the release-copy volume.')
        if not os.access(project.parent.parent, os.W_OK):
            raise config.ConfigError('release_parent_not_writable', 'The project parent must allow an owned sibling release directory.')
        checks.append({'name': 'output_volume', 'state': 'passed', 'freeBytes': free})
    except (config.ConfigError, OSError) as exc:
        errors.append({'code': getattr(exc, 'code', 'io_error'), 'message': str(exc)})
    return {'scope': 'packaging', 'configurationValid': True, 'readyToPackage': not errors,
            'checks': checks, 'pendingChecks': ['native_build', 'provider_discovery'] if not errors else [],
            'errors': errors, 'config': resolved, 'cacheRequirements': config.cache_requirements()}


def safe_project_path(root: Path, value: str) -> Path:
    if '\\' in value or re.match(r'^[A-Za-z]:', value) or Path(value).is_absolute() or '..' in Path(value).parts:
        raise config.ConfigError('invalid_path', f'Expected a project-relative POSIX path: {value}')
    path = root / value
    if not path.resolve().is_relative_to(root.resolve()):
        raise config.ConfigError('invalid_path', 'Path resolves outside project.')
    return path


def io_path(path: Path) -> Path:
    """Use the same Win32 long-path spelling for scans and publication."""
    value = str(Path(path).resolve())
    if os.name == 'nt' and not value.startswith('\\\\?\\'):
        value = '\\\\?\\UNC\\' + value[2:] if value.startswith('\\\\') else '\\\\?\\' + value
    return Path(value)


def publish_package(source: Path, destination: Path):
    """Preserve loose-package assets beyond Win32 MAX_PATH, including UNC roots."""
    shutil.copytree(io_path(source), io_path(destination))


def project_files(root: Path):
    """Exclude derived/local data; retain installed binary-only plugin payloads."""
    for directory, folders, files in os.walk(root):
        parent = Path(directory)
        folders[:] = sorted(f for f in folders if f not in SKIP_DIRECTORIES and not (
            f == 'Binaries' and (parent == root or (parent / 'Source').is_dir())) and not (
            f in ('bin', 'obj') and any(parent.glob('*.csproj'))))
        for folder in folders:
            if (parent / folder).is_symlink():
                raise config.ConfigError('project_link_unsupported', f'Symlinked input: {(parent / folder).relative_to(root)}')
        for name in sorted(files):
            if name == '.git' or name.startswith('.env') or name.endswith(('.pdb', '.sln', '.suo')):
                continue
            path = parent / name
            if path.is_symlink():
                raise config.ConfigError('project_link_unsupported', f'Symlinked input: {path.relative_to(root)}')
            yield path


def freeze_project(root: Path) -> dict:
    return {p.relative_to(root).as_posix(): {'sha256': jit.sha256(p), 'bytes': p.stat().st_size}
            for p in project_files(root)}


def copy_project(project: Path, destination: Path, expected: dict):
    for relative, identity in expected.items():
        source, target = project.parent / relative, destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(source, target)
        if jit.sha256(target) != identity['sha256']:
            raise config.ConfigError('input_changed', f'Input changed during copy: {relative}')


def clean_producer(root: Path):
    """Remove recognized old generated data only in the owned release copy."""
    for directory in list(root.rglob('AS_JITTED_CODE')):
        if not directory.is_dir():
            continue
        files = [p for p in directory.rglob('*') if p.is_file()]
        if any(p.suffix not in ('.cpp', '.h', '.hpp') for p in files):
            raise config.ConfigError('stale_jit_unknown', f'Unknown content in {directory.relative_to(root)}')
        for path in files:
            path.unlink()
    stale = list(root.rglob('*.jit.cpp')) + list(root.rglob('*.jit.inl'))
    if stale:
        raise config.ConfigError('stale_jit_manual', 'Manually integrated JIT outside AS_JITTED_CODE requires an explicit removal diff.')
    for script in [root / 'Script', *root.glob('Plugins/**/Script')]:
        if script.is_dir():
            for name in config.BASE_CACHES:
                (script / name).unlink(missing_ok=True)
            unknown = [p.name for p in script.iterdir() if p.is_file() and 'cache' in p.name.lower()]
            if unknown:
                raise config.ConfigError('cache_output_contract_required', 'Unowned cache files: ' + ', '.join(unknown))


class Executor:
    def __init__(self, directory: Path):
        self.directory = directory
        self.sequence = 0
        self.receipts = []

    def run(self, args, label: str, cwd: Path, timeout=2700):
        if (self.directory / 'cancel.request.json').exists():
            raise config.ConfigError('cancelled', 'Cancellation requested.')
        self.sequence += 1
        logfile = self.directory / f'{self.sequence:02d}-{label}.log'
        command = [str(arg) for arg in args]
        record = {'name': label, 'arguments': command, 'log': logfile.name, 'startedAt': timestamp()}
        flags = {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP} if os.name == 'nt' else {'start_new_session': True}
        with logfile.open('wb') as output:
            process = subprocess.Popen(command, cwd=cwd, stdout=output, stderr=subprocess.STDOUT, **flags)
            record['pid'] = process.pid
            write_json(self.directory / 'active-process.json', record)
            started = time.monotonic()
            error = None
            reader = logfile.open('rb')
            combined = (self.directory / 'job.log').open('a', encoding='utf-8')
            combined.write(f'[{timestamp()}] {label} started (PID {process.pid})\n')
            combined.flush()
            decoder = codecs.getincrementaldecoder('utf-8')(errors='replace')
            def copy_output(final=False):
                while block := reader.read(65536):
                    combined.write(decoder.decode(block))
                if final:
                    combined.write(decoder.decode(b'', final=True))
                combined.flush()
            try:
                while process.poll() is None:
                    copy_output()
                    if (self.directory / 'cancel.request.json').exists():
                        error = 'cancelled'
                        break
                    if time.monotonic() - started >= timeout:
                        error = 'process_timeout'
                        break
                    time.sleep(0.25)
            finally:
                if process.poll() is None:
                    if os.name == 'nt':
                        subprocess.run(['taskkill.exe', '/PID', str(process.pid), '/T', '/F'], stdout=output, stderr=output, timeout=30, check=False)
                    else:
                        os.killpg(process.pid, signal.SIGKILL)
                    process.wait(timeout=30)
                record.update(exitCode=process.returncode, endedAt=timestamp(), error=error)
                copy_output(final=True)
                combined.write(f'[{timestamp()}] {label} finished (exit {process.returncode})\n')
                combined.close()
                reader.close()
                self.receipts.append(record)
                write_json(self.directory / 'processes.json', self.receipts)
                (self.directory / 'active-process.json').unlink(missing_ok=True)
        if error or process.returncode:
            raise config.ConfigError(error or 'process_failed', f'{label}: exit={process.returncode}, see {logfile.name}')
        return logfile


def provider_steps(executor: Executor, engine: Path, project: Path, resolved: dict, phase: str, frozen=None, *, outputs=None, script_directory=None, fingerprint=None):
    def request(value):
        request_id = uuid.uuid4().hex
        request_path = executor.directory / f'provider-{request_id}.request.json'
        response_path = executor.directory / f'provider-{request_id}.response.json'
        write_json(request_path, value)
        editor = engine / 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
        try:
            executor.run([editor, project, '-run=SoloASProviders', '-RequestFile=' + str(request_path),
                          '-ResponseFile=' + str(response_path), '-as-skip-write-bind-db', *RUNTIME_ARGS], 'providers-' + phase, project.parent, 600)
        except config.ConfigError as exc:
            if exc.code != 'process_failed' or not response_path.is_file():
                raise
            response = config.read_json(response_path)
            if response.get('ok') is not False:
                raise
        response = config.read_json(response_path)
        if response.get('ok') is not True:
            error = response.get('error') or {}
            raise config.ConfigError(error.get('code', 'provider_failed'), error.get('message', 'Provider failed.'))
        return response['data']
    # Discover through the real bridge if the tools plugin is installed in this project.
    tools = list((project.parent / 'Plugins').glob('**/SoloASDeveloperTools.uplugin'))
    disabled = any(p.get('Name') == 'SoloASDeveloperTools' and p.get('Enabled') is False
                   for p in config.read_json(project).get('Plugins', []))
    required = set(resolved['effective']['requiredProviders'])
    if extension_enabled(project):
        required.add('core_extensions')
    if not tools or disabled:
        if required:
            raise config.ConfigError('provider_missing', 'Required providers need SoloASDeveloperTools installed and enabled in this project.')
        return []
    current = request({'schemaVersion': 1, 'operation': 'list'}).get('packagingProviders', [])
    identity = [{'providerId': p['providerId'], 'version': p['version'], 'steps': p['steps']} for p in current]
    if frozen is not None and identity != frozen:
        raise config.ConfigError('provider_changed', 'Provider descriptors changed during this job.')
    missing = required - {p['providerId'] for p in current}
    if missing:
        raise config.ConfigError('provider_missing', 'Missing required providers: ' + ', '.join(sorted(missing)))
    for provider in current:
        for step in provider['steps']:
            if step['phase'] == phase and resolved['effective']['deliveryMode'] in step['modes']:
                data = request({'schemaVersion': 1, 'operation': 'packaging.execute', 'providerId': provider['providerId'],
                         'name': step['name'], 'jobId': executor.directory.name,
                         'arguments': {'config': resolved, 'project': str(project), 'jobDirectory': str(executor.directory),
                                       'scriptDirectory': str(script_directory or (project.parent / 'Script')),
                                       'expectedExtensionFingerprint': fingerprint or ''}})
                if outputs is not None:
                    outputs[step['name']] = data
    return identity


def extension_enabled(project: Path) -> bool:
    plugins = config.read_json(project).get('Plugins', [])
    selected = next((p.get('Enabled') for p in plugins if p.get('Name') == 'SoloASCoreExtensions'), None)
    if selected is not None:
        return selected is True
    return any(config.read_json(p).get('EnabledByDefault') is True
               for p in (project.parent / 'Plugins').glob('**/SoloASCoreExtensions.uplugin'))


def extension_precheck(outputs: dict, effective: dict, *, required=False):
    data = outputs.get('core_extensions.precheck')
    if data is None:
        if required:
            raise config.ConfigError('extension_contract_invalid', 'Enabled extensions must provide their precheck capability.')
        return None
    fingerprint = data.get('extensionFingerprint')
    assets = data.get('cookAssets')
    if (data.get('capability') != 'core_extensions' or not isinstance(fingerprint, str) or not fingerprint
            or not isinstance(assets, list) or any(not isinstance(a, str) or not a.startswith('/')
                or '..' in a.split('/') or any(c in a for c in '\r\n\x00"') for a in assets)):
        raise config.ConfigError('extension_contract_invalid', 'Provider must supply a fingerprint and cook asset paths.')
    # CookDir includes the asset's package folder; the provider owns which assets are necessary.
    for asset in assets:
        directory = asset.rsplit('/', 1)[0]
        if directory not in effective['cookDirectories']:
            effective['cookDirectories'].append(directory)
    return fingerprint


def provider_companions(outputs: dict) -> list[str]:
    names = []
    for data in outputs.values():
        if isinstance(data, dict) and 'requiredCompanions' in data:
            names.extend(config.companion_names(data['requiredCompanions']))
    return config.companion_names(names)


def bcr_arguments(project: Path, effective: dict, archive: Path, *, producer=False) -> list[str]:
    args = ['-WaitForUATMutex', '-ScriptsForProject=' + str(project), 'BuildCookRun', '-project=' + str(project),
            '-target=' + effective['target'], '-platform=Win64', '-clientconfig=' + effective['configuration'],
            '-NoP4', '-unattended', '-utf8output', '-nocompileeditor', '-build', '-cook', '-stage', '-package', '-archive',
            '-archivedirectory=' + str(archive), '-UbtArgs=-NoUBA -WaitMutex -NoEngineChanges -MaxParallelActions=8',
            '-AdditionalCookerOptions=-NoMessaging -NoASDebugServer -unattended -nocrashreports',
            '-ini:Game:[/Script/UnrealEd.ProjectPackagingSettings]:bUseZenStore=False']
    if not producer and effective['ioStore']:
        args.extend(['-pak', '-iostore'])
    else:
        args.extend(['-pak', '-skipiostore'] if effective['pak'] and not producer else ['-nopak', '-skipiostore'])
    if effective['maps']:
        args.append('-map=' + '+'.join(effective['maps']))
    if effective['deliveryMode'] != 'source':
        index = next(i for i, arg in enumerate(args) if arg.startswith('-UbtArgs='))
        args[index] += ' -NoUBTMakefiles'
    if not producer and effective['deliveryMode'] != 'source':
        # Plugin Game.ini layers are merged after the project platform ini.
        # Remove only the Bridge's source-tree staging entry for cache delivery.
        args.append('-ini:Game:[/Script/UnrealEd.ProjectPackagingSettings]:-DirectoriesToAlwaysStageAsNonUFS=(Path="../Script")')
        index = next(i for i, arg in enumerate(args) if arg.startswith('-AdditionalCookerOptions='))
        args[index] += ' -as-skip-write-bind-db'
    for directory in effective['cookDirectories']:
        args.append('-CookDir=' + directory)
    return args


def apply_release_settings(root: Path, effective: dict):
    # Clear inherited arrays in the platform layer so an invocation replacement
    # does not leave project ini entries active in the native cook/stage process.
    path = root / 'Config/Windows/WindowsGame.ini'
    original = path.read_bytes() if path.exists() else b''
    lines = ['', '[' + config.PACKAGING_SECTION + ']',
             'BuildTarget=' + effective['target'], 'BuildConfiguration=PPBC_' + effective['configuration'],
             'UsePakFile=' + str(effective['pak']), 'bUseIoStore=' + str(effective['ioStore'])]
    for key, field, member in (('MapsToCook', 'maps', 'FilePath'),
                               ('DirectoriesToAlwaysCook', 'cookDirectories', 'Path'),
                               ('DirectoriesToAlwaysStageAsNonUFS', 'extraFiles', 'Path')):
        lines.append('!' + key + '=ClearArray')
        lines.extend('+' + key + '=(' + member + '="' + value + '")' for value in effective[field])
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(original + ('\n'.join(lines) + '\n').encode('utf-8'))


def find_executable(archive: Path, target: str, configuration: str) -> Path:
    name = target + ('-Win64-' + configuration if configuration != 'Development' else '') + '.exe'
    scan_root = io_path(archive)
    found = list(scan_root.glob('Windows/*/Binaries/Win64/' + name))
    if len(found) != 1:
        raise config.ConfigError('executable_missing', f'Expected exactly one packaged {name}, found {len(found)}.')
    return archive / found[0].relative_to(scan_root)


def stage_plugin_scripts(project: Path, engine: Path, packaged: Path, roots_log: Path):
    """Stage the enabled roots reported by Runtime, preserving plugin locations."""
    match = re.search(r'\{\s*"AngelscriptScriptRoots":\s*\[.*\]\s*\}',
                      roots_log.read_text(encoding='utf-8-sig', errors='replace'))
    if not match:
        raise config.ConfigError('script_roots_missing', 'Runtime did not report its enabled script roots.')
    for value in json.loads(match.group())['AngelscriptScriptRoots']:
        source = Path(value).resolve()
        if source == (project.parent / 'Script').resolve():
            continue
        if source.is_relative_to((project.parent / 'Plugins').resolve()):
            destination = packaged / project.stem / source.relative_to(project.parent.resolve())
        elif source.is_relative_to((engine / 'Engine/Plugins').resolve()):
            destination = packaged / 'Engine' / source.relative_to((engine / 'Engine').resolve())
        else:
            raise config.ConfigError('script_root_unsupported', 'Script root is outside the release project and engine: ' + value)
        shutil.copytree(io_path(source), io_path(destination), dirs_exist_ok=True)


def package_files(root: Path, engine: Path | None, executor: Executor | None, expected_container=None):
    """Read actual IoStore indexes and UFS script bytes from every container."""
    values, containers = {}, []
    scan_root = io_path(root)
    for path in scan_root.rglob('*'):
        if path.is_file() and '/script/' in '/' + path.relative_to(scan_root).as_posix().lower():
            values[path.relative_to(scan_root).as_posix()] = path
    tocs = sorted(scan_root.rglob('*.utoc'))
    data_files = sorted(scan_root.rglob('*.ucas'))
    paks = sorted(scan_root.rglob('*.pak'))
    observed_container = 'iostore' if tocs or data_files else ('pak' if paks else 'loose')
    if expected_container is not None and expected_container != observed_container:
        raise config.ConfigError('container_mode_differs', f'Expected {expected_container}, found {observed_container}.')
    for data in data_files:
        stem = re.sub(r'_s[0-9]+$', '', data.stem)
        if not any(toc.parent == data.parent and toc.stem == stem for toc in tocs):
            raise config.ConfigError('container_missing', f'IoStore data has no TOC: {data.name}')
    for toc in tocs:
        if not toc.with_suffix('.ucas').is_file():
            raise config.ConfigError('container_missing', f'IoStore TOC has no data: {toc.name}')
        if engine is None or executor is None:
            raise config.ConfigError('container_reader_required', 'IoStore inspection needs this engine and an executor/log directory.')
        tool = engine / 'Engine/Binaries/Win64/UnrealPak.exe'
        listing = executor.directory / ('iostore-' + uuid.uuid4().hex + '.csv')
        executor.run([tool, '-ListContainer=' + str(root / toc.relative_to(scan_root)), '-Csv=' + str(listing),
                      '-unattended', '-nocrashreports', '-NoMessaging'], 'iostore-list', root, 600)
        with listing.open(encoding='utf-8-sig', newline='') as handle:
            reader = csv.DictReader(handle, skipinitialspace=True)
            if not reader.fieldnames or not {'Filename', 'ChunkId'} <= set(reader.fieldnames):
                raise config.ConfigError('container_index_invalid', 'IoStore reader did not produce a chunk index.')
            has_partition_index = 'PartitionIndex' in reader.fieldnames
            entries = list(reader)
        if not entries:
            raise config.ConfigError('container_index_invalid', f'IoStore index is empty: {toc.name}')
        for entry in entries:
            if has_partition_index:
                if not entry.get('PartitionIndex', '').isdecimal():
                    raise config.ConfigError('container_index_invalid', 'Invalid IoStore partition index.')
                partition = int(entry['PartitionIndex'])
            else:
                partition = 0
            data = toc.with_name(toc.stem + (f'_s{partition}' if partition else '') + '.ucas')
            if not data.is_file() or not data.stat().st_size:
                raise config.ConfigError('container_missing', f'IoStore partition missing or empty: {data.name}')
        containers.append({'path': toc.relative_to(scan_root).as_posix(), 'sha256': jit.sha256(toc),
                           'entries': entries})
        script_entries = [e['Filename'].replace('\\', '/') for e in entries
                          if '/script/' in '/' + e['Filename'].replace('\\', '/').lower()]
        if script_entries:
            output = executor.directory / ('extract-' + uuid.uuid4().hex)
            output.mkdir()
            executor.run([tool, root / toc.relative_to(scan_root), '-Extract', output, '-Filter=*/Script/*',
                          '-unattended', '-nocrashreports', '-NoMessaging'], 'iostore-extract', root, 600)
            extracted = {p.relative_to(output).as_posix(): p for p in output.rglob('*') if p.is_file()}
            for name in script_entries:
                key = name.removeprefix('../../../')
                if key not in extracted:
                    raise config.ConfigError('container_extract_incomplete', 'IoStore script not extracted: ' + name)
                if key in values:
                    raise config.ConfigError('duplicate_package_path', 'Duplicate loose/container path: ' + key)
                values[key] = extracted[key]
    containers.extend({'path': p.relative_to(scan_root).as_posix(), 'sha256': jit.sha256(p)} for p in data_files)
    for pak in paks:
        if engine is None or executor is None:
            raise config.ConfigError('container_reader_required', 'Pak inspection needs this engine and an executor/log directory.')
        output = executor.directory / ('extract-' + uuid.uuid4().hex)
        output.mkdir()
        tool = engine / 'Engine/Binaries/Win64/UnrealPak.exe'
        executor.run([tool, root / pak.relative_to(scan_root), '-Extract', output, '-Filter=*/Script/*', '-unattended', '-nocrashreports', '-NoMessaging'],
                     'pak-extract', root, 600)
        containers.append({'path': pak.relative_to(scan_root).as_posix(), 'sha256': jit.sha256(pak)})
        scan_output = io_path(output)
        for path in scan_output.rglob('*'):
            if path.is_file() and '/script/' in '/' + path.relative_to(scan_output).as_posix().lower():
                key = path.relative_to(scan_output).as_posix()
                if key in values:
                    raise config.ConfigError('duplicate_package_path', 'Duplicate loose/container path: ' + key)
                values[key] = path
    return values, containers


def inspect_package(path: Path, expected_mode=None, *, engine=None, executor=None, expected_container=None) -> dict:
    """Four outcomes; without native provenance never infer semi-AOT from cpp files."""
    root = Path(path).resolve()
    errors, observed = [], None
    try:
        files, containers = package_files(root, engine, executor, expected_container)
        sources = sorted(name for name in files if name.lower().endswith('.as'))
        cache_groups = {}
        for name, file in files.items():
            if file.name in config.BASE_CACHES:
                cache_groups.setdefault(Path(name).parent.as_posix(), {})[file.name] = file
        for parent, members in cache_groups.items():
            # Binds.Cache is the native binding database, also required when
            # compiling delivered .as. Only PrecompiledScript.Cache selects VM
            # bytecode over those sources.
            required = ('Binds.Cache',) if sources else config.BASE_CACHES
            for name in required:
                if name not in members or not members[name].stat().st_size:
                    errors.append({'code': 'cache_missing_or_empty', 'path': parent + '/' + name})
        precompiled = any('PrecompiledScript.Cache' in members for members in cache_groups.values())
        if sources and not precompiled:
            observed = 'source'
            if not cache_groups:
                errors.append({'code': 'binding_database_missing', 'message': 'A source Game package still needs its native Binds.Cache.'})
        elif cache_groups and not sources and not errors:
            observed = 'bytecode'
        else:
            errors.append({'code': 'mixed_or_missing_scripts', 'message': 'Expected source without caches or complete caches without source.'})
        manifest_path = root / 'SoloASPackage.json'
        manifest = config.read_json(manifest_path) if manifest_path.exists() else None
        if manifest is not None:
            if (not isinstance(manifest, dict) or manifest.get('schemaVersion') != 1 or
                    not isinstance(manifest.get('scriptArtifacts'), dict) or
                    not isinstance(manifest.get('executables'), dict) or
                    not isinstance(manifest.get('containers'), list) or
                    manifest.get('deliveryMode') not in ('source', 'bytecode', 'semi_aot') or
                    (manifest.get('jit') is not None and not isinstance(manifest['jit'], dict))):
                raise config.ConfigError('invalid_package_manifest', 'Invalid package provenance manifest.')
            expected_files = manifest.get('scriptArtifacts', {})
            actual_files = {name: {'sha256': jit.sha256(file), 'bytes': file.stat().st_size} for name, file in files.items()}
            if expected_files != actual_files:
                errors.append({'code': 'script_artifacts_differ', 'message': 'Script/cache files differ from this job generation/staging hashes.'})
            for relative, identity in manifest.get('executables', {}).items():
                if not isinstance(identity, dict) or not isinstance(identity.get('sha256'), str):
                    raise config.ConfigError('invalid_package_manifest', 'Invalid executable identity.')
                binary = safe_project_path(root, relative)
                if not binary.is_file() or jit.sha256(binary) != identity['sha256']:
                    errors.append({'code': 'executable_differs', 'path': relative})
            actual_container = 'iostore' if any(c['path'].endswith('.utoc') for c in containers) else ('pak' if containers else 'loose')
            if manifest.get('containerMode', actual_container) != actual_container:
                errors.append({'code': 'container_mode_differs'})
            if manifest.get('containers', []) != containers:
                errors.append({'code': 'container_differs'})
            native = manifest.get('jit', {})
            if native:
                if native.get('compiled') is True and native.get('linked') is True and native.get('generated') and native.get('objects') and manifest.get('executables'):
                    if observed == 'bytecode':
                        observed = 'semi_aot'
                    else:
                        errors.append({'code': 'jit_delivery_mismatch'})
                else:
                    errors.append({'code': 'jit_not_linked'})
            elif manifest.get('deliveryMode') == 'semi_aot':
                errors.append({'code': 'jit_not_linked'})
            if manifest.get('deliveryMode') != observed:
                errors.append({'code': 'declared_mode_differs'})
        elif cache_groups and observed != 'source':
            errors.append({'code': 'native_identity_unverified', 'message': 'Caches alone do not prove native build/configuration or absence/presence of linked JIT.'})
        if expected_mode and expected_mode != observed:
            errors.append({'code': 'expected_mode_differs', 'expected': expected_mode, 'observed': observed})
        return {'classification': 'inconsistent' if errors else observed, 'observedMode': observed, 'ok': not errors,
                'errors': errors, 'sourceFiles': sources, 'cacheDirectories': sorted(cache_groups),
                'containers': containers, 'nativeProvenance': bool(manifest), 'runtimeVerified': False}
    except (config.ConfigError, OSError) as exc:
        return {'classification': 'inconsistent', 'ok': False,
                'errors': [{'code': getattr(exc, 'code', 'io_error'), 'message': str(exc)}], 'runtimeVerified': False}


def cancel_job(directory: Path, job: dict) -> dict:
    if job['state'] not in TERMINAL:
        write_json(directory / 'cancel.request.json', {'schemaVersion': 1, 'jobId': directory.name, 'requestedAt': timestamp()})
    return job


def check_package(project: Path, path: Path, mode='auto') -> dict:
    try:
        engine = engine_root(project) if any(list(io_path(path).rglob('*' + suffix)) for suffix in ('.pak', '.utoc', '.ucas')) else None
        directory = project.parent / 'Saved/SoloASDeveloperTools/PackageChecks' / uuid.uuid4().hex
        directory.mkdir(parents=True, exist_ok=False)
        result = inspect_package(path, None if mode == 'auto' else mode, engine=engine, executor=Executor(directory))
        result['error'] = None if result['ok'] else {
            'code': 'package_inconsistent', 'message': json.dumps(result['errors'])}
        return result
    except (config.ConfigError, OSError) as exc:
        error = {'code': getattr(exc, 'code', 'io_error'), 'message': str(exc)}
        return {'classification': 'inconsistent', 'ok': False,
                'errors': [error], 'error': error, 'runtimeVerified': False}


def clean_cache(project: Path, resolved: dict) -> dict:
    return step_result(run_job(project, resolved, stop_after='clean'))


def generate_precompiled(project: Path, resolved: dict) -> dict:
    return step_result(run_job(project, resolved, stop_after='generate'))


def sync_jit(project: Path, resolved: dict) -> dict:
    return step_result(run_job(project, resolved, stop_after='sync'))


def step_result(job: dict) -> dict:
    return {'ok': job['state'] == 'succeeded', 'job': job, 'error': job.get('error')}


def run_job(project: Path, resolved: dict, *, stop_after=None) -> dict:
    # Import job persistence lazily so the B-line entry remains its single owner.
    import soloas_package as cli
    project = Path(project).resolve()
    directory, job = cli.create_job(project, resolved)
    job_id = job['jobId']
    cli.transition(directory, job, 'prechecking', phase='precheck')
    executor = Executor(directory)
    transaction = None
    checkpoint_path = directory.parent / 'steps.json'
    project_lock = None
    release = None
    release_owned = False
    release_new = False
    previous_clean_release = None
    def phase(name):
        if (directory / 'cancel.request.json').exists():
            raise config.ConfigError('cancelled', 'Cancellation requested.')
        if job['state'] == 'prechecking':
            cli.transition(directory, job, 'running', phase=name)
        else:
            cli.transition(directory, job, 'running', phase=name)
    try:
        project_lock = ProjectLock(directory.parent)
        project_lock.acquire()
        checked = precheck(project, resolved)
        write_json(directory / 'precheck.json', checked)
        if not checked['readyToPackage']:
            error = checked['errors'][0]
            raise config.ConfigError(error['code'], error['message'])
        effective = json.loads(json.dumps(resolved['effective']))
        if stop_after in ('generate', 'sync') and effective['deliveryMode'] == 'source':
            raise config.ConfigError('step_mode', 'Precompiled steps require bytecode or semi_aot delivery.')
        if stop_after == 'sync' and effective['deliveryMode'] != 'semi_aot':
            raise config.ConfigError('step_mode', 'sync-jit requires semi_aot delivery.')
        engine = engine_root(project)
        dotnet, ubt, uat = toolchain(engine)
        phase('prepare')
        original = freeze_project(project.parent)
        frozen_inputs = {'project': original, 'engineBuild': config.read_json(engine / 'Engine/Build/Build.version'),
                         'engineTools': {p.relative_to(engine).as_posix(): jit.sha256(p) for p in (dotnet, ubt, uat)},
                         'executor': {name: jit.sha256(Path(__file__).parent / name)
                                      for name in ('packaging.py', 'jit.py', 'config.py')},
                         'configDigest': resolved['configDigest']}
        write_json(directory / 'frozen-inputs.json', frozen_inputs)
        # Native toolchains still impose path limits on some file operations.
        # Keep the build root short; the longer public job directory holds logs.
        checkpoint = None
        if stop_after == 'clean' and checkpoint_path.exists():
            try:
                previous_clean_release = checkpoint_release(project, config.read_json(checkpoint_path), cli)
            except (config.ConfigError, KeyError, TypeError, ValueError):
                # An unverified prior checkpoint must never authorize deletion.
                previous_clean_release = None
        if stop_after != 'clean' and checkpoint_path.exists():
            checkpoint = config.read_json(checkpoint_path)
            try:
                previous_directory, previous_job = cli.read_job(project, checkpoint['jobId'])
                if previous_job['state'] != 'succeeded':
                    raise config.ConfigError('step_incomplete', 'Previous step did not succeed.')
                if config.read_json(previous_directory / 'frozen-inputs.json') != frozen_inputs:
                    raise config.ConfigError('step_inputs_changed', 'Project, configuration or engine differs from the previous step; clean again.')
                release = safe_project_path(project.parent.parent, checkpoint['releaseDirectory'])
                if freeze_project(release) != checkpoint['releaseFiles']:
                    raise config.ConfigError('step_release_changed', 'Release-copy inputs differ from the previous step; clean again.')
                if checkpoint['stage'] in ('generate', 'sync'):
                    previous_frozen = previous_directory / 'Generated'
                    if jit.inventory(previous_frozen) != checkpoint['generatedFiles']:
                        raise config.ConfigError('step_artifacts_changed', 'Previous generation is missing or changed; generate again from clean inputs.')
                checkpoint_release(project, checkpoint, cli)
                release_owned = True
            except config.ConfigError as exc:
                if stop_after is not None or exc.code not in ('step_incomplete', 'step_inputs_changed', 'step_release_changed', 'step_artifacts_changed'):
                    raise
                discarded_release = None
                try:
                    discarded_release = checkpoint_release(project, checkpoint, cli)
                except (config.ConfigError, KeyError, TypeError, ValueError):
                    pass
                checkpoint_path.unlink()
                with (directory / 'job.log').open('a', encoding='utf-8') as log:
                    log.write(f'[{timestamp()}] Discarded previous checkpoint ({exc.code}): {exc}. Starting from clean inputs.\n')
                if discarded_release:
                    cleanup_release(project, discarded_release, directory)
                checkpoint = None
                release = None
                release_owned = False
            if checkpoint is not None and checkpoint['stage'] in ('generate', 'sync'):
                shutil.copytree(previous_frozen, directory / 'Generated')
                for name in ('providers.json', 'extension.json'):
                    shutil.copyfile(previous_directory / name, directory / name)
        if checkpoint is None:
            if stop_after in ('generate', 'sync'):
                raise config.ConfigError('step_required', 'Run clean-cache before generate-precompiled, then sync-jit.')
            release = Path(tempfile.mkdtemp(prefix='AS-', dir=project.parent.parent))
            release_owned = True
            release_new = True
            job['releaseDirectory'] = release.name
            cli.write_job(directory, job)
            copy_project(project, release, original)
            clean_producer(release)
        job['releaseDirectory'] = release.name
        cli.write_job(directory, job)
        consumer = release / project.name
        completed = checkpoint['stage'] if checkpoint else None
        def finish_step(stage):
            if (directory / 'cancel.request.json').exists():
                raise config.ConfigError('cancelled', 'Cancellation requested before step completion.')
            if freeze_project(project.parent) != original:
                raise config.ConfigError('source_project_changed', 'Development project changed during this step.')
            state = {'jobId': job_id, 'stage': stage, 'releaseDirectory': release.name,
                     'releaseFiles': freeze_project(release),
                     'generatedFiles': jit.inventory(directory / 'Generated') if stage != 'clean' else {}}
            job['result'] = {'step': stage, 'releaseDirectory': release.name}
            write_json(checkpoint_path, state)
            cli.transition(directory, job, 'succeeded', phase=job['phase'])
            return job
        if stop_after == 'clean':
            result = finish_step('clean')
            if previous_clean_release and previous_clean_release != release.name:
                cleanup_release(project, previous_clean_release, directory)
            return result
        if stop_after == 'generate' and completed != 'clean':
            raise config.ConfigError('step_order', 'generate-precompiled needs a clean-cache checkpoint.')
        if stop_after == 'sync' and completed != 'generate':
            raise config.ConfigError('step_order', 'sync-jit needs a generate-precompiled checkpoint.')
        if completed not in ('generate', 'sync'):
            # Cook requires the actual project's Editor modules. Build failure stops before any launch.
            editor_target = effective['target'] + 'Editor'
            if not (release / 'Source' / (editor_target + '.Target.cs')).exists():
                candidates = list((release / 'Source').glob('*Editor.Target.cs'))
                if len(candidates) == 1:
                    editor_target = candidates[0].name.removesuffix('.Target.cs')
                elif not list((release / 'Source').glob('*.Target.cs')):
                    editor_target = 'UnrealEditor'
                else:
                    raise config.ConfigError('editor_target_missing', 'Cannot identify one project Editor target.')
            executor.run([dotnet, ubt, editor_target, 'Win64', 'Development', '-Project=' + str(consumer),
                          '-NoUBA', '-WaitMutex', '-NoEngineChanges', '-MaxParallelActions=8',
                          '-Log=' + str(directory / 'editor-build-ubt.log')], 'editor-build', engine / 'Engine/Source')
            # UBT expands Script/... before Cook. Generate the real native binding
            # database now so its paths enter the Game staging receipt. Cook may
            # refresh the bytes later; source delivery still has no script bytecode.
            (release / 'Script').mkdir(exist_ok=True)
            executor.run([engine / 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe', consumer,
                          '-as-write-bind-db', '-run=AngelscriptAllScriptRoots', *RUNTIME_ARGS,
                          '-abslog=' + str(directory / 'binding-database-game.log')],
                         'binding-database', release, 600)
            for name in ('Binds.Cache', 'Binds.Cache.Headers'):
                if not (release / 'Script' / name).is_file() or not (release / 'Script' / name).stat().st_size:
                    raise config.ConfigError('binding_database_missing', 'Editor did not generate ' + name + ' before Game staging.')
            outputs = {}
            providers = provider_steps(executor, engine, consumer, resolved, 'precheck', outputs=outputs)
            fingerprint = extension_precheck(outputs, effective, required=extension_enabled(consumer))
            companions = provider_companions(outputs)
            write_json(directory / 'extension.json', {'fingerprint': fingerprint, 'cookDirectories': effective['cookDirectories'],
                                                       'requiredCompanions': companions})
            apply_release_settings(release, effective)
            write_json(directory / 'providers.json', providers)
        else:
            providers = config.read_json(directory / 'providers.json')
            outputs = {}
            provider_steps(executor, engine, consumer, resolved, 'precheck', providers, outputs=outputs)
            fingerprint = extension_precheck(outputs, effective, required=extension_enabled(consumer))
            companions = provider_companions(outputs)
            extension = config.read_json(directory / 'extension.json')
            if extension != {'fingerprint': fingerprint, 'cookDirectories': effective['cookDirectories'],
                             'requiredCompanions': companions}:
                raise config.ConfigError('extension_identity_changed', 'Extension fingerprint or cook assets changed between steps.')
        archive = directory / 'Archive'
        mode = effective['deliveryMode']
        jit_result = None
        frozen = directory / 'Generated'
        if mode != 'source' and completed not in ('generate', 'sync'):
            phase('generate')
            producer = directory / 'Producer'
            executor.run([dotnet, uat, *bcr_arguments(consumer, effective, producer, producer=True)], 'producer-package', engine / 'Engine/Source')
            stage_plugin_scripts(consumer, engine, producer / 'Windows', directory / 'binding-database-game.log')
            exe = find_executable(producer, effective['target'], effective['configuration'])
            args = [exe, *effective['maps'][:1], '-as-generate-precompiled-data', '-as-write-bind-db', *RUNTIME_ARGS,
                    '-abslog=' + str(directory / 'generate-game.log')]
            executor.run(args, 'generate', producer / 'Windows', 600)
            phase('collect')
            script = exe.parents[2] / 'Script'
            jit.collect(producer / 'Windows/AS_JITTED_CODE', script, frozen,
                        ['core_extensions'] if fingerprint else [], companions)
            outputs = {}
            provider_steps(executor, engine, consumer, resolved, 'before_stage', providers,
                           outputs=outputs, script_directory=script, fingerprint=fingerprint)
            if mode != 'source' and fingerprint and outputs.get('core_extensions.validate_caches', {}).get('extensionFingerprint') != fingerprint:
                raise config.ConfigError('extension_identity_changed', 'Generated caches differ from the extension provider identity.')
        if mode != 'source' and completed not in ('generate', 'sync'):
            provider_steps(executor, engine, consumer, resolved, 'after_generate', providers)
        if stop_after == 'generate':
            jit.verify_collected(frozen)
            return finish_step('generate')
        if mode != 'source':
            if mode == 'semi_aot' and completed != 'sync':
                provider_steps(executor, engine, consumer, resolved, 'before_jit_sync', providers)
                phase('jit_sync')
                transaction = jit.Transaction(release, directory / 'Rollback')
                jit_result = jit.sync(consumer, effective['target'], frozen / 'AS_JITTED_CODE', engine, transaction)
                executor.run([dotnet, ubt, '-ProjectFiles', '-Game', '-CurrentPlatform', '-WaitMutex', '-Project=' + str(consumer),
                              '-Log=' + str(directory / 'project-files-ubt.log')],
                             'project-files', engine / 'Engine/Source', 600)
                graph = directory / 'jit-actions.json'
                executor.run([dotnet, ubt, effective['target'], 'Win64', effective['configuration'], '-Project=' + str(consumer),
                              '-NoUBA', '-WaitMutex', '-NoEngineChanges', '-NoUBTMakefiles', '-MaxParallelActions=8',
                              '-WriteOutdatedActions=' + str(graph), '-Log=' + str(directory / 'jit-actions-ubt.log')],
                             'jit-actions', engine / 'Engine/Source', 600)
                jit_actions = jit.compile_actions(graph, release / 'Source' / jit.MODULE / 'AS_JITTED_CODE',
                    release / 'Binaries/Win64' / (effective['target'] + '-Win64-' + effective['configuration'] + '.exe'))
            if completed == 'sync':
                jit_result = config.read_json(previous_directory / 'jit-result.json')
                jit_actions = config.read_json(previous_directory / 'jit-actions-selected.json')
            if jit_result:
                write_json(directory / 'jit-result.json', jit_result)
                write_json(directory / 'jit-actions-selected.json', jit_actions)
        if stop_after == 'sync':
            jit.verify_collected(frozen)
            return finish_step('sync')
        if mode != 'source':
            jit.verify_collected(frozen)
            for cache in (frozen / 'Script').iterdir():
                destination = release / 'Script' / cache.name
                destination.parent.mkdir(exist_ok=True)
                shutil.copyfile(cache, destination)
            if config.check_caches(release / 'Script', ['core_extensions'] if fingerprint else []):
                raise config.ConfigError('cache_restore_failed', 'Cache set is incomplete before BuildCookRun.')
        phase('build')
        executor.run([dotnet, ubt, effective['target'], 'Win64', effective['configuration'], '-Project=' + str(consumer),
                      '-NoUBA', '-WaitMutex', '-NoEngineChanges', '-NoUBTMakefiles', '-MaxParallelActions=8',
                      '-Log=' + str(directory / 'game-build-ubt.log')], 'game-build', engine / 'Engine/Source')
        if jit_result:
            jit_result['objects'] = jit.verify_objects(jit_actions)
            jit_result['linked'] = True
        phase('stage')
        outputs = {}
        provider_steps(executor, engine, consumer, resolved, 'before_stage', providers, outputs=outputs, fingerprint=fingerprint)
        if mode != 'source' and fingerprint and outputs.get('core_extensions.validate_caches', {}).get('extensionFingerprint') != fingerprint:
            raise config.ConfigError('extension_identity_changed', 'Cache validation fingerprint differs from generation.')
        executor.run([dotnet, uat, *bcr_arguments(consumer, effective, archive)], 'final-package', engine / 'Engine/Source')
        packaged = archive / 'Windows'
        if mode == 'source':
            stage_plugin_scripts(consumer, engine, packaged, directory / 'binding-database-game.log')
        exe = find_executable(archive, effective['target'], effective['configuration'])
        if mode != 'source':
            jit.verify_collected(frozen)
            for name in companions:
                destination = exe.parents[2] / 'Script' / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(frozen / 'Script' / name, destination)
        phase('package_check')
        container_mode = 'iostore' if effective['ioStore'] else ('pak' if effective['pak'] else 'loose')
        files, containers = package_files(packaged, engine, executor, container_mode)
        if mode != 'source':
            for cache in (frozen / 'Script').iterdir():
                delivered = [p for p in files.values() if p.name == cache.name]
                if len(delivered) != 1 or jit.sha256(delivered[0]) != jit.sha256(cache):
                    raise config.ConfigError('cache_generation_differs', f'Final package cache differs from generation: {cache.name}')
        built = release / 'Binaries/Win64' / exe.name
        if not built.is_file() or jit.sha256(built) != jit.sha256(exe):
            raise config.ConfigError('executable_differs', 'Archived executable differs from the current Game build.')
        if jit_result:
            # A successful clean module build compiles its original cpp units. Bind the exact binary.
            jit_result['compiled'] = True
        manifest = {'schemaVersion': 1, 'jobId': job_id, 'deliveryMode': mode, 'configuration': effective['configuration'],
                    'configDigest': resolved['configDigest'], 'inputDigest': hashlib.sha256(json.dumps(frozen_inputs, sort_keys=True).encode()).hexdigest(),
                    'scriptArtifacts': {name: {'sha256': jit.sha256(p), 'bytes': p.stat().st_size} for name, p in files.items()},
                    'executables': {exe.relative_to(packaged).as_posix(): {'sha256': jit.sha256(exe)}},
                    'containers': containers, 'containerMode': container_mode, 'jit': jit_result}
        write_json(packaged / 'SoloASPackage.json', manifest)
        inspection = inspect_package(packaged, mode, engine=engine, executor=executor, expected_container=container_mode)
        write_json(directory / 'package-check.json', inspection)
        if not inspection['ok']:
            raise config.ConfigError('package_inconsistent', json.dumps(inspection['errors']))
        provider_steps(executor, engine, consumer, resolved, 'after_package_check', providers)
        if freeze_project(project.parent) != original:
            raise config.ConfigError('source_project_changed', 'The development project changed during packaging; release retained for review.')
        output = safe_project_path(project.parent, effective['outputDirectory']) / job_id
        if (directory / 'cancel.request.json').exists():
            raise config.ConfigError('cancelled', 'Cancellation requested before publishing.')
        output.parent.mkdir(parents=True, exist_ok=True)
        publish_package(packaged, output)
        if (directory / 'cancel.request.json').exists():
            raise config.ConfigError('cancelled', 'Cancellation requested while copying output; owned partial output retained.')
        job['result'] = {'outputDirectory': output.relative_to(project.parent).as_posix(), 'inspection': inspection,
                         'manifest': 'SoloASPackage.json', 'logs': 'processes.json'}
        cli.transition(directory, job, 'succeeded', phase='package_check')
        if checkpoint is not None:
            checkpoint_path.unlink(missing_ok=True)
    except (config.ConfigError, OSError, ValueError, subprocess.SubprocessError) as exc:
        error = {'code': getattr(exc, 'code', 'packaging_failed'), 'message': str(exc)}
        if transaction:
            conflicts = transaction.restore()
            if conflicts:
                error['rollbackConflicts'] = conflicts
                error['cause'] = error['code']
                error['code'] = 'rollback_conflict'
        if error['code'] == 'cancelled':
            if job['state'] == 'running':
                cli.transition(directory, job, 'cancelling', phase=job['phase'])
            cli.transition(directory, job, 'cancelled', phase=job['phase'])
        else:
            cli.transition(directory, job, 'failed', phase=job['phase'], error=error)
    finally:
        if (project_lock and project_lock.owned and release_owned and release
                and (stop_after is None or (release_new and job['state'] != 'succeeded'))):
            try:
                if checkpoint_path.exists() and config.read_json(checkpoint_path).get('releaseDirectory') == release.name:
                    checkpoint_path.unlink()
            except (config.ConfigError, OSError, AttributeError) as exc:
                try:
                    with (directory / 'job.log').open('a', encoding='utf-8') as log:
                        log.write(f'[{timestamp()}] Checkpoint cleanup failed ({checkpoint_path}): {exc}\n')
                except OSError:
                    pass
            cleanup_release(project, release.name, directory)
        if project_lock:
            try:
                project_lock.release()
            except OSError as exc:
                try:
                    with (directory / 'job.log').open('a', encoding='utf-8') as log:
                        log.write(f'[{timestamp()}] Project lock cleanup failed: {exc}\n')
                except OSError:
                    pass
    return job
