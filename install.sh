#!/usr/bin/env bash
# Install Windows 10 Paint (the ribbon version) under Wine, for the current user.
#
# No Microsoft file is part of this project. By default the installer downloads
# the Windows 10 image from Microsoft's own servers (the same source that the
# Media Creation Tool uses), takes the few files that Paint needs, and patches
# them so that they run under Wine. See README.md.
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
DATA=${MSPAINT_WINE_HOME:-$DATA_HOME/mspaint-wine}
CACHE=${MSPAINT_WINE_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/mspaint-wine}
BIN_DIR=$HOME/.local/bin
APPS_DIR=$DATA_HOME/applications
ICON_DIR=$DATA_HOME/icons/hicolor/256x256/apps
PREFIX=$DATA/prefix

# Portable Wine build (https://github.com/Kron4ek/Wine-Builds). A WoW64 build
# runs without 32-bit libraries on the host.
WINE_VERSION=${MSPAINT_WINE_VERSION:-10.0}
WINE_NAME=wine-$WINE_VERSION-amd64-wow64
WINE_URL=${MSPAINT_WINE_URL:-https://github.com/Kron4ek/Wine-Builds/releases/download/$WINE_VERSION/$WINE_NAME.tar.xz}
WINE_SHA256=${MSPAINT_WINE_SHA256:-aeebbbf239e548f0136f1cd72a2e109dd9a572a8f703da3c09fa40d74d5c255f}

# Official 7-Zip for Linux (reads ISO, WIM and ESD files). Used when 7zz is not installed.
SEVENZIP_URL=https://github.com/ip7z/7zip/releases/download/26.03/7z2603-linux-x64.tar.xz
SEVENZIP_SHA256=dc99eff5008f1ab79bd7084c68513701547a808a89502bf4133683535ab3c695

# Windows 10 Media Creation Tool catalog (HTTPS, Microsoft). It lists the ESD images.
PRODUCTS_CAB_URL=https://go.microsoft.com/fwlink/?LinkId=841361

usage() {
    cat <<EOF
Usage: $0 [options]

Installs Windows 10 Paint for the current user. With no options, it downloads
a portable Wine build (about 75 MB) and the Windows 10 image from Microsoft
(about 4.2 GB, deleted after the install).

Options:
  --from PATH       Take the Windows files from PATH instead of downloading
                    them. PATH can be a Windows 10 .iso, an install.wim or
                    install.esd file, a mounted Windows 10 drive, or its
                    Windows folder.
  --lang CODE       Language of the downloaded image, for example en-us,
                    de-de, fr-fr (default: en-us).
  --system-wine     Use the Wine that is installed on the system.
  --fonts SET       Windows fonts to install: default, all or none
                    (default: default, a set of about 90 common fonts).
  --pictures        Open and Save As start in Pictures (as on Windows),
                    not in Downloads.
  --keep-download   Keep the downloaded Windows image in $CACHE.
  -y, --yes         Do not ask before the download.
  -h, --help        Show this help.
EOF
}

FROM= LANG_CODE=en-us SYSTEM_WINE=0 FONTS=default PICTURES=0 KEEP=0 YES=0
while [ $# -gt 0 ]; do
    case $1 in
        --from) FROM=${2:?--from needs a path}; shift ;;
        --lang) LANG_CODE=$(echo "${2:?--lang needs a code}" | tr 'A-Z' 'a-z'); shift ;;
        --system-wine) SYSTEM_WINE=1 ;;
        --fonts) FONTS=${2:?--fonts needs a value}; shift ;;
        --pictures) PICTURES=1 ;;
        --keep-download) KEEP=1 ;;
        -y|--yes) YES=1 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done
case $FONTS in default|all|none) ;; *) echo "--fonts must be default, all or none" >&2; exit 2 ;; esac

