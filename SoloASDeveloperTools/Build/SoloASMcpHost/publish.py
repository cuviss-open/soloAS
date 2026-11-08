#!/usr/bin/env python3
"""Publish a self-contained MCP Host into this plugin's platform directory."""
from __future__ import annotations

import argparse
from pathlib import Path
import shutil
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--runtime', required=True, choices=('win-x64', 'osx-x64', 'osx-arm64', 'linux-x64'))
    parser.add_argument('--dotnet', default=shutil.which('dotnet'))
    args = parser.parse_args()
    if not args.dotnet:
        parser.error('a .NET 8+ SDK is required to build; specify --dotnet when it is not on PATH')
    root = Path(__file__).resolve().parent
    platform = {'win-x64': 'Win64', 'osx-x64': 'Mac', 'osx-arm64': 'MacArm64', 'linux-x64': 'Linux'}[args.runtime]
    output = root.parent.parent / 'Binaries/ThirdParty/SoloASMcpHost' / platform
    # Explicit official source supplies only SDK/runtime packs; the project has no package dependencies.
    command = [args.dotnet, 'publish', str(root / 'SoloASMcpHost.csproj'), '-c', 'Release',
               '-r', args.runtime, '--self-contained', 'true', '--source', 'https://api.nuget.org/v3/index.json',
               '--nologo', '-o', str(output)]
    return subprocess.run(command, cwd=root, timeout=600, check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
