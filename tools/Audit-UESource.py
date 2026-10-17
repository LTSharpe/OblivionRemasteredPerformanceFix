"""Checks that files we publish contain no verbatim Unreal Engine source (code or comments).

Epic's licence lets us name engine functions, classes, members and offsets and describe them in our own words, but not
redistribute engine code or comments. This tool finds runs of N consecutive tokens (default 12) that appear both in a
checked file and in a local UE source tree, and prints each match with its location so a person can judge it.

Usage: python tools/Audit-UESource.py --ue <UE source root> [--tokens 12] [--words] <files or folders ...>
  --words compares words only (catches comments copied with changed punctuation); try --words --tokens 10.
Exit code 0 = no matches, 1 = matches found (review them), 2 = usage error.
Only for the maintainer's checks: the UE source is not part of this repo and is never published.
"""
import argparse, os, re, sys, hashlib

TOKEN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*|0x[0-9A-Fa-f]+|\d+(?:\.\d+)?|\S")
UE_EXT = {'.cpp', '.h', '.inl', '.ush', '.usf', '.cs'}
OUR_EXT = {'.cpp', '.h', '.asm', '.md', '.txt', '.ini', '.ps1', '.cs', '.lua', '.py', '.yml', '.json', '.def'}
# Token runs made only of these are boilerplate (includes, punctuation, keywords), not authored text.
BORING = set('{ } ( ) ; , . : :: < > = * & [ ] # - + / " \' return const void int if else for while include pragma once '
             'static auto bool true false nullptr struct class namespace public private uint8 uint32 uint64 int32 int64 '
             'uint8_t uint32_t uint64_t size_t the a of to and in is'.split())


WORDS_ONLY = False
# printf-style format strings ("%.0f, %.0f, %.0f") are shared by everyone who prints a vector: not authored text.
FORMAT = set('% f d s x u i g e ld lu llu zu'.split())


def is_boring(t):
    # Names of one or two letters (x, dx, ay, bz) are the generic maths everyone writes the same way
    # ("dx * dx + dy * dy + dz * dz"), not authored text.
    return t in BORING or t in FORMAT or t.isdigit() or (len(t) <= 2 and t[0].isalpha())


def tokens(text):
    return [m.group(0) for m in TOKEN.finditer(text) if not WORDS_ONLY or m.group(0)[0].isalpha()]


def shingles(toks, n):
    for i in range(len(toks) - n + 1):
        run = toks[i:i + n]
        if sum(1 for t in run if not is_boring(t)) < n // 2:
            continue
        yield i, hashlib.blake2b(' '.join(run).encode(), digest_size=8).digest()


def files_under(paths, exts):
    for p in paths:
        if os.path.isfile(p):
            yield p
            continue
        for root, dirs, names in os.walk(p):
            dirs[:] = [d for d in dirs if d not in ('build', '.git', 'obj', '__pycache__')]
            for name in names:
                if os.path.splitext(name)[1].lower() in exts:
                    yield os.path.join(root, name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ue', required=True)
    ap.add_argument('--tokens', type=int, default=12)
    ap.add_argument('--words', action='store_true', help='compare words only (finds comments copied with other punctuation)')
    ap.add_argument('paths', nargs='+')
    a = ap.parse_args()
    global WORDS_ONLY
    WORDS_ONLY = a.words
    ours = {}
    for f in files_under(a.paths, OUR_EXT):
        with open(f, encoding='utf-8', errors='replace') as fh:
            text = fh.read()
        toks, lines = [], []
        for m in TOKEN.finditer(text):
            if WORDS_ONLY and not m.group(0)[0].isalpha():
                continue
            toks.append(m.group(0)); lines.append(text.count('\n', 0, m.start()) + 1)
        ours[f] = (toks, lines)
    want = {}
    for f, (toks, lines) in ours.items():
        for i, h in shingles(toks, a.tokens):
            want.setdefault(h, []).append((f, i))
    print(f'{len(ours)} files, {len(want)} token runs of {a.tokens}', file=sys.stderr)
    hits = {}
    for uf in files_under([a.ue], UE_EXT):
        with open(uf, encoding='utf-8', errors='replace') as fh:
            for i, h in shingles(tokens(fh.read()), a.tokens):
                if h in want:
                    for f, j in want[h]:
                        hits.setdefault((f, j), uf)
    if not hits:
        print('UE-AUDIT ok: no shared token runs')
        return 0
    # Merge adjacent runs into one finding per place.
    last = None
    for (f, j) in sorted(hits):
        toks, lines = ours[f]
        if last and last[0] == f and j <= last[1] + a.tokens:
            last = (f, j, last[2]); continue
        if last: report(ours, last, a.tokens, hits)
        last = (f, j, j)
    report(ours, last, a.tokens, hits)
    return 1


def report(ours, item, n, hits):
    f, end, start = item
    toks, lines = ours[f]
    text = ' '.join(toks[start:end + n])
    print(f'{f}:{lines[start]}  [{os.path.relpath(hits[(f, start)])}]  {text[:200]}')


if __name__ == '__main__':
    sys.exit(main())
