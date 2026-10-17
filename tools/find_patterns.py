r"""Lists every byte pattern in the fix's source and how often each one occurs in a game exe.

The first thing to run after a game update (before the self-test, and without a compiler): a pattern that is found
once is fine; 0 = the code changed (re-find it, see docs/porting.md); 2+ = no longer unique (make it longer). The fix
itself also checks more than these patterns (offsets, call targets, layouts), so the self-test and the log stay the
authority; this is the quick map of where to look.

Usage:
  python tools/find_patterns.py [path\to\OblivionRemastered-Win64-Shipping.exe] [--src src/OblivionPerfFix/src]
  python tools/find_patterns.py <exe> --dump 0x26B9B08 [--len 64]     hex bytes at an RVA (to compare with a comment)
  python tools/find_patterns.py <exe> --find "48 8B ?? 05"             one pattern: every RVA where it matches
  python tools/find_patterns.py <exe> --save tools/pattern-baseline.tsv  record the counts of a build that works
By default the counts are compared with tools/pattern-baseline.tsv (made on the build the fix was tested on) and only
patterns whose count changed are flagged, so on a new game build the CHANGED lines are the ones to look at.
Without an exe path it looks up the Steam install (tools/GamePaths.ps1 does the same for PowerShell).
Standard library only. Prints RVAs as exe+0x..., the notation used in the source comments and in OblivionPerfFix.log.
"""
import argparse
import hashlib
import os
import re
import struct
import sys

HEX_LITERAL = re.compile(r'^(?:[0-9A-Fa-f?]{2})(?: [0-9A-Fa-f?]{2}| %s)*$')
STRING_RUN = re.compile(r'((?:"(?:[^"\\]|\\.)*"\s*)+)')
NAME_BEFORE = re.compile(r'(?:constexpr\s+char|const\s+char\s*\*\s*const)\s+(\w+)\s*(?:\[\])?\s*=\s*$')


class Image:
    def __init__(self, path):
        with open(path, 'rb') as f:
            self.data = f.read()
        d = self.data
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        if d[pe:pe + 4] != b'PE\0\0':
            raise SystemExit(f'{path}: not a PE file')
        nsec = struct.unpack_from('<H', d, pe + 6)[0]
        optsz = struct.unpack_from('<H', d, pe + 20)[0]
        self.timestamp = struct.unpack_from('<I', d, pe + 8)[0]
        opt = pe + 24
        self.size_of_image = struct.unpack_from('<I', d, opt + 56)[0]
        self.sections = []
        for i in range(nsec):
            o = opt + optsz + i * 40
            name = d[o:o + 8].rstrip(b'\0').decode(errors='replace')
            vsize, va, rsize, raw = struct.unpack_from('<IIII', d, o + 8)
            self.sections.append((name, va, vsize, raw, rsize))

    def section_bytes(self, name):
        for n, va, vs, raw, rs in self.sections:
            if n == name:
                return va, self.data[raw:raw + min(vs, rs) if vs else rs]
        return None, b''

    def rva_to_offset(self, rva):
        for n, va, vs, raw, rs in self.sections:
            if va <= rva < va + max(vs, rs):
                return rva - va + raw
        return None


def to_regex(pattern):
    parts = []
    for tok in pattern.split():
        parts.append(b'.' if tok == '??' else re.escape(bytes([int(tok, 16)])))
    return re.compile(b''.join(parts), re.DOTALL)


def search(img, pattern, limit=8):
    rx = to_regex(pattern)
    hits = []
    for name in ('.text',):
        va, blob = img.section_bytes(name)
        for m in rx.finditer(blob):
            hits.append(va + m.start())
            if len(hits) > limit:
                break
    return hits


def patterns_in(src_dir, lab=False):
    from pathlib import Path
    try:
        from strip_lab import strip_text
    except ImportError:
        if lab:
            raise SystemExit('--lab needs the maintainer source tree')
        strip_text = lambda text, *args, **kwargs: text
    root = Path(src_dir)
    for path in sorted(root.rglob('*')):
        if path.suffix not in ('.cpp', '.h') or (not lab and 'lab' in path.relative_to(root).parts):
            continue
        fname = path.relative_to(root).as_posix()
        text = path.read_text(encoding='utf-8-sig')
        if not lab:
            text = strip_text(text, str(path), preserve_lines=True)
        for m in STRING_RUN.finditer(text):
            parts = re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1))
            joined = ''.join(parts).strip()
            if not HEX_LITERAL.match(joined) or len(joined.split()) < 4:
                continue
            line = text.count('\n', 0, m.start()) + 1
            before = text[max(0, m.start() - 120):m.start()]
            nm = NAME_BEFORE.search(before)
            yield fname, line, (nm.group(1) if nm else ''), joined


