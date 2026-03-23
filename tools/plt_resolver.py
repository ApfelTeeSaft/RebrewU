#!/usr/bin/env python3
"""
plt_resolver.py — Wii U RPX PLT Import Resolver for RebrewU
============================================================
Maps PLT stub addresses (0x02D0xxxx in .text) to their Wii U SDK function names
by correlating R_PPC_REL24 (type=10) relocations in .rela.text with the ELF
symbol table's import-section entries.

Usage:
    python3 plt_resolver.py Gambit.rpx [--port-dir ../port]

Outputs:
    - port/os/gambit_plt_auto.cpp   (STUB registration for all stubs)
    - port/os/coreinit/coreinit_addrs.h
    - port/os/gx2/gx2_addrs.h
    - port/os/snd_core/snd_core_addrs.h
    - port/os/vpad/vpad_addrs.h
    - port/os/padscore/padscore_addrs.h
    - port/os/stubs/stubs_addrs.h
    - plt_full_index.txt            (human-readable full listing)
"""

import argparse
import collections
import json
import os
import re
import struct
import sys
import zlib

SHF_RPL_ZLIB = 0x08000000
SHT_SYMTAB   = 2
SHT_RELA     = 4
R_PPC_REL24  = 10   # BL / B instruction relocation (PPC ABI type 10)

DEDICATED_MODULES = {'coreinit', 'gx2', 'snd_core', 'vpad', 'padscore'}


def read32(data, off): return struct.unpack_from('>I', data, off)[0]
def read16(data, off): return struct.unpack_from('>H', data, off)[0]


def get_section_data(raw, section):
    d = raw[section['offset']:section['offset'] + section['size']]
    if section['flags'] & SHF_RPL_ZLIB:
        d = zlib.decompress(d[4:])
    return d


def parse_elf(raw):
    e_shoff     = read32(raw, 0x20)
    e_shentsize = read16(raw, 0x2E)
    e_shnum     = read16(raw, 0x30)
    e_shstrndx  = read16(raw, 0x32)

    def parse_sh(i):
        o = e_shoff + i * e_shentsize
        return {
            'name_off': read32(raw, o),
            'type':     read32(raw, o + 4),
            'flags':    read32(raw, o + 8),
            'addr':     read32(raw, o + 12),
            'offset':   read32(raw, o + 16),
            'size':     read32(raw, o + 20),
            'link':     read32(raw, o + 24),
            'info':     read32(raw, o + 28),
            'entsize':  read32(raw, o + 36),
        }

    sections = [parse_sh(i) for i in range(e_shnum)]
    shstrtab = get_section_data(raw, sections[e_shstrndx])

    def shname(i):
        o = sections[i]['name_off']
        return shstrtab[o:shstrtab.index(b'\x00', o)].decode()

    section_names = [shname(i) for i in range(e_shnum)]
    return sections, section_names


