#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds small GOG-style test installers with Inno Setup's compiler (CI, Windows runner):
#   make-installers.sh <path to ISCC.exe> <out_dir>
# The game data is random bytes and generated text, nothing copyrighted. Variants cover what
# GOG installers use: lzma2/lzma/zip/bzip chunks, solid and not, data in setup-N.bin slices
# (one and several slices per disk) or inside the .exe, per-language files, {tmp} and
# {commonappdata} files that must not land in the game folder, and an add-on installer.
set -euo pipefail
export MSYS2_ARG_CONV_EXCL="*"   # Git Bash must not turn ISCC's /Q into a path
ISCC="${1:?ISCC.exe}"
OUT="$(mkdir -p "${2:?out dir}" && cd "$2" && pwd)"
WORK="$(mktemp -d)"
cd "$WORK"

mkdir -p "payload/data/sub dir/deep" payload/bin lang tmpfiles addon/dlc
head -c 1200000 /dev/urandom > payload/data/textures.pak
head -c 900000 /dev/urandom > "payload/data/sub dir/music.ogg"
head -c 300000 /dev/urandom > payload/bin/TestGame.exe
for i in $(seq 1 20000); do echo "line $i of a compressible game script"; done > payload/data/script.txt
for i in $(seq 1 3000); do echo "config $i = $((i * 7))"; done > "payload/data/sub dir/deep/config.ini"
: > payload/data/empty.dat
echo "unicode name" > "payload/data/Café ünï.txt"
echo "english text" > lang/english.txt
echo "deutscher Text" > lang/german.txt
echo "only during setup" > tmpfiles/helper.txt
head -c 200000 /dev/urandom > addon/dlc/expansion.pak
echo "patched by the add-on" > addon/script.txt

# name compression solid spanning slices_per_disk
variant() {
    local name=$1 comp=$2 solid=$3 span=$4 per_disk=$5
    {
        echo "[Setup]"
        echo "AppName=MYIOSDECK Test Game"
        echo "AppVersion=1.0"
        echo "AppPublisher=MYIOSDECK CI"
        echo "DefaultDirName={pf}\\GOG Games\\MYIOSDECK Test Game"
        echo "OutputDir=$(cygpath -w "$OUT")"
        echo "OutputBaseFilename=$name"
        echo "Compression=$comp"
        echo "SolidCompression=$solid"
        echo "Uninstallable=no"
        echo "PrivilegesRequired=lowest"
        echo "DisableProgramGroupPage=yes"
        if [ "$span" = yes ]; then
            echo "DiskSpanning=yes"
            echo "SlicesPerDisk=$per_disk"
            echo "DiskSliceSize=1000000"
        fi
        echo "[Languages]"
        echo "Name: \"english\"; MessagesFile: \"compiler:Default.isl\""
        echo "Name: \"german\"; MessagesFile: \"compiler:Languages\\German.isl\""
        echo "[Dirs]"
        echo "Name: \"{app}\\saves\""
        echo "[Files]"
        echo "Source: \"payload\\*\"; DestDir: \"{app}\"; Flags: recursesubdirs createallsubdirs"
        echo "Source: \"lang\\english.txt\"; DestDir: \"{app}\"; DestName: \"lang.txt\"; Languages: english"
        echo "Source: \"lang\\german.txt\"; DestDir: \"{app}\"; DestName: \"lang.txt\"; Languages: german"
        echo "Source: \"tmpfiles\\helper.txt\"; DestDir: \"{tmp}\"; Flags: deleteafterinstall"
        echo "Source: \"lang\\english.txt\"; DestDir: \"{commonappdata}\\MYIOSDECK Test\"; DestName: \"settings.txt\""
    } > "$name.iss"
    "$ISCC" /Q "$name.iss"
}

variant setup_mid_lzma2 lzma2/max yes yes 1
variant setup_mid_lzma lzma yes yes 3
variant setup_mid_zip zip no yes 1
variant setup_mid_bzip bzip no no 1

# An add-on: more files for the same game folder, one of them replacing a base game file.
cat > setup_mid_dlc.iss <<EOF
[Setup]
AppName=MYIOSDECK Test Game - Expansion
AppVersion=1.0
AppPublisher=MYIOSDECK CI
DefaultDirName={pf}\\GOG Games\\MYIOSDECK Test Game
OutputDir=$(cygpath -w "$OUT")
OutputBaseFilename=setup_mid_dlc
Compression=lzma2
SolidCompression=yes
Uninstallable=no
PrivilegesRequired=lowest
DisableProgramGroupPage=yes
[Files]
Source: "addon\\dlc\\*"; DestDir: "{app}\\dlc"
Source: "addon\\script.txt"; DestDir: "{app}\\data"
EOF
"$ISCC" /Q setup_mid_dlc.iss

ls -la "$OUT"
