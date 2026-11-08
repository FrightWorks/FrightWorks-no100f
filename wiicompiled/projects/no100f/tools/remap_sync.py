"""Keep the runtime's HLE hooks in step with the target MAP, by name.

hook_remap.txt lists every hook: first address -> current address, name or PARKED
(name). For each named hook, the wanted address is MAP[name] when the MAP names that function, else a parked dead
address (0x80001000 + 4n, unique per hook). Hooks whose current address differs from the wanted one are rewritten in
the runtime sources, and the report is rewritten. Run after every MAP change (manual fixes included).

Usage: remap_sync.py <runtime/src> <target MAP> <hook_remap.txt>
"""
import os, re, sys

def main():
    src, target_map, report = sys.argv[1:4]
    target = {}
    for line in open(target_map):
        p = line.split()
        if len(p) >= 2 and not p[1].startswith('fn_'): target.setdefault(p[1], int(p[0], 16))
    rows = []
    for line in open(report):
        m = re.match(r'([0-9A-F]{8}) -> ([0-9A-F]{8})  (?:PARKED \((\S+)\)|(\S+))\s+(\S+)', line)
        if m: rows.append([int(m.group(1), 16), int(m.group(2), 16), m.group(3) or m.group(4), m.group(5)])
    used_dead = {r[1] for r in rows if 0x80001000 <= r[1] < 0x80003000}
    next_dead = max(used_dead | {0x80000FFC}) + 4
    moves = {}
    for r in rows:
        old, cur, name, path = r
        want = target.get(name)
        if want is None:
            if 0x80001000 <= cur < 0x80003000: continue  # already parked
            want = next_dead; next_dead += 4
        if want != cur:
            moves[cur] = want; r[1] = want
    pat = re.compile(r'(PPC_NATIVE_OVERRIDE(?:_VOID)?\(\s*|(?:REGISTER_NATIVE_FUNCTION|REGISTER_TRANSLATED_FUNCTION)\w*\(\s*0x)([0-9A-Fa-f]{8})')
    for root, _, files in os.walk(src):
        for f in files:
            if not f.endswith(('.cpp', '.h', '.inc')): continue
            path = os.path.join(root, f)
            text = open(path, encoding='utf-8').read()
            new_text = pat.sub(lambda m: m.group(1) + ('%08X' % moves[int(m.group(2), 16)] if int(m.group(2), 16) in moves else m.group(2)), text)
            if new_text != text: open(path, 'w', encoding='utf-8', newline='').write(new_text)
    with open(report, 'w') as out:
        for old, cur, name, path in rows:
            parked = 0x80001000 <= cur < 0x80003000
            out.write('%08X -> %08X  %-40s %s\n' % (old, cur, ('PARKED (%s)' % name) if parked else name, path))
    print('moved %d hooks; %d parked' % (len(moves), sum(1 for r in rows if 0x80001000 <= r[1] < 0x80003000)))

if __name__ == '__main__':
    main()