say() { printf '\033[1m==>\033[0m %s\n' "$*"; }
die() { printf '\033[31merror:\033[0m %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "$1 is not installed. ${2:-}"; }

[ "$(uname -m)" = x86_64 ] || die "only x86_64 is supported (Paint is a 64-bit Windows program)"
need python3 "Install the python3 package of your distribution."
need tar
if command -v curl >/dev/null 2>&1; then
    fetch() { curl -fL --retry 3 -C - -o "$1" "$2"; }
    fetch_quiet() { curl -fsSL --retry 3 -o "$1" "$2"; }
elif command -v wget >/dev/null 2>&1; then
    fetch() { wget -c -O "$1" "$2"; }
    fetch_quiet() { wget -q -O "$1" "$2"; }
else
    die "curl or wget is necessary"
fi

# download URL FILE ALGORITHM CHECKSUM: download (or resume) and verify.
# Returns 2 when the download fails (the partial file is kept for a resume)
# and 1 when the checksum is wrong (the file is emptied for a new start).
download() {
    if [ -f "$2" ] && check_sum "$3" "$2" "$4"; then return 0; fi
    fetch "$2" "$1" || return 2
    if ! check_sum "$3" "$2" "$4"; then
        : > "$2"
        return 1
    fi
}
download_or_die() { # what URL FILE ALGORITHM CHECKSUM
    local what=$1 rc=0
    shift
    download "$@" || rc=$?
    case $rc in
        0) ;;
        1) die "the $what download is damaged (checksum mismatch). Run the installer again." ;;
        *) die "the $what download failed. Run the installer again to resume it." ;;
    esac
}