def steam_exe():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r'Software\Valve\Steam') as k:
            steam = winreg.QueryValueEx(k, 'SteamPath')[0]
    except OSError:
        return None
    vdf = os.path.join(steam, 'steamapps', 'libraryfolders.vdf')
    libs = [steam]
    if os.path.exists(vdf):
        with open(vdf, encoding='utf-8', errors='replace') as f:
            libs += [p.replace('\\\\', '\\') for p in re.findall(r'"path"\s+"([^"]+)"', f.read())]
    for lib in libs:
        acf = os.path.join(lib, 'steamapps', 'appmanifest_2623190.acf')
        if os.path.exists(acf):
            with open(acf, encoding='utf-8', errors='replace') as f:
                inst = re.search(r'"installdir"\s+"([^"]+)"', f.read())
            if inst:
                exe = os.path.join(lib, 'steamapps', 'common', inst.group(1), 'OblivionRemastered', 'Binaries',
                                   'Win64', 'OblivionRemastered-Win64-Shipping.exe')
                if os.path.exists(exe):
                    return exe
    return None


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('exe', nargs='?')
    ap.add_argument('--src', default=os.path.join(here, '..', 'src', 'OblivionPerfFix', 'src'))
    ap.add_argument('--lab', action='store_true', help='include lab sources and branches')
    ap.add_argument('--dump', help='RVA to hex-dump, e.g. 0x26B9B08')
    ap.add_argument('--len', type=lambda s: int(s, 0), default=64)
    ap.add_argument('--find', help='one pattern to search, e.g. "48 8B ?? 05"')
    ap.add_argument('--baseline', default=os.path.join(here, 'pattern-baseline.tsv'))
    ap.add_argument('--save', help='write the counts of this exe as a baseline file')
    a = ap.parse_args()
    exe = a.exe or steam_exe()
    if not exe or not os.path.exists(exe):
        raise SystemExit('game exe not found: pass its path')
    img = Image(exe)
    print(f'# {os.path.basename(exe)}  TimeDateStamp 0x{img.timestamp:08X}  SizeOfImage 0x{img.size_of_image:X}')

    if a.dump:
        rva = int(a.dump, 0)
        off = img.rva_to_offset(rva)
        if off is None:
            raise SystemExit('RVA outside the image')
        blob = img.data[off:off + a.len]
        for i in range(0, len(blob), 16):
            print(f'exe+0x{rva + i:X}  ' + ' '.join(f'{b:02X}' for b in blob[i:i + 16]))
        return 0
    if a.find:
        hits = search(img, a.find, limit=50)
        print(f'{len(hits)}{"+" if len(hits) > 50 else ""} match(es): ' + ' '.join(f'exe+0x{h:X}' for h in hits[:50]))
        return 0

    base = {}
    if not a.save and os.path.exists(a.baseline):
        with open(a.baseline, encoding='utf-8') as f:
            for row in f:
                if row.startswith('#') or not row.strip():
                    continue
                key, count = row.rstrip('\n').split('\t')[:2]
                base[key] = count
        print(f'# compared with {os.path.basename(a.baseline)} ({len(base)} patterns)')
    rows, changed = [], 0
    print('| file:line | name | bytes | matches | baseline | where |')
    print('|---|---|---|---|---|---|')
    for fname, line, name, pat in patterns_in(a.src, a.lab):
        key = f'{fname}:{name or "-"}:' + hashlib.sha1(pat.encode()).hexdigest()[:10]
        if '%s' in pat:
            count, where = 'template', 'filled in at run time; see the code'
        else:
            hits = search(img, pat)
            count = f'{len(hits)}+' if len(hits) > 8 else str(len(hits))
            where = ' '.join(f'exe+0x{h:X}' for h in hits[:4]) + (' ...' if len(hits) > 4 else '')
        was = base.get(key, 'new' if base else '-')
        flag = ''
        if base and was != count:
            flag = ' **CHANGED**'; changed += 1
        elif not base and count == '0':
            flag = ' **not found**'; changed += 1
        rows.append(f'{key}\t{count}\t{fname}:{line}')
        print(f'| {fname}:{line} | {name} | {len(pat.split())} | {count}{flag} | {was} | {where} |')
    if a.save:
        with open(a.save, 'w', encoding='utf-8', newline='\n') as f:
            f.write(f'# find_patterns.py counts, TimeDateStamp 0x{img.timestamp:08X}, SizeOfImage 0x{img.size_of_image:X}\n')
            f.write('# key (file:constant:sha1 of the pattern)\tmatches\tfile:line when saved\n')
            f.write('\n'.join(rows) + '\n')
        print(f'\nsaved {len(rows)} counts to {a.save}')
    print()
    print(f'PATTERNS {"changed" if changed else "unchanged"}: {changed} of {len(rows)} differ from the baseline' if base
          else f'PATTERNS {len(rows)} listed, {changed} not found (no baseline to compare with)')
    print('Some patterns match several places on purpose (one bone-upload loop per CPU instruction set; short ones are')
    print('checked at a known address rather than searched). Read the comment above a CHANGED line, then docs/porting.md.')
    return 0


if __name__ == '__main__':
    sys.exit(main())
