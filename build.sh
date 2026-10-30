#!/bin/bash
# Builds dinput8.dll and MKXAccess.asi with clang + lld-link (no Windows SDK needed).
set -e
cd "$(dirname "$0")"
mkdir -p out
llvm-dlltool -m i386:x86-64 -d src/kernel32.def -l out/kernel32.lib
llvm-dlltool -m i386:x86-64 -d src/user32.def -l out/user32.lib
CF="--target=x86_64-pc-windows-msvc -O2 -fasynchronous-unwind-tables -ffreestanding -fno-builtin -fno-stack-protector -mno-stack-arg-probe -fms-extensions -Wall -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable"
clang $CF -c src/crt.c -o out/crt.o
clang $CF -c src/dinput8.c -o out/dinput8.o
clang $CF -c src/mkxaccess.c -o out/mkxaccess.o
# version details shown in the file's Properties (taken from VERSION in src/mkxaccess.c)
VER=$(sed -n 's/^#define VERSION L"\([0-9.]*\)".*/\1/p' src/mkxaccess.c)
IFS=. read -r MAJ MIN PAT <<< "$VER"; PAT=${PAT:-0}
mkres() { sed -e "s/@VER@/$VER/g; s/@MAJ@/$MAJ/; s/@MIN@/$MIN/; s/@PAT@/$PAT/; s/@MAJ@/$MAJ/; s/@MIN@/$MIN/; s/@PAT@/$PAT/" \
              -e "s|@DESC@|$2|; s/@NAME@/$3/; s/@FILE@/$1/" src/version.rc.in > out/$3.rc
          llvm-rc -no-preprocess /FO out/$3.res out/$3.rc; }
mkres MKXAccess.asi "MKX Access - screen reader support for Mortal Kombat X" MKXAccess
mkres dinput8.dll "MKX Access loader - loads MKX Access into Mortal Kombat X" dinput8
LF="/dll /nodefaultlib /entry:DllMain /subsystem:windows /machine:x64 /opt:ref"
lld-link $LF out/dinput8.o out/crt.o out/dinput8.res out/kernel32.lib /out:out/dinput8.dll
lld-link $LF out/mkxaccess.o out/crt.o out/MKXAccess.res out/kernel32.lib out/user32.lib /out:out/MKXAccess.asi
echo "Built out/dinput8.dll and out/MKXAccess.asi"
