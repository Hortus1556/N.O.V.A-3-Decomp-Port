"""Count ARM instruction mnemonics across .text to scope the recompiler."""
import sys, collections
sys.path.insert(0, 'E:/nova3/tools')
from elf import load
from capstone import Cs, CS_ARCH_ARM, CS_MODE_ARM

e = load()
addr, text = e.sec('.text')
md = Cs(CS_ARCH_ARM, CS_MODE_ARM)
md.skipdata = True
mix = collections.Counter(); special = collections.Counter(); total = 0
for ins in md.disasm_lite(text, addr):
    _, _, mn, ops = ins
    total += 1
    base = mn.split('.')[0]
    mix[base] += 1
    if 'pc' in ops and not mn.startswith(('b', 'push')) and ('ldr' in mn and ops.startswith('pc') or mn.startswith(('add', 'mov', 'ldm', 'pop')) and 'pc' in ops.split(',')[0]):
        special['write-pc:' + mn + ' ' + ops.split(',')[0]] += 1
    if mn.startswith(('ldrex', 'strex', 'dmb', 'swp', 'mrc', 'mcr', 'svc', 'clrex')): special[mn] += 1
print('total', total, 'distinct', len(mix))
for m, c in mix.most_common(): print(f'{m:12}{c}', end='\n' if c > 2000 else '  ')
print('\n--- special'); [print(k, v) for k, v in special.most_common(40)]
