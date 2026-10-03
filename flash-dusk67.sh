#!/bin/bash
# Build, convert and flash the Dusk67 over raw-HID -- no hands on the keyboard.
#
#   ./flash-dusk67.sh              build + flash
#   ./flash-dusk67.sh --uf2 FILE   flash an already-built .uf2 (skip the build)
#
# Requires:
#   * an ARM toolchain on PATH (arm-none-eabi-gcc)
#   * ./sync-qmk.sh run at least once (the keyboard must be inside qmk_firmware/)
#   * the udev rule from Architecture.md section 5, so the raw-HID node is
#     readable and writable by this user
#   * v0.2 or later on the board. v0.1 has no id_bootloader_jump handler, so
#     0x0B is ignored and this script cannot work with it. That is the one
#     manual step: hold Esc once while plugging the keyboard in.
#
# How it works:
#   1. send VIA cmd 0x0B (id_bootloader_jump) on the raw-HID node
#   2. the board resets into its vendor UF2 bootloader and re-enumerates as a
#      different USB device with an MSC volume
#   3. wait for that volume, copy the .uf2 onto it, let the bootloader write
#   4. wait for the keyboard to come back
#
# Step 1 deliberately does NOT wait for a reply: the firmware sends its reply
# then resets within microseconds, while USB polls the endpoint only every 1 ms,
# so a reply is not reliably delivered. Success is "the device left the bus",
# which is what we watch for.
set -euo pipefail

# Set by main() when the artifact is a temp file we made; deleted on exit.
TMP_ARTIFACT=""

REPO="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
# Both ids measured on hardware 2026-10-03, not guessed.
# The bootloader is NOT a 9d5b device: it enumerates as
#   1209:db42  Generic Devan Lai dapboot DFU bootloader
# which is picode's generic VID/PID used by the UniCore-F1 vendor bootloader,
# and the MSC volume it exposes is vfat labelled "UniCore-F1".
KEYBOARD_USB_ID="9d5b:2406"
BOOTLOADER_USB_ID="1209:db42"
BOOTLOADER_VOLUME_LABEL="UniCore-F1"
GONE_TIMEOUT_S=8
BOOTLOADER_TIMEOUT_S=25
RE_ENUM_TIMEOUT_S=30

