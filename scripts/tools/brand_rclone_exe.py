"""Gives the bundled rclone.exe Motiva's icon and version strings (Explorer, Task Manager, taskbar).

Only the PE resources change (RT_ICON / RT_GROUP_ICON / RT_VERSION); the code is untouched.
rclone's copyright line is kept on purpose (MIT licence). Idempotent.
Usage:  python -I brand_rclone_exe.py Assets/application/motiva.ico rclone.exe [...]
"""
import ctypes
import struct
import sys
from ctypes import wintypes as w

RT_ICON, RT_GROUP_ICON, RT_VERSION = 3, 14, 16
OLD_ICON_IDS = range(2, 10)

STRINGS = [
    ('CompanyName', 'Motiva'),
    ('FileDescription', 'Motiva cloud helper'),
    ('FileVersion', '1.75.2'),
    ('InternalName', 'motiva-cloud'),
    ('LegalCopyright', 'Based on rclone (MIT), The Rclone Authors'),
    ('OriginalFilename', 'rclone.exe'),
    ('ProductName', 'Motiva'),
    ('ProductVersion', '1.75.2'),
]


def u16(s):
    return s.encode('utf-16le') + b'\0\0'


def block(key, value=b'', children=b'', text=False):
    """One VS_VERSION_INFO node: length, value length, type, key, 4-byte aligned value and children."""
    data = struct.pack('<HH', len(value) // 2 if text else len(value), 1 if text else 0) + u16(key)
    data += bytes(1) * (-(2 + len(data)) % 4)
    data += value
    if children:
        data += bytes(1) * (-(2 + len(data)) % 4) + children
    return struct.pack('<H', len(data) + 2) + data + bytes(1) * (-(len(data) + 2) % 4)


def version_resource():
    strs = b''.join(block(k, u16(v), text=True) for k, v in STRINGS)
    table = block('040904b0', children=strs)
    sfi = block('StringFileInfo', children=table)
    var = block('Translation', struct.pack('<HH', 0x409, 0x4b0))
    vfi = block('VarFileInfo', children=var)
    fixed = struct.pack('<IIIIIIIIIIIII', 0xFEEF04BD, 0x10000, 0x10001, 0x4B0000, 0, 0x10001, 0x4B0000,
                        0x3F, 0, 0x4, 1, 0, 0)
    # fixed: signature, strucver, filever MS/LS, productver MS/LS, mask, flags, OS, type, subtype, date
    return block('VS_VERSION_INFO', fixed, sfi + vfi)


def main(ico_path, exes):
    ico = open(ico_path, 'rb').read()
    _, _, count = struct.unpack('<HHH', ico[:6])
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.BeginUpdateResourceW.restype = w.HANDLE
    k.BeginUpdateResourceW.argtypes = [w.LPCWSTR, w.BOOL]
    k.UpdateResourceW.argtypes = [w.HANDLE, ctypes.c_void_p, ctypes.c_void_p, w.WORD, ctypes.c_void_p, w.DWORD]
    LANG = 1033
    k.EndUpdateResourceW.argtypes = [w.HANDLE, w.BOOL]
    for exe in exes:
        h = k.BeginUpdateResourceW(exe, False)
        if not h:
            sys.exit('cannot open %s (running?)' % exe)

        def put(ty, name, data):
            buf = ctypes.create_string_buffer(data, len(data)) if data else None
            if not k.UpdateResourceW(h, ty, name, LANG, buf, len(data) if data else 0):
                sys.exit('UpdateResource failed %d (type %s id %s)' % (ctypes.get_last_error(), ty, name))

        for i in OLD_ICON_IDS:
            put(RT_ICON, i, b'')
        group = struct.pack('<HHH', 0, 1, count)
        for n in range(count):
            wd, ht, cc, rs, pl, bc, size, off = struct.unpack('<BBBBHHII', ico[6 + 16 * n:22 + 16 * n])
            put(RT_ICON, 2 + n, ico[off:off + size])
            group += struct.pack('<BBBBHHIH', wd, ht, cc, rs, pl, bc, size, 2 + n)
        put(RT_GROUP_ICON, 1, group)
        put(RT_VERSION, 1, version_resource())
        if not k.EndUpdateResourceW(h, False):
            sys.exit('EndUpdateResource failed %d' % ctypes.get_last_error())
        print(exe, ': branded')


main(sys.argv[1], sys.argv[2:])
