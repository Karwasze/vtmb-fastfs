#!/bin/sh
# Builds vtmb-fastfs.vtm (a 32-bit Windows DLL without a C runtime) with clang + lld.
# Needs: clang, lld-link, llvm-dlltool, and libdeflate 1.22 source unpacked as ./libdeflate
#   (https://github.com/ebiggers/libdeflate/releases/tag/v1.22).
set -e
cd "$(dirname "$0")"
CF="--target=i686-pc-windows-msvc -O2 -ffreestanding -fno-stack-protector -mno-stack-arg-probe -fno-builtin -Ilibdeflate"
for d in kernel32 user32 winmm; do llvm-dlltool -m i386 -d $d.def -l $d.lib -k; done
mkdir -p obj
for f in deflate_decompress zlib_decompress adler32 utils cpu_features; do
  src=libdeflate/lib/$f.c; [ -f $src ] || src=libdeflate/lib/x86/$f.c
  clang --target=i686-w64-windows-gnu -O2 -DFREESTANDING -ffreestanding -fno-stack-protector -Ilibdeflate -c $src -o obj/ld_$f.obj
done
M="fastfs findcache profiler spewlog watch testharness fastconnect fastparse fastui fastmodels fastinflate fastbones fastseq fadeprobe fadefix texprefetch fastsym"
for f in $M; do clang $CF -c $f.c -o obj/$f.obj; done
lld-link /dll /nodefaultlib /entry:DllMain /machine:x86 /safeseh:no /out:vtmb-fastfs.vtm \
  $(for f in $M; do printf 'obj/%s.obj ' $f; done) obj/ld_*.obj kernel32.lib winmm.lib user32.lib
# The Unofficial Patch loader refuses .vtm files of certain sizes (old plugins it replaces);
# 125440 bytes is one of them. Pad such a build with a zero overlay.
if [ "$(wc -c < vtmb-fastfs.vtm)" -eq 125440 ]; then head -c 512 /dev/zero >> vtmb-fastfs.vtm; fi
ls -l vtmb-fastfs.vtm