check_sum() { # algorithm file expected
    local got
    got=$(python3 -c 'import hashlib,sys
h=hashlib.new(sys.argv[1])
with open(sys.argv[2],"rb") as f:
    for b in iter(lambda: f.read(1<<20), b""): h.update(b)
print(h.hexdigest())' "$1" "$2")
    [ "$got" = "$3" ]
}

untar_xz() { # archive destination
    if command -v xz >/dev/null 2>&1; then
        tar -xJf "$1" -C "$2"
    else
        python3 -c 'import sys,tarfile
t=tarfile.open(sys.argv[1])
t.extractall(sys.argv[2], **({"filter": "tar"} if hasattr(tarfile, "tar_filter") else {}))' "$1" "$2"
    fi
}

# Paint from this install must not run while its files are replaced.
for pid in $(pgrep -x mspaint.exe 2>/dev/null || true); do
    if tr '\0' '\n' < "/proc/$pid/environ" 2>/dev/null | grep -qxF "WINEPREFIX=$PREFIX"; then
        die "Paint is open. Close Paint, then run the installer again."
    fi
done

mkdir -p "$DATA" "$CACHE"
WORK=$(mktemp -d "$CACHE/work.XXXXXX")
DOWNLOADED_IMAGE=
cleanup() {
    # Only remove what this run made: the work folder and, unless --keep-download, the image.
    rm -rf -- "$WORK"
    if [ -n "$DOWNLOADED_IMAGE" ] && [ "$KEEP" = 0 ] && [ "${INSTALL_OK:-0}" = 1 ]; then
        rm -f -- "$DOWNLOADED_IMAGE"
    fi
}
trap cleanup EXIT

# ------------------------------------------------------------------ Wine --
if [ "$SYSTEM_WINE" = 1 ]; then
    need wine "Install Wine from your distribution, or run without --system-wine."
    WINE=$(command -v wine)
    WINESERVER=$(command -v wineserver || true)
    [ -n "$WINESERVER" ] || WINESERVER=$(dirname "$(readlink -f "$WINE")")/wineserver
    say "Using system $("$WINE" --version)"
else
    if [ ! -x "$DATA/wine/bin/wine" ] || [ "$(cat "$DATA/wine/.version" 2>/dev/null)" != "$WINE_NAME" ]; then
        say "Getting Wine $WINE_VERSION (portable build)"
        download_or_die Wine "$WINE_URL" "$CACHE/$WINE_NAME.tar.xz" sha256 "$WINE_SHA256"
        untar_xz "$CACHE/$WINE_NAME.tar.xz" "$WORK"
        [ ! -e "$DATA/wine" ] || mv "$DATA/wine" "$WORK/old-wine"
        mv "$WORK/$WINE_NAME" "$DATA/wine"
        echo "$WINE_NAME" > "$DATA/wine/.version"
        rm -f -- "$CACHE/$WINE_NAME.tar.xz"
    fi
    WINE=$DATA/wine/bin/wine
    WINESERVER=$DATA/wine/bin/wineserver
    say "Using $("$WINE" --version) from $DATA/wine"
fi

# ----------------------------------------------------------------- 7-Zip --
sevenzip() {
    if command -v 7zz >/dev/null 2>&1; then SEVENZ=$(command -v 7zz); return; fi
    if [ ! -x "$CACHE/7zip/7zz" ]; then
        say "Downloading 7-Zip"
        mkdir -p "$CACHE/7zip"
        fetch_quiet "$WORK/7z.tar.xz" "$SEVENZIP_URL" || die "the 7-Zip download failed"
        check_sum sha256 "$WORK/7z.tar.xz" "$SEVENZIP_SHA256" || die "the 7-Zip download is damaged (checksum mismatch)"
        untar_xz "$WORK/7z.tar.xz" "$CACHE/7zip"
    fi
    SEVENZ=$CACHE/7zip/7zz
}

# Paths inside a Windows image (relative to the image root).
IMAGE_FILES=(
    Windows/System32/mspaint.exe Windows/SystemResources/mspaint.exe.mun
    Windows/System32/UIRibbon.dll Windows/System32/UIRibbonRes.dll
    Windows/System32/mfc42u.dll Windows/System32/DismApi.dll Windows/System32/fms.dll
    'Windows/System32/*/mspaint.exe.mui' 'Windows/System32/*/UIRibbon.dll.mui'
    'Windows/System32/*/fms.dll.mui'
)

# extract_image WIM_OR_ESD DEST: extract the Paint files (and fonts) of the
# first Windows edition in the image. Prints the folder that has Windows/.
extract_image() {
    local img=$1 dest=$2 xml index
    sevenzip
    xml=$("$SEVENZ" x -so "$img" '[1].xml' 2>/dev/null | python3 -c 'import sys
d=sys.stdin.buffer.read()
print(d.decode("utf-16") if d[:2] in (b"\xff\xfe", b"\xfe\xff") else d.decode("utf-8", "replace"))' || true)
    # The first image with an edition that is not Windows PE or Setup.
    index=$(printf '%s' "$xml" | python3 -c 'import re,sys
x=sys.stdin.read()
for m in re.finditer(r"<IMAGE INDEX=\"(\d+)\">(.*?)</IMAGE>", x, re.S):
    e=re.search(r"<EDITIONID>([^<]*)", m.group(2))
    if e and e.group(1) != "WindowsPE":
        print(m.group(1)); break')
    local patterns=() p
    for p in "${IMAGE_FILES[@]}"; do patterns+=("${index:+$index/}$p"); done
    case $FONTS in
        default|all) patterns+=("${index:+$index/}Windows/Fonts/*.ttf" "${index:+$index/}Windows/Fonts/*.ttc") ;;
    esac
    "$SEVENZ" x -y -bso0 -bsp0 -ssc- -o"$dest" "$img" "${patterns[@]}" >/dev/null 2>&1 || true
    if [ -n "$index" ] && [ -d "$dest/$index" ]; then echo "$dest/$index"; return; fi
    # A single-image WIM has no index folder.
    "$SEVENZ" x -y -bso0 -bsp0 -ssc- -o"$dest" "$img" "${patterns[@]#"$index/"}" >/dev/null 2>&1 || true
    echo "$dest"
}

# ------------------------------------------------------- Windows files --
SRC=$WORK/src
mkdir -p "$SRC"
gather() { python3 "$HERE/lib/mspaint_patch.py" gather "$1" "$SRC" --fonts "$FONTS"; }

if [ -z "$FROM" ]; then
    sevenzip
    say "Getting the Windows 10 download list from Microsoft"
    fetch_quiet "$WORK/products.cab" "$PRODUCTS_CAB_URL"
    "$SEVENZ" x -y -bso0 -bsp0 -o"$WORK" "$WORK/products.cab" products.xml >/dev/null
    read -r ESD_URL ESD_SHA1 ESD_SIZE ESD_NAME < <(python3 - "$WORK/products.xml" "$LANG_CODE" <<'EOF'
import sys, xml.etree.ElementTree as ET
root = ET.parse(sys.argv[1]).getroot()
for f in root.iter("File"):
    d = {c.tag: (c.text or "") for c in f}
    if (d.get("LanguageCode", "").lower() == sys.argv[2] and d.get("Architecture") == "x64"
            and "CLIENTCONSUMER" in d.get("FileName", "")):
        print(d["FilePath"], d["Sha1"], d["Size"], d["FileName"])
        break
EOF
)
    [ -n "${ESD_URL:-}" ] || die "no Windows 10 image for language '$LANG_CODE' in Microsoft's list"
    IMAGE=$CACHE/$ESD_NAME
    if [ -f "$IMAGE" ] && [ "$(stat -c %s "$IMAGE")" = "$ESD_SIZE" ] && check_sum sha1 "$IMAGE" "$ESD_SHA1"; then
        say "Using the Windows 10 image that is already downloaded"
    else
        GB=$(python3 -c "print(f'{$ESD_SIZE/1e9:.1f}')")
        if [ "$YES" = 0 ] && [ -t 0 ]; then
            printf 'The installer will now download the Windows 10 image from Microsoft (%s GB)\n' "$GB"
            printf 'into %s. Only the Paint files are kept. Continue? [Y/n] ' "$CACHE"
            read -r answer
            case $answer in [nN]*) die "cancelled" ;; esac
        fi
        say "Downloading $ESD_NAME ($GB GB) from Microsoft"
        # dl.delivery.mp.microsoft.com serves only HTTP. The SHA-1 from the HTTPS catalog checks the file.
        download_or_die "Windows 10 image" "$ESD_URL" "$IMAGE" sha1 "$ESD_SHA1"
    fi
    DOWNLOADED_IMAGE=$IMAGE
    say "Extracting the Paint files"
    gather "$(extract_image "$IMAGE" "$WORK/image")"
elif [ -d "$FROM" ]; then
    say "Copying the Paint files from $FROM"
    gather "$FROM"
elif [ -f "$FROM" ]; then
    case ${FROM,,} in
        *.iso)
            sevenzip
            say "Extracting the Windows image from $FROM"
            "$SEVENZ" x -y -bso0 -bsp0 -ssc- -o"$WORK/iso" "$FROM" sources/install.wim sources/install.esd >/dev/null || true
            WIM=$(ls "$WORK"/iso/sources/install.* 2>/dev/null | head -n 1)
            [ -n "$WIM" ] || die "no sources/install.wim or install.esd in $FROM"
            say "Extracting the Paint files"
            gather "$(extract_image "$WIM" "$WORK/image")"
            ;;
        *.wim|*.esd|*.swm)
            say "Extracting the Paint files from $FROM"
            gather "$(extract_image "$FROM" "$WORK/image")"
            ;;
        *) die "$FROM is not an .iso, .wim or .esd file" ;;
    esac
