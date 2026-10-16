#!/bin/sh
# AVRCP volume probe for the W1's classic-Bluetooth speaker ("FS-0F7A32-A", an A2DP sink).
# Connects, reads the speaker's volume, optionally sets it to MUTE (0) or MAX (127), reads it back, and ALWAYS
# disconnects and removes the pairing afterwards, verifying that it is gone. It never touches the BLE motor path.
#   audio-probe.sh read      only read the current volume
#   audio-probe.sh mute      set volume 0, then wait while you press console buttons and listen
#   audio-probe.sh max       set volume 127, then wait
#   audio-probe.sh restore   set volume 105, the value the console had before we ever touched it
# NOTE: the console REMEMBERS the volume across connections, so always finish with "restore".
# It cannot tell you whether this volume affects the console's own beeps: that needs the console awake and your ears.
# Env: W1_AUDIO_ADDRESS (default DD:87:70:39:74:1F), WAIT_SECONDS (default 20).
set -u
addr=${W1_AUDIO_ADDRESS:-DD:87:70:39:74:1F}
mode=${1:-read}
case "$mode" in mute) vol=0;; max) vol=127;; restore) vol=105;; read) vol=;; *) echo "usage: $0 read|mute|max|restore" >&2; exit 2;; esac
wait_seconds=${WAIT_SECONDS:-20}
node=dev_$(echo "$addr" | tr ':' '_')
sink() { XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)} wpctl inspect @DEFAULT_AUDIO_SINK@ 2>/dev/null | awk -F'"' '/node.name/ {print $2; exit}'; }
before=$(sink); before=${before:-unknown}
bluetoothctl --timeout 60 scan on >/dev/null 2>&1 &
scan=$!
cleanup() {
    bluetoothctl disconnect "$addr" >/dev/null 2>&1
    bluetoothctl remove "$addr" >/dev/null 2>&1
    kill "$scan" 2>/dev/null
    sleep 2
    if bluetoothctl devices Paired 2>/dev/null | grep -q "$addr"; then echo "WARNING: $addr is still paired; run: bluetoothctl remove $addr" >&2; else echo "pairing removed"; fi
    after=$(sink); after=${after:-unknown}
    if [ "$before" = "$after" ]; then echo "default audio sink unchanged: $after"; else echo "WARNING: default audio sink changed from $before to $after" >&2; fi
}
trap cleanup EXIT INT TERM
sleep 9
if ! bluetoothctl connect "$addr" 2>&1 | grep -q 'Connection successful'; then echo "could not connect to $addr (is the console powered?)" >&2; exit 1; fi
sleep 3
transport=$(busctl --system tree org.bluez 2>/dev/null | grep "$node" | grep '/fd' | head -1 | awk '{print $NF}')
if [ -z "$transport" ]; then echo "no media transport appeared for $addr" >&2; exit 1; fi
volume() { busctl --system get-property org.bluez "$transport" org.bluez.MediaTransport1 Volume 2>&1; }
echo "transport: $transport"
echo "volume now: $(volume)"
if [ -n "$vol" ]; then
    busctl --system set-property org.bluez "$transport" org.bluez.MediaTransport1 Volume q "$vol" 2>&1
    sleep 1
    echo "volume after setting $vol: $(volume)"
    if [ "$mode" != restore ]; then
        echo ">>> volume set to $vol; press console buttons and listen for beeps now (${wait_seconds}s)" >&2
        sleep "$wait_seconds"
    fi
fi