usage() { sed -n '2,26p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
die() { echo "error: $*" >&2; exit 1; }
say() { echo "==> $*"; }

via_node() {
    # The raw-HID interface is whichever node answers get_protocol_version. Ask
    # each candidate rather than trusting a device number: those differ between
    # interfaces and change across reboots.
    local n
    for n in /dev/hidraw*; do
        [[ -r "$n" && -w "$n" ]] || continue
        python3 "$REPO/tools/via_probe.py" "$n" >/dev/null 2>&1 && { echo "$n"; return 0; }
    done
    return 1
}

usb_ids() { lsusb | awk '{print $6}'; }

wait_until_absent() {
    local want=$1 deadline=$((SECONDS + GONE_TIMEOUT_S))
    while (( SECONDS < deadline )); do
        usb_ids | grep -qxF "$want" || return 0
        sleep 0.2
    done
    return 1
}

wait_until_present() {
    local want=$1 deadline=$((SECONDS + $2)) n=0
    while (( SECONDS < deadline )); do
        usb_ids | grep -qxF "$want" && return 0
        sleep 0.3; n=$((n + 1))
    done
    return 1
}

# The bootloader's MSC volume, found by its measured USB id via udev (which
# knows the real topology; lsblk labels do not carry the USB id). Falls back to
# the volume label, since the id belongs to the vendor and could change if the
# vendor bootloader is updated.
bootloader_device() {
    local p props
    for p in /dev/sd*; do
        [[ -b "$p" ]] || continue
        props="$(udevadm info --query=property --name="$p" 2>/dev/null)" || continue
        if grep -q "^ID_VENDOR_ID=${BOOTLOADER_USB_ID%%:*}$" <<<"$props" &&
           grep -q "^ID_MODEL_ID=${BOOTLOADER_USB_ID##*:}$"    <<<"$props" &&
           grep -q "^ID_BUS=usb$"                            <<<"$props"; then
            echo "$p"; return 0
        fi
    done
    lsblk -o PATH,LABEL -P | awk -v l="$BOOTLOADER_VOLUME_LABEL" '
        index($0, "LABEL=\"" l "\"") {gsub(/.*PATH="/,""); gsub(/".*/,""); print; exit}'
}

build_uf2() {
    local out=$1
    command -v arm-none-eabi-gcc >/dev/null || die "arm-none-eabi-gcc not on PATH"
    [[ -d "$REPO/qmk_firmware/.git" || -f "$REPO/qmk_firmware/.git" ]] || \
        die "qmk_firmware submodule not initialised: git submodule update --init --recursive"
    [[ -d "$REPO/qmk_firmware/keyboards/ydkb/unicore_f1" ]] || \
        die "keyboard not synced into the submodule: run ./sync-qmk.sh"
    say "building"
    ( cd "$REPO/qmk_firmware" && make ydkb/unicore_f1:dusk67_via )
    say "converting to UF2"
    ( cd "$REPO/qmk_firmware" && python3 util/uf2conv.py \
        .build/ydkb_unicore_f1_dusk67_via.bin \
        -b 0x8004000 -c -f 0x9d5bcf10 -o "$out" )
}
main() {
    local uf2=""
    case "${1:-}" in
        --uf2) [[ -n "${2:-}" ]] || die "--uf2 needs a file"; uf2=$2; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        "") ;;
        *) usage; exit 2 ;;
    esac

    if [[ -n "$uf2" ]]; then
        [[ -f "$uf2" ]] || die "no such file: $uf2"
        # A file the caller owns. TMP_ARTIFACT stays empty so it is never removed.
    else
        uf2="$(mktemp -t dusk67.XXXXXX.uf2)"
        TMP_ARTIFACT="$uf2"
        build_uf2 "$uf2"
    fi
    say "artifact: $uf2 ($(stat -c%s "$uf2") bytes, md5 $(md5sum "$uf2" | cut -d' ' -f1))"

    local node
    node="$(via_node)" || die "no writable raw-HID node answers as a VIA keyboard.
  - is the board plugged in?
  - did you run ./sync-qmk.sh?
  - does the udev rule exist? (Architecture.md section 5)
  - is the board still on v0.1? 0x0B needs v0.2 or later."

    say "requesting bootloader jump on $node (VIA 0x0B)"
    python3 "$REPO/tools/via_probe.py" "$node" --bootloader-jump || true

    say "waiting for the board to leave the bus"
    wait_until_absent "$KEYBOARD_USB_ID" || die "the board did not reset.
  On v0.1 firmware 0x0B is ignored by design -- flash v0.2 once by hand
  (hold Esc while plugging in), then retry."

    say "waiting for the bootloader ($BOOTLOADER_USB_ID)"
    wait_until_present "$BOOTLOADER_USB_ID" "$BOOTLOADER_TIMEOUT_S" ||
        die "the bootloader did not enumerate as $BOOTLOADER_USB_ID.
  Run 'lsusb' now; if it shows another id, correct BOOTLOADER_USB_ID at the
  top of this script and retry."

    # The USB device enumerates before its block node, so the id being present
    # is not enough -- poll for the volume itself.
    local dev=""
    local deadline=$((SECONDS + BOOTLOADER_TIMEOUT_S))
    while (( SECONDS < deadline )); do
        dev="$(bootloader_device || true)"
        [[ -n "$dev" ]] && break
        sleep 0.5
    done
    [[ -n "$dev" ]] || die "the bootloader is up but its MSC volume never appeared.
  Run 'lsusb', 'lsblk -o PATH,LABEL -P' and
  'udevadm info --query=property --name=/dev/sdX' and report what appears."

    say "bootloader volume: $dev"
    local mp; mp="$(findmnt -n -o TARGET "$dev" 2>/dev/null || true)"
    if [[ -z "$mp" ]]; then
        # udisksctl needs no privileges; a plain mount into /run/media needs the
        # directory to exist first, which udisksctl creates for us.
        udisksctl mount -b "$dev" >/dev/null 2>&1 ||
            die "could not mount $dev (tried udisksctl)"
        mp="$(findmnt -n -o TARGET "$dev")"
    fi
    [[ -w "$mp" ]] || die "$mp is not writable by $(id -un)"

    say "copying UF2 to $mp"
    cp "$uf2" "$mp/"
    sync
    sleep 1

    say "waiting for the board to re-enumerate as a keyboard"
    if wait_until_present "$KEYBOARD_USB_ID" "$RE_ENUM_TIMEOUT_S"; then
        say "done"
    else
        say "warning: the board did not re-appear within ${RE_ENUM_TIMEOUT_S}s; replug it."
    fi
}

main "$@"

# The artifact is deleted only if we made it ourselves; a user-supplied --uf2 is
# theirs. Set at file scope so it survives main()'s return.
[[ -n "${TMP_ARTIFACT:-}" ]] && rm -f "$TMP_ARTIFACT"
exit 0