else
    die "$FROM does not exist"
fi

say "Patching the files for Wine"
OUT=$WORK/out
BUILD_ARGS=()
[ "$PICTURES" = 1 ] && BUILD_ARGS+=(--pictures)
python3 "$HERE/lib/mspaint_patch.py" build "$SRC" "$OUT" "${BUILD_ARGS[@]}"

# ---------------------------------------------------------- Wine prefix --
export WINEPREFIX=$PREFIX WINEDEBUG=-all
if [ ! -f "$PREFIX/system.reg" ]; then
    say "Creating the Wine prefix in $PREFIX"
    # mscoree/mshtml off: Paint does not need Mono or Gecko, so Wine does not ask to install them.
    WINEDLLOVERRIDES="mscoree,mshtml=" WINEARCH=win64 "$WINE" wineboot -i >/dev/null 2>&1
    "$WINESERVER" -w
fi
[ -d "$PREFIX/drive_c/windows/system32" ] || die "the Wine prefix was not created"

say "Installing the files"
C=$PREFIX/drive_c
mkdir -p "$C/mspaint"
cp -f "$OUT"/app/* "$C/mspaint/"
cp -f "$HERE/shims/bin/featstage.dll" "$C/mspaint/"
cp -f "$OUT"/system32/* "$HERE/shims/bin/U32SHM.dll" "$HERE/shims/bin/bcp47langs.dll" "$C/windows/system32/"

REG=$WORK/mspaint.reg
cat > "$REG" <<'EOF'
REGEDIT4

[HKEY_CLASSES_ROOT\CLSID\{32665929-d77e-4ab5-8c08-fbf409b8a233}]
@="UIRibbon"

[HKEY_CLASSES_ROOT\CLSID\{32665929-d77e-4ab5-8c08-fbf409b8a233}\InprocServer32]
@="C:\\windows\\system32\\uiribbon.dll"
"ThreadingModel"="Apartment"

[HKEY_CURRENT_USER\Software\Wine\AppDefaults\mspaint.exe\DllOverrides]
"uiribbon"="native"
EOF
"$WINE" reg import "$REG" /reg:64 >/dev/null 2>&1 || die "registry import failed"

if [ -d "$SRC/fonts" ] && [ -n "$(ls -A "$SRC/fonts")" ]; then
    cp -f "$SRC"/fonts/* "$C/windows/Fonts/"
    python3 "$HERE/lib/mspaint_patch.py" fontreg "$SRC/fonts" "$WORK/fonts.reg" >/dev/null
    "$WINE" reg import "$WORK/fonts.reg" /reg:64 >/dev/null 2>&1 || die "font registration failed"
fi
"$WINESERVER" -w

# ------------------------------------------------- launcher and menu --
say "Adding the launcher and the menu entry"
mkdir -p "$BIN_DIR" "$APPS_DIR" "$ICON_DIR"
LAUNCHER=$BIN_DIR/mspaint
sed -e "s|@PREFIX@|$PREFIX|" -e "s|@WINE@|$WINE|" "$HERE/lib/mspaint.in" > "$LAUNCHER"
chmod +x "$LAUNCHER"
[ -f "$OUT/mspaint.png" ] && cp -f "$OUT/mspaint.png" "$ICON_DIR/mspaint-wine.png"
sed -e "s|@LAUNCHER@|$LAUNCHER|" "$HERE/lib/mspaint-wine.desktop.in" > "$APPS_DIR/mspaint-wine.desktop"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$APPS_DIR" >/dev/null 2>&1 || true
command -v gtk-update-icon-cache >/dev/null 2>&1 && gtk-update-icon-cache -q "$DATA_HOME/icons/hicolor" >/dev/null 2>&1 || true

INSTALL_OK=1
say "Done. Start Paint from the application menu, or run: $LAUNCHER [file]"
case ":$PATH:" in *":$BIN_DIR:"*) ;; *) echo "    ($BIN_DIR is not in your PATH.)" ;; esac
command -v xclip >/dev/null 2>&1 || echo "    Install xclip to keep a copied image on the clipboard after Paint closes."
