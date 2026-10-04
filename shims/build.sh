#!/usr/bin/env bash
# Rebuild the shim DLLs in shims/bin/ (they are prebuilt in the repository).
# Needs zig (https://ziglang.org), for example from pip: python3 -m pip install ziglang
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")"
if command -v zig >/dev/null 2>&1; then ZIG=(zig); else ZIG=(python3 -m ziglang); fi
CC=("${ZIG[@]}" cc -target x86_64-windows-gnu -shared -O2 -s)
mkdir -p bin
# Feature-staging API stub, PNG clipboard bridge and EM_FORMATRANGE for the text tool.
"${CC[@]}" -o bin/featstage.dll featstage.c pclip.c -lole32 -luser32 -lgdi32
# user32 proxy for uiribbon.dll: the missing ordinals, everything else is forwarded.
"${CC[@]}" -nostdlib -o bin/U32SHM.dll u32shm.c u32shm.def -luser32 -lkernel32
# Bcp47Langs.dll!IsTransientLcid for fms.dll (Wine 9.0 does not have Bcp47Langs.dll).
"${CC[@]}" -nostdlib -o bin/bcp47langs.dll bcp47langs.c
ls -l bin
