#!/usr/bin/env bash
# Remove what install.sh installed for the current user.
set -euo pipefail

DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}
DATA=${MSPAINT_WINE_HOME:-$DATA_HOME/mspaint-wine}
CACHE=${MSPAINT_WINE_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/mspaint-wine}
FILES=(
    "$HOME/.local/bin/mspaint"
    "$DATA_HOME/applications/mspaint-wine.desktop"
    "$DATA_HOME/icons/hicolor/256x256/apps/mspaint-wine.png"
)

echo "This removes:"
echo "  $DATA  (the Wine prefix with Paint, your Paint settings and the portable Wine)"
echo "  $CACHE  (downloads)"
for f in "${FILES[@]}"; do echo "  $f"; done
echo "Images that you saved outside the Wine prefix are not touched."
if [ "${1:-}" != "-y" ]; then
    printf 'Continue? [y/N] '
    read -r answer
    case $answer in [yY]*) ;; *) echo "Cancelled."; exit 1 ;; esac
fi

# Stop the Wine processes of this prefix only.
if [ -x "$DATA/wine/bin/wineserver" ] && [ -d "$DATA/prefix" ]; then
    WINEPREFIX="$DATA/prefix" "$DATA/wine/bin/wineserver" -k 2>/dev/null || true
elif command -v wineserver >/dev/null 2>&1 && [ -d "$DATA/prefix" ]; then
    WINEPREFIX="$DATA/prefix" wineserver -k 2>/dev/null || true
fi

case $DATA in */mspaint-wine) rm -rf -- "$DATA" ;; *) echo "Not removing $DATA (unexpected name)";; esac
case $CACHE in */mspaint-wine) rm -rf -- "$CACHE" ;; *) echo "Not removing $CACHE (unexpected name)";; esac
rm -f -- "${FILES[@]}"
command -v update-desktop-database >/dev/null 2>&1 && update-desktop-database "$DATA_HOME/applications" >/dev/null 2>&1 || true
echo "Paint is uninstalled."
