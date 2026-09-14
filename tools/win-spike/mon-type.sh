#!/usr/bin/env bash
# mon-type.sh - type an ASCII string into a QEMU guest through the monitor.
#
# The rig drives WinPE's Shift+F10 console, which is the only way to read a
# log file out of a RAM disk that never touches the host. QEMU's sendkey
# takes one key name at a time and drops keys that arrive in a batch, so this
# walks the string character by character with a gap between each.
#
# Usage: mon-type.sh SOCKET "text to type" [--enter]

set -euo pipefail

SOCK="${1:?usage: mon-type.sh SOCKET TEXT [--enter]}"
TEXT="${2:?usage: mon-type.sh SOCKET TEXT [--enter]}"
ENTER="${3:-}"

send() { echo "sendkey $1" | socat - UNIX-CONNECT:"$SOCK" >/dev/null; sleep 0.15; }

for (( i = 0; i < ${#TEXT}; i++ )); do
    c="${TEXT:i:1}"
    case "$c" in
        [a-z0-9])  send "$c";;
        [A-Z])     send "shift-$(printf '%s' "$c" | tr 'A-Z' 'a-z')";;
        ' ')       send spc;;
        '\')       send backslash;;
        '/')       send slash;;
        '.')       send dot;;
        '-')       send minus;;
        '_')       send shift-minus;;
        ':')       send shift-semicolon;;
        ';')       send semicolon;;
        '=')       send equal;;
        ',')       send comma;;
        '*')       send shift-8;;
        '>')       send shift-dot;;
        '@')       send shift-2;;
        '|')       send shift-backslash;;
        '~')       send shift-grave_accent;;
        '"')       send shift-apostrophe;;
        *)         echo "mon-type: no key name for '$c'" >&2; exit 1;;
    esac
done

[ "$ENTER" = "--enter" ] && send ret
exit 0
