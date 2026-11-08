"""Find functions of a donor game (with symbols) in a stripped target main.dol by masked instruction match.

Each instruction is reduced to Dolphin's signature key (opcode + non-address fields; branch targets and immediates
dropped), so the same function compiled at another address, with other data addresses, still matches. A donor
function matches when its whole masked body occurs exactly once in the target text (several hits = ambiguous).
Donor symbols: "addr size name", both hex.

Usage: donormatch.py <donor main.dol> <donor symbols> <target main.dol> <out MAP> [--min 24]
"""
import struct, sys, collections
sys.path.insert(0, __import__('os').path.dirname(__file__))
from sigmatch import dol_text, op_key

def masked_text(d):
    words = {}
    for addr, data in dol_text(d):
        for i in range(0, len(data) - 3, 4): words[addr + i] = struct.unpack('>I', data[i:i + 4])[0]
    return words

def main():
    donor_dol, donor_syms, target_dol, out = sys.argv[1:5]
    minsize = int(sys.argv[sys.argv.index('--min') + 1]) if '--min' in sys.argv else 24
    dw = masked_text(open(donor_dol, 'rb').read())
    tw = masked_text(open(target_dol, 'rb').read())
    taddrs = sorted(tw)
    tkeys = [op_key(tw[a]) for a in taddrs]
    index = collections.defaultdict(list)
    for i in range(len(taddrs) - 3):
        if taddrs[i + 3] - taddrs[i] == 12: index[tuple(tkeys[i:i + 4])].append(i)
    found, ambiguous, missing = {}, 0, 0
    for line in open(donor_syms):
        p = line.split()
        if len(p) < 3: continue
        addr, size, name = int(p[0], 16), int(p[1], 16), p[2]
        if size < minsize or any(addr + o not in dw for o in range(0, size, 4)): continue
        keys = [op_key(dw[addr + o]) for o in range(0, size, 4)]
        hits = [i for i in index.get(tuple(keys[:4]), []) if tkeys[i:i + len(keys)] == keys
                and taddrs[i + len(keys) - 1] - taddrs[i] == size - 4]
        if len(hits) == 1: found[taddrs[hits[0]]] = name
        elif len(hits) > 1: ambiguous += 1
        else: missing += 1
    with open(out, 'w') as f:
        for a in sorted(found): f.write('%08X %s\n' % (a, found[a]))
    print('matched %d, ambiguous %d, not found %d' % (len(found), ambiguous, missing))

if __name__ == '__main__':
    main()
