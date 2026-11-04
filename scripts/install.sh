#!/bin/sh
# Installs the latest KnockThru release on a Steam Frame, without a PC.
# In Desktop mode, open Konsole and run:
#   curl -fsSL https://github.com/VioletTides/KnockThru/releases/latest/download/install.sh | sh
#
# It downloads the release ZIP named in the FrameDrop manifest, checks its
# SHA-256, unpacks it to ~/KnockThru, adds KnockThru to the Steam library, and
# starts it if SteamVR is running.
#
# KNOCKTHRU_DIR sets the install folder. KNOCKTHRU_RELEASE_URL sets the folder
# that holds knockthru.framedrop.json, for testing a release before it ships.
set -eu

# Everything runs from main, so a download cut short by curl | sh runs nothing.
main() {
    release_url=${KNOCKTHRU_RELEASE_URL:-https://github.com/VioletTides/KnockThru/releases/latest/download}
    dir=${KNOCKTHRU_DIR:-$HOME/KnockThru}

    fail() {
        echo "KnockThru was not installed: $1" >&2
        exit 1
    }

    [ "$(uname -s)" = Linux ] && [ "$(uname -m)" = aarch64 ] || fail "this script runs on the Steam Frame only."
    [ "$(id -u)" != 0 ] || fail "run it as your own user, not root."
    for tool in curl python3 sha256sum unzip; do
        command -v "$tool" >/dev/null 2>&1 || fail "$tool is missing."
    done

    # Only a test release folder on disk may use file://. Redirects always stay on https.
    case $release_url in
        file://*) proto='=https,file' schemes='https|file' ;;
        *) proto='=https' schemes=https ;;
    esac

    work=$(mktemp -d)
    trap 'rm -rf "$work"' EXIT

    echo "Finding the latest release"
    curl -fsSL --proto "$proto" --proto-redir =https "$release_url/knockthru.framedrop.json" -o "$work/manifest.json" || fail "cannot download $release_url/knockthru.framedrop.json"
    python3 - "$work/manifest.json" "$schemes" >"$work/file" <<'EOF' || fail "the release manifest is not valid."
import json, re, sys
entry = json.load(open(sys.argv[1]))["files"][0]
url, sha256 = entry["url"], entry["sha256"]
if not re.match("(" + sys.argv[2] + ")://", url) or not re.search(r"/knockthru-[0-9][0-9A-Za-z.-]*-linux-arm64\.zip$", url):
    sys.exit(1)
if not re.fullmatch("[0-9a-f]{64}", sha256):
    sys.exit(1)
print(url)
print(sha256)
EOF
    zip_url=$(sed -n 1p "$work/file")
    sha256=$(sed -n 2p "$work/file")

    echo "Downloading $zip_url"
    curl -fL --proto "$proto" --proto-redir =https --progress-bar "$zip_url" -o "$work/knockthru.zip" || fail "the download failed."
    echo "$sha256  $work/knockthru.zip" | sha256sum -c --status || fail "the ZIP does not match the SHA-256 in the manifest."

    # Unpack next to the install folder, then swap the folders. A running KnockThru
    # keeps its old files open until --install below restarts it.
    rm -rf "$dir.new" "$dir.old"
    mkdir -p "$dir.new"
    unzip -q "$work/knockthru.zip" -d "$dir.new" || fail "the ZIP did not unpack."
    [ -x "$dir.new/knockthru" ] || fail "the ZIP has no knockthru program."
    if [ -e "$dir" ]; then mv "$dir" "$dir.old"; fi
    if ! mv "$dir.new" "$dir"; then
        if [ -e "$dir.old" ]; then mv "$dir.old" "$dir"; fi
        fail "the new files could not replace $dir. The old install is back."
    fi
    rm -rf "$dir.old"
    echo "Installed to $dir"

    # Add to Steam once, so KnockThru shows under Library > Non-Steam.
    added=no
    for shortcuts in "$HOME"/.local/share/Steam/userdata/*/config/shortcuts.vdf; do
        if [ -f "$shortcuts" ] && grep -qF "$dir/knockthru" "$shortcuts"; then added=yes; fi
    done
    if [ "$added" = no ] && command -v steamos-add-to-steam >/dev/null 2>&1; then
        if steamos-add-to-steam "$dir/knockthru" >/dev/null 2>&1; then
            echo "Added KnockThru to your Steam library under Non-Steam"
        fi
    fi

    if "$dir/knockthru" --install; then
        exit 0
    fi
    echo
    echo "The files are in place, but SteamVR did not answer."
    echo "Go back to Gaming mode and launch KnockThru once from Library > Non-Steam."
}

main "$@"
