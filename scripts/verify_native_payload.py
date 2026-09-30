#!/usr/bin/env python3
"""Read native Windows PE resources as data, without executing the installer."""
import argparse
import ctypes
from ctypes import wintypes
from pathlib import Path
import sys


def verify(installer: Path, source: Path) -> None:
    if sys.platform != 'win32':
        raise RuntimeError('Native resource verification requires Windows')
    api = ctypes.WinDLL('kernel32', use_last_error=True)
    api.LoadLibraryExW.argtypes = (wintypes.LPCWSTR, wintypes.HANDLE, wintypes.DWORD)
    api.LoadLibraryExW.restype = wintypes.HMODULE
    api.FindResourceW.argtypes = (wintypes.HMODULE, ctypes.c_void_p, ctypes.c_void_p)
    api.FindResourceW.restype = wintypes.HRSRC
    api.SizeofResource.argtypes = (wintypes.HMODULE, wintypes.HRSRC)
    api.SizeofResource.restype = wintypes.DWORD
    api.LoadResource.argtypes = (wintypes.HMODULE, wintypes.HRSRC)
    api.LoadResource.restype = wintypes.HGLOBAL
    api.LockResource.argtypes = (wintypes.HGLOBAL,)
    api.LockResource.restype = ctypes.c_void_p
    api.FreeLibrary.argtypes = (wintypes.HMODULE,)
    module = api.LoadLibraryExW(str(installer.resolve()), None, 0x00000002)  # LOAD_LIBRARY_AS_DATAFILE
    if not module:
        raise ctypes.WinError(ctypes.get_last_error())
    resources = {102: 'LICENSE', 103: 'SubtitleTranslate - ChatGPT.as', 104: 'SubtitleTranslate - ChatGPT.ico',
                 105: 'SubtitleTranslate - ChatGPT - Without Context.as', 106: 'SubtitleTranslate - ChatGPT - Without Context.ico'}
    try:
        for resource_id, name in resources.items():
            resource = api.FindResourceW(module, resource_id, 10)  # RT_RCDATA
            if not resource:
                raise ctypes.WinError(ctypes.get_last_error())
            size = api.SizeofResource(module, resource)
            pointer = api.LockResource(api.LoadResource(module, resource))
            if not pointer or ctypes.string_at(pointer, size) != (source / name).read_bytes():
                raise ValueError(f'Embedded payload differs from current staged source: {name}')
            print(f'PASS native resource: {name}')
    finally:
        api.FreeLibrary(module)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--installer', type=Path, required=True)
    parser.add_argument('--source-dir', type=Path, required=True)
    args = parser.parse_args()
    verify(args.installer, args.source_dir)
