#!/usr/bin/env python3
"""Host address -> function and source line, from build/mtm.pdb (dbghelp).

    py -3 tools/addr2line.py 0x6016C82D [more addresses]

A fault report prints the host PC (`=== fault ... at 0x6016C82D`); this names
the lifted function and the line of generated C it was on.
"""
import ctypes
import ctypes.wintypes as W
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build-farm', 'mtm.exe')
BASE = 0x60000000                               # /BASE in CMakeLists.txt

dbg = ctypes.windll.dbghelp
proc = W.HANDLE(0x1234)                         # any unique value: no live process needed
dbg.SymSetOptions(0x2 | 0x10)                   # UNDNAME | LOAD_LINES
dbg.SymInitialize(proc, os.path.dirname(EXE).encode(), False)
dbg.SymLoadModuleEx.restype = ctypes.c_uint64
dbg.SymLoadModuleEx(proc, None, EXE.encode(), None, ctypes.c_uint64(BASE), 0, None, 0)


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [('SizeOfStruct', W.ULONG), ('TypeIndex', W.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', W.ULONG), ('Size', W.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', W.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', W.ULONG),
                ('Scope', W.ULONG), ('Tag', W.ULONG), ('NameLen', W.ULONG), ('MaxNameLen', W.ULONG),
                ('Name', ctypes.c_char * 256)]


class LINE64(ctypes.Structure):
    _fields_ = [('SizeOfStruct', W.DWORD), ('Key', ctypes.c_void_p), ('LineNumber', W.DWORD),
                ('FileName', ctypes.c_char_p), ('Address', ctypes.c_uint64)]


for a in sys.argv[1:]:
    addr = int(a, 16)
    s = SYMBOL_INFO(SizeOfStruct=ctypes.sizeof(SYMBOL_INFO) - 256 + 1, MaxNameLen=255)
    disp = ctypes.c_uint64()
    name = s.Name.decode() if dbg.SymFromAddr(proc, ctypes.c_uint64(addr), ctypes.byref(disp), ctypes.byref(s)) else '?'
    ln, d32 = LINE64(SizeOfStruct=ctypes.sizeof(LINE64)), W.DWORD()
    where = ('%s:%d' % (ln.FileName.decode(), ln.LineNumber)
             if dbg.SymGetLineFromAddr64(proc, ctypes.c_uint64(addr), ctypes.byref(d32), ctypes.byref(ln)) else '')
    print('0x%08X  %s+0x%X  %s' % (addr, name, disp.value, where))
