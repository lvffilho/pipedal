#!/usr/bin/env bash
# Copyright (c) Robin E.R. Davies
#
# Permission is hereby granted, free of charge, to any person obtaining a copy of
# this software and associated documentation files (the "Software"), to deal in
# the Software without restriction, including without limitation the rights to
# use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of
# the Software, and to permit persons to whom the Software is furnished to do so,
# subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
# FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
# COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
# IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
# CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

# install-mod-guis.sh -- add MOD GUIs to LV2 plugins installed from distro packages.
#
# MOD Devices publishes MOD GUIs for many plugins that Debian/Ubuntu package without
# one (Guitarix, x42, ZAM, Dragonfly, DPF, ...) in github.com/moddevices/mod-lv2-data.
# For each installed plugin without a working MOD GUI, this script looks for one
# there (pinned to a tested commit; Guitarix plugins also fall back to the matching
# Guitarix 0.47.0 release), and installs it as an overlay bundle:
#
#     /usr/local/lib/lv2/<bundle>-modgui.lv2/
#         manifest.ttl    <plugin-uri> rdfs:seeAlso <modgui.ttl> . (for each plugin)
#         modgui.ttl      only the plugin's modgui:gui description
#         modgui/         the GUI's resources (templates, stylesheets, images)
#
# MOD's plugin descriptions and binaries are never installed, and nothing under
# /usr/lib/lv2 (owned by dpkg) is modified. MOD's GUIs were written for MOD's
# builds of the plugins, so a GUI is only installed if every control, port and
# parameter it uses exists in the installed plugin; the others are listed with the
# reason. A GUI that uses a port under another name than the installed plugin
# (a typo, or a port renamed since MOD's build) is adapted in the overlay: the port
# is mapped to the installed one (tools/mod-gui-port-aliases.txt lists the known
# ones; others only when the match is unambiguous), or else its control is hidden.
# Restart PiPedal afterwards (sudo systemctl restart pipedald).
#
# Usage:
#     tools/install-mod-guis.sh [--dry-run]       install or update the overlays
#     tools/install-mod-guis.sh --install-plugins also install the distro packages
#                                                 of plugins MOD has GUIs for
#     tools/install-mod-guis.sh --uninstall       remove the overlays
#
# Options:
#     --dry-run        Show what would be done; nothing is installed or written to
#                      the destination. (Still downloads the GUI sources into a
#                      temporary directory to check them.)
#     --install-plugins[=PKG,...]
#                      First install (apt-get install --no-install-recommends) the
#                      packages from the distribution's archive that contain
#                      plugins MOD has GUIs for (or just the packages listed), then
#                      install the GUIs.
#     --uninstall      Remove every overlay installed by this script (and by the
#                      older install-gx-modgui.sh).
#     --dest DIR       Install overlays in DIR (default /usr/local/lib/lv2). Root is
#                      only needed when DIR is not writable.
#     --lv2-dir DIR    Scan DIR for installed plugins (default /usr/lib/lv2). May be
#                      repeated. Plugin bundles in --dest are scanned too.
#     --source DIR     Use a local mod-lv2-data checkout instead of downloading.
#     --guitarix-source DIR
#                      Use a local guitarix source checkout instead of downloading.
#     --port-aliases FILE
#                      Use FILE instead of tools/mod-gui-port-aliases.txt.
#     -h, --help       Show this help.
#
# Requires git, python3 and python3-lilv (sudo apt install git python3-lilv).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PYTHON=/usr/bin/python3
ORIGINAL_ARGS=("$@")

usage() {
    sed -n '/^# install-mod-guis.sh/,/^$/p' "$0" | sed 's/^# \{0,1\}//'
}

die() {
    echo "Error: $*" >&2
    exit 1
}

DEST_DIR=/usr/local/lib/lv2
DRY_RUN=0
NEEDS_ROOT=0
while [ $# -gt 0 ]; do
    case "$1" in
        -h|--help) usage; exit 0 ;;
        --dry-run) DRY_RUN=1 ;;
        --install-plugins|--install-plugins=*) NEEDS_ROOT=1 ;;
        --dest) [ $# -ge 2 ] || die "--dest requires a directory."; DEST_DIR="$2"; shift ;;
        --dest=*) DEST_DIR="${1#--dest=}" ;;
        --lv2-dir|--source|--guitarix-source|--port-aliases) [ $# -ge 2 ] || die "$1 requires an argument."; shift ;;
        --lv2-dir=*|--source=*|--guitarix-source=*|--port-aliases=*|--uninstall) ;;
        *) usage >&2; die "Unknown option: $1" ;;
    esac
    shift
done

[ -x "$PYTHON" ] || die "python3 is required (sudo apt install python3 python3-lilv)."
"$PYTHON" -c 'import lilv' 2>/dev/null || die "python3-lilv is required (sudo apt install python3-lilv)."

# Root is needed to install packages, or to write the destination.
if [ "$DRY_RUN" -eq 0 ] && [ "$(id -u)" -ne 0 ]; then
    probe="$DEST_DIR"
    while [ ! -e "$probe" ]; do probe="$(dirname "$probe")"; done
    if [ ! -w "$probe" ]; then
        NEEDS_ROOT=1
    fi
    if [ "$NEEDS_ROOT" -eq 1 ]; then
        if command -v sudo >/dev/null 2>&1; then
            echo "This requires root; re-running with sudo."
            exec sudo -- bash "$0" "${ORIGINAL_ARGS[@]}"
        fi
        die "Run this script as root."
    fi
fi

exec "$PYTHON" "$SCRIPT_DIR/mod_guis.py" "${ORIGINAL_ARGS[@]}"
