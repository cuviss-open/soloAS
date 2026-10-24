# SPDX-License-Identifier: MPL-2.0
"""Public PR checks, using only Python's standard library."""
from pathlib import Path
import argparse
import json
import re
import subprocess
import urllib.parse
import urllib.request

ROOT = Path(__file__).resolve().parents[2]
SOURCE_SUFFIXES = {'.h', '.hpp', '.c', '.cpp', '.cs', '.as', '.py', '.sh', '.S', '.s', '.asm', '.ps1', '.psm1', '.bat', '.cmd', '.mjs', '.js', '.mm', '.m', '.cc', '.cxx'}


# Longest directory prefix wins; Cuviss changes follow their plugin's license.
SOURCE_LICENSES = {
    'AngelscriptRuntime/ThirdParty/': ('Zlib',),
    'AngelscriptRuntime/': ('MIT',),
    'AngelscriptManualBindPlugins/AngelscriptGAS/':
        ('Apache-2.0', 'MIT', 'Apache-2.0 OR MIT', '(Apache-2.0 OR MIT)',
         'MIT OR Apache-2.0', '(MIT OR Apache-2.0)'),
    'AngelscriptManualBindPlugins/AngelscriptEnhancedInput/': ('MIT',),
    'AngelscriptAutoBind/ThirdParty/libffi/': ('MIT',),
    '': ('MPL-2.0',),
}


def license_errors(name, text, require_spdx=False):
    if Path(name).suffix not in SOURCE_SUFFIXES:
        return []
    allowed = next(SOURCE_LICENSES[prefix] for prefix in
                   sorted(SOURCE_LICENSES, key=len, reverse=True) if name.startswith(prefix))
    header = '\n'.join(text.splitlines()[:20])
    identifiers = re.findall(r'SPDX-License-Identifier:\s*([^\r\n]+)', header)
    errors = []
    if require_spdx and not identifiers:
        errors.append(name + ': new source needs an SPDX header')
    if any(identifier.strip().removesuffix('*/').strip() not in allowed for identifier in identifiers):
        errors.append(name + ': SPDX license must follow its plugin (' + ' or '.join(allowed) + ')')
    if ('Mozilla Public License' in header or 'mozilla.org/MPL/2.0' in header) and 'MPL-2.0' not in allowed:
        errors.append(name + ': MPL notice conflicts with plugin license')
    return errors


def notices(path):
    text = path.read_text(errors='replace')
    return [line.strip() for line in text.splitlines()
            if re.search(r'SPDX-License-Identifier:|copyright|all rights reserved', line, re.I)]


def licenses():
    baseline = json.loads((ROOT / '.github/license-headers.json').read_text())
    errors = []
    base = __import__('os').environ.get('PR_BASE_SHA')
    if base:
        old = json.loads(subprocess.check_output(['git', '-C', str(ROOT), 'show', base + ':.github/license-headers.json']))
        # Existing notices cannot be erased by redefining the baseline in a PR.
        for name, header in old.items():
            if name not in baseline or baseline[name] != header:
                errors.append(name + ': retained license baseline changed')
        baseline = old
    for path in ROOT.rglob('*'):
        if not path.is_file() or '.git' in path.relative_to(ROOT).parts or path.suffix not in SOURCE_SUFFIXES:
            continue
        name = path.relative_to(ROOT).as_posix()
        actual = notices(path)
        if name in baseline:
            if actual != baseline[name]:
                errors.append(name + ': retained license notices changed; maintainer review required')
        errors.extend(license_errors(name, path.read_text(errors='replace'), require_spdx=name not in baseline))
    return errors


def links(site=None):
    errors = []
    checked = set()
    docs = [ROOT / name for name in ('README.md', 'README.zh-CN.md', 'CONTRIBUTING.md', 'RELEASE_NOTES.md', 'UPSTREAM.md')]
    docs += list((ROOT / 'docs').rglob('*.md')) if (ROOT / 'docs').exists() else []
    for doc in docs:
        text = re.sub(r'```.*?```', '', doc.read_text(), flags=re.S)
        urls = re.findall(r'\[[^\]]*\]\(([^\s)]+)(?:\s+[^)]*)?\)|<(https?://[^>]+)>', text)
        for pair in urls:
            url = next(x for x in pair if x).strip('<>')
            parsed = urllib.parse.urlsplit(url)
            if parsed.scheme in ('mailto',) or url.startswith('#'):
                continue
            if parsed.scheme in ('http', 'https'):
                if parsed.hostname == 'soloas.cuviss.com' and site:
                    dest = Path(site) / parsed.path.lstrip('/')
                    if not parsed.path or parsed.path.endswith('/'):
                        dest /= 'index.html'
                    if not dest.is_file():
                        errors.append(f'{doc.name}: missing built page {parsed.path}')
                    elif parsed.fragment and urllib.parse.unquote(parsed.fragment) not in re.findall(r'\bid=["\']([^"\']+)["\']', dest.read_text()):
                        errors.append(f'{doc.name}: missing built anchor {url}')
                elif site:
                    continue  # Offline release check validates published-site links against its build.
                elif url not in checked:
                    checked.add(url)
                    try:
                        request = urllib.request.Request(urllib.parse.urlunsplit(parsed._replace(fragment='')), headers={'User-Agent': 'soloAS-link-check'})
                        with urllib.request.urlopen(request, timeout=30) as response:
                            if response.status >= 400:
                                errors.append(f'{doc.name}: HTTP {response.status}: {url}')
                    except Exception as exc:
                        errors.append(f'{doc.name}: {url}: {type(exc).__name__}')
            elif not (doc.parent / urllib.parse.unquote(parsed.path)).is_file():
                errors.append(f'{doc.name}: missing relative link {url}')
    return errors


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('check', choices=['links', 'licenses'])
    parser.add_argument('--site')
    args = parser.parse_args()
    errors = links(args.site) if args.check == 'links' else licenses()
    for error in errors:
        print(error)
    print(f'{args.check}: {len(errors)} problems')
    raise SystemExit(bool(errors))