def build_plt_map(rpx_path):
    with open(rpx_path, 'rb') as f:
        raw = f.read()

    sections, snames = parse_elf(raw)

    # Identify import sections
    import_sections = {
        i: snames[i].replace('.fimport_', '').replace('.dimport_', '')
        for i, s in enumerate(sections)
        if snames[i].startswith(('.fimport_', '.dimport_'))
    }

    # Parse symbol table
    symtab_idx = next(i for i, s in enumerate(sections) if s['type'] == SHT_SYMTAB)
    symtab_s = sections[symtab_idx]
    strtab_s = sections[symtab_s['link']]
    symtab   = get_section_data(raw, symtab_s)
    strtab   = get_section_data(raw, strtab_s)

    def str_at(off):
        end = strtab.index(b'\x00', off)
        return strtab[off:end].decode('utf-8', 'replace')

    # Collect import function symbols (those whose st_shndx is an import section)
    SYMSIZE = 16
    import_syms = {}
    for si in range(len(symtab) // SYMSIZE):
        o = si * SYMSIZE
        st_name  = read32(symtab, o)
        st_shndx = read16(symtab, o + 14)
        if st_shndx in import_sections:
            name = str_at(st_name)
            if not name.startswith('.'):
                module = import_sections[st_shndx]
                import_syms[si] = (name, module)

    # Parse .text section
    text_idx  = snames.index('.text')
    text_s    = sections[text_idx]
    text_vaddr = text_s['addr']
    text_data  = get_section_data(raw, text_s)

    # Parse .rela.text
    rela_idx  = snames.index('.rela.text')
    rela_s    = sections[rela_idx]
    rela_data = get_section_data(raw, rela_s)

    # Build PLT map: addr -> (func_name, module)
    plt_map = {}
    conflicts = 0
    for i in range(len(rela_data) // 12):
        o = i * 12
        r_offset = read32(rela_data, o)
        r_info   = read32(rela_data, o + 4)
        sym_idx  = r_info >> 8
        rel_type = r_info & 0xFF
        if rel_type != R_PPC_REL24 or sym_idx not in import_syms:
            continue
        text_off = r_offset - text_vaddr
        if not (0 <= text_off <= len(text_data) - 4):
            continue
        instr = read32(text_data, text_off)
        if (instr >> 26) != 18:  # not a branch instruction
            continue
        li = instr & 0x03FFFFFC
        if li & 0x02000000:
            li -= 0x04000000  # sign-extend 26 bits
        target = (r_offset + li) & 0xFFFFFFFF
        name, module = import_syms[sym_idx]
        key = (name, module)
        if target not in plt_map:
            plt_map[target] = key
        elif plt_map[target] != key:
            conflicts += 1

    if conflicts:
        print(f'Warning: {conflicts} address conflicts (multiple symbols → same stub)',
              file=sys.stderr)

    return plt_map


def macro_name(s):
    """Convert function name to a valid C identifier for ADDR_ macros."""
    return re.sub(r'[^A-Za-z0-9_]', '_', s)


def write_addrs_h(filepath, modules_dict):
    lines = [
        '// AUTO-GENERATED by tools/plt_resolver.py — DO NOT EDIT',
        '// Real PLT stub addresses extracted from the game RPX.',
        '#pragma once',
        '',
    ]
    for mod in sorted(modules_dict.keys()):
        stubs = sorted(modules_dict[mod], key=lambda x: x[0])
        lines.append(f'// --- {mod} ---')
        for addr, name in stubs:
            lines.append(f'#define ADDR_{macro_name(name)} 0x{addr:08X}u')
        lines.append('')
    os.makedirs(os.path.dirname(filepath) or '.', exist_ok=True)
    with open(filepath, 'w') as f:
        f.write('\n'.join(lines))


def write_plt_auto(filepath, plt_map):
    lines = [
        '// AUTO-GENERATED by tools/plt_resolver.py — DO NOT EDIT',
        '// Registers STUB for every PLT stub address so no import call crashes.',
        '// Module-specific register() functions override with real implementations.',
        '#include "os/os_common.h"',
        '#include "runtime/rebrewu_runtime.h"',
        '',
        'void gambit_plt_auto_register(CPUState* cpu) {',
    ]
    for addr in sorted(plt_map.keys()):
        name, module = plt_map[addr]
        lines.append(
            f'    rbrew_register_func(cpu, 0x{addr:08X}u, STUB);'
            f'  // {module}::{name}'
        )
    lines += ['}', '']
    os.makedirs(os.path.dirname(filepath) or '.', exist_ok=True)
    with open(filepath, 'w') as f:
        f.write('\n'.join(lines))


def main():
    parser = argparse.ArgumentParser(description='Wii U RPX PLT Import Resolver')
    parser.add_argument('rpx', help='Path to the RPX file')
    parser.add_argument('--port-dir', default='port',
                        help='Path to the port/ directory (default: port)')
    parser.add_argument('--json', default=None,
                        help='Also write JSON index to this file')
    args = parser.parse_args()

    print(f'Parsing {args.rpx}...')
    plt_map = build_plt_map(args.rpx)

    # Group by module
    by_module = collections.defaultdict(list)
    for addr, (name, module) in plt_map.items():
        by_module[module].append((addr, name))

    print(f'Found {len(plt_map)} unique PLT stubs across {len(by_module)} modules:')
    for mod in sorted(by_module.keys()):
        print(f'  {mod}: {len(by_module[mod])} stubs')

    port = args.port_dir

    # Write per-module addrs.h files
    for mod in DEDICATED_MODULES:
        if mod not in by_module:
            continue
        path = os.path.join(port, 'os', mod, f'{mod}_addrs.h')
        write_addrs_h(path, {mod: by_module[mod]})
        print(f'  Wrote {path}')

    # Write stubs_addrs.h for all remaining modules
    stub_mods = {m: v for m, v in by_module.items() if m not in DEDICATED_MODULES}
    if stub_mods:
        path = os.path.join(port, 'os', 'stubs', 'stubs_addrs.h')
        write_addrs_h(path, stub_mods)
        print(f'  Wrote {path}')

    # Write gambit_plt_auto.cpp
    auto_path = os.path.join(port, 'os', 'gambit_plt_auto.cpp')
    write_plt_auto(auto_path, plt_map)
    print(f'  Wrote {auto_path}')

    # Write full index
    index_path = os.path.join(os.path.dirname(args.rpx), 'plt_full_index.txt')
    with open(index_path, 'w') as f:
        for addr in sorted(plt_map.keys()):
            name, module = plt_map[addr]
            f.write(f'0x{addr:08X}  {module:<20}  {name}\n')
    print(f'  Wrote {index_path}')

    if args.json:
        data = {f'0x{a:08X}': {'module': m, 'name': n}
                for a, (n, m) in plt_map.items()}
        with open(args.json, 'w') as f:
            json.dump(data, f, indent=2, sort_keys=True)
        print(f'  Wrote {args.json}')


if __name__ == '__main__':
    main()
