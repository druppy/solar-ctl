#!/bin/sh
# env-powercut.sh - bench driver for the power-cut-mid-fw_setenv test.
#
# The U-Boot env is a RAW redundant pair in a hidden SD area (/etc/fw_env.config:
# /dev/mmcblk0 @0x6800000 + @0x6804000, 16 KiB copies, class solar-ablayout).
# A torn write must always leave one valid copy ([crc32 LE][flags][data],
# higher flags wins) so the board boots and fw_printenv reports ONE consistent
# value. This script arms each trial over ssh; the HUMAN yanks the power at the
# printed prompt (only a real power cut emulates SD write-cache loss); verdict
# waits for the reboot and decodes both raw copies.
#
# Usage: env-powercut.sh <dump|done|arm|torn|corrupt|verdict> [user@host]
#        (default host: root@192.168.0.53)
#
#  dump      decode both raw copies + live env (read-only, run first/anytime)
#  done      control flip: fw_setenv completes + double sync + reboot
#  arm       fw_setenv to the OTHER slot, then prompt "PULL POWER NOW".
#            Kernel dirty-writeback is raised to ~15 s so at cut time the write
#            is still in the page cache: this is the "fw_setenv said OK but
#            power died before the SD saw it" case. Verdict records whether the
#            value survived (=> libubootenv fsyncs internally) or reverted
#            (=> the untouched copy won). Either is a PASS; crash/torn slot is not.
#  corrupt   NO power cut: flip a byte in the ACTIVE (higher-flags) copy to
#            invalidate its CRC, then reboot. Deterministically proves the
#            worst case - one dead copy - falls back to the other and boots.
#            The board rewrites the dead copy on the next fw_setenv (self-heal).
#  torn      Fully self-run (no hand on the plug): build the COMPLETE promoted
#            standby copy (active vars + slot flipped to the other + flags
#            max+1 + correct CRC), then flush only its LEADING ~60% to the
#            standby region, leaving the tail stale. That is the exact partial-
#            write state a power cut leaves mid-fw_setenv: standby now has the
#            NEWEST flags but a CRC that no longer matches => libubootenv/u-boot
#            must reject it and boot the untouched active (last-good) copy.
#            More faithful than corrupt(): it exercises the highest-flags-but-
#            torn copy, which is the real power-cut-winner trap.
#  verdict   wait for the board, decode copies, PASS/FAIL + tsv row (run it
#            yourself after each physical cut).
#
# The marker file (from/to/type) is written to /data and SYNCED before the
# risky write, so it always survives the cut and verdict knows the trial.
#
# Host needs: ssh key auth + python3 (CRC decode). Device side is pure
# busybox: NO od -t / head -c / base64 applets - the copies are fetched
# binary-clean over command-form ssh (no pty).
set -e

CMD=${1:-dump}
HOST=${2:-root@192.168.0.53}
SSH="ssh -o BatchMode=yes -o ConnectTimeout=40 -o KexAlgorithms=curve25519-sha256"
OFF0=109051904          # 0x6800000 copy0 - class solar-ablayout geometry, keep in sync
OFF1=109068288          # 0x6804000 copy1
RESULTS=$(dirname "$0")/powercut-results.tsv

rsh() { timeout 60 $SSH "$HOST" "$1"; }

other() { case "$1" in a) echo b;; b) echo a;; *) return 1;; esac; }

decode() {
    # $1 tmpd  $2 obs-slot  $3 root-partition  $4 booted(1/0)  $5 trial  $6 marker
    python3 - "$1" "$2" "$3" "$4" "$5" "$6" <<'PYEOF'
import struct, sys, zlib

tmpd, obs, root, booted, trial, marker = sys.argv[1:7]
copies = []
for i, name in enumerate(("copy0@0x6800000", "copy1@0x6804000")):
    blob = open(f"{tmpd}/copy{i}.bin", "rb").read()
    crc_stored, = struct.unpack("<I", blob[0:4])
    data = blob[5:]
    toks = [t.decode(errors="replace") for t in data.split(b"\0") if b"=" in t]
    copies.append(dict(
        name=name, flags=blob[4], valid=crc_stored == (zlib.crc32(data) & 0xFFFFFFFF),
        crcs=f"{crc_stored:08x}/{zlib.crc32(data) & 0xFFFFFFFF:08x}",
        slot=next((t[5:] for t in toks if t.startswith("slot=")), None), vars=len(toks)))

for c in copies:
    print(f"  {c['name']}: flags={c['flags']:<3} CRC {'VALID  ' if c['valid'] else 'INVALID'} "
          f"(stored/calc {c['crcs']}) slot={c['slot']} vars={c['vars']}")

valid = [c for c in copies if c["valid"]]
# u-boot picks the higher flags (signed wrap comparison); bench-age flags make
# a plain max() equivalent.
winner = max(valid, key=lambda c: c["flags"]) if valid else None

checks = [
    ("BOOT       board answered ssh again", booted == "1"),
    ("SINGLEVAL  fw_printenv slot is exactly a or b", obs in ("a", "b")),
    ("WINNER     max-flags valid copy carries that value",
     bool(winner) and winner["slot"] == obs),
    ("ROOTMATCH  booted root= matches the slot", {"a": "p2", "b": "p3"}.get(obs, "?") in root),
    ("COMPLETE   winner env is full (vars>10, not slot-only garbage)",
     bool(winner) and winner["vars"] > 10),
]
for label, ok in checks:
    print(f"  [{' ok' if ok else 'FAIL'}] {label}")
if trial != "dump":
    verdict = "PASS" if all(ok for _, ok in checks) else "FAIL"
    print(f"  VERDICT: {verdict}")
    print(f"{verdict}\t{trial}\t{marker}\tobs={obs}\troot={root}\t"
          + "\t".join("ok" if ok else "FAIL" for _, ok in checks))
PYEOF
}

fetch_and_decode() {  # $1 booted  $2 trial  $3 marker
    TMPD=$(mktemp -d)
    $SSH "$HOST" "dd if=/dev/mmcblk0 bs=512 skip=$((OFF0/512)) count=32 2>/dev/null" > "$TMPD/copy0.bin"
    $SSH "$HOST" "dd if=/dev/mmcblk0 bs=512 skip=$((OFF1/512)) count=32 2>/dev/null" > "$TMPD/copy1.bin"
    OBS=$(rsh "fw_printenv -n slot 2>/dev/null" || echo MISSING)
    ROOT=$(rsh "tr '\\0' ' ' < /proc/cmdline | grep -o 'root=[^ ]*'" || echo none)
    set +e
    decode "$TMPD" "$OBS" "$ROOT" "$1" "$2" "$3" | tee /tmp/envcut-row.txt
    set -e
    rm -rf "$TMPD"
}

case "$CMD" in
    dump)
        fetch_and_decode 1 dump - | grep -v VERDICT
        ;;
    done|arm|corrupt|torn)
        CUR=$(rsh 'fw_printenv -n slot')
        TGT=$(other "$CUR") || { echo "unexpected current slot '$CUR'" >&2; exit 1; }
        echo "current slot=$CUR -> this trial targets '$TGT'"
        ;;
esac

case "$CMD" in
    done)
        rsh "fw_setenv slot $TGT && sync && sync"
        echo "value written + synced; rebooting"
        rsh 'reboot' || true
        sleep 40
        ;;
    arm)
        echo ">>> After the 'PULL POWER NOW' line: PULL THE POWER (wait for that line!)"
        rsh "mkdir -p /data/solar-envcut
             echo 'type=arm from=$CUR to=$TGT' > /data/solar-envcut/marker
             date -u >> /data/solar-envcut/marker
             sync
             echo 3000 > /proc/sys/vm/dirty_expire_centisecs
             echo 1500 > /proc/sys/vm/dirty_writeback_centisecs
             fw_setenv slot $TGT
             echo 'PULL POWER NOW (write is in the page cache, ~15s window)'"
        echo "--- if you did NOT cut: 'ssh $HOST reboot' and rerun ---"
        exit 0
        ;;
    corrupt)
        # Determine which copy is ACTIVE (higher flags), fetch it, flip one data
        # byte HOST-side, write the whole 16 KiB region back (busybox dd has no
        # conv=notrunc, and a whole-region write needs none; the stream is
        # binary-clean over command-form ssh - fbdump lesson).
        TMPD=$(mktemp -d)
        $SSH "$HOST" "dd if=/dev/mmcblk0 bs=512 skip=$((OFF0/512)) count=32 2>/dev/null" > "$TMPD/copy0.bin"
        $SSH "$HOST" "dd if=/dev/mmcblk0 bs=512 skip=$((OFF1/512)) count=32 2>/dev/null" > "$TMPD/copy1.bin"
        ACTIVE=$(python3 -c "
f0=open('$TMPD/copy0.bin','rb').read(); f1=open('$TMPD/copy1.bin','rb').read()
assert len(f0)==16384 and len(f1)==16384
print(0 if f0[4]>=f1[4] else 1)")
        python3 -c "
b=bytearray(open('$TMPD/copy$ACTIVE.bin','rb').read())
b[40] ^= 0xAA   # data byte (header is 5): CRC now mismatches
open('$TMPD/copy$ACTIVE.bin','wb').write(bytes(b))"
        ACTOFF=$((ACTIVE==0 ? OFF0 : OFF1))
        echo "active copy = copy$ACTIVE @0x$(printf '%x' $ACTOFF); killing its CRC (flip byte 40)"
        rsh "mkdir -p /data/solar-envcut
             echo 'type=corrupt note=active-copy-CRC-killed' > /data/solar-envcut/marker
             date -u >> /data/solar-envcut/marker
             sync"
        cat "$TMPD/copy$ACTIVE.bin" | timeout 60 $SSH "$HOST" \
            "dd of=/dev/mmcblk0 bs=512 seek=$((ACTOFF/512)) count=32 2>/dev/null && sync"
        rm -rf "$TMPD"
        echo "dead copy planted; rebooting into the surviving copy"
        rsh 'reboot' || true
        sleep 40
        ;;
    torn)
        # The real power-cut state, synthesized deterministically: a real cut
        # mid-fw_setenv leaves the STANDBY copy partially flushed - newest
        # flags, CRC no longer matching the (partly stale) bytes. We build the
        # complete promoted standby copy (active vars + slot=$TGT + flags
        # max+1 + fresh CRC) and flush only its leading 19 KiB blocks, leaving
        # the tail stale: exactly what a torn block write leaves. The standby
        # copy then has the HIGHEST flags but a failing CRC - the real
        # power-cut-winner trap that corrupt() cannot reach.
        TMPD=$(mktemp -d)
        $SSH "$HOST" "dd if=/dev/mmcblk0 bs=512 skip=$((OFF0/512)) count=32 2>/dev/null" > "$TMPD/copy0.bin"
        $SSH "$HOST" "dd if=/dev/mmcblk0 bs=512 skip=$((OFF1/512)) count=32 2>/dev/null" > "$TMPD/copy1.bin"
        ACTIVE=$(python3 -c "
f0=open('$TMPD/copy0.bin','rb').read(); f1=open('$TMPD/copy1.bin','rb').read()
assert len(f0)==16384 and len(f1)==16384
print(0 if f0[4]>=f1[4] else 1)")
        STANDBY=$((1-ACTIVE))
        python3 - "$ACTIVE" "$STANDBY" "$TGT" "$TMPD" <<'PYEOF'
import struct, sys, zlib
active, standby, tgt, tmpd = int(sys.argv[1]), int(sys.argv[2]), sys.argv[3], sys.argv[4]
src = open(f"{tmpd}/copy{active}.bin", "rb").read()
flags = max(open(f"{tmpd}/copy0.bin", "rb").read()[4],
            open(f"{tmpd}/copy1.bin", "rb").read()[4]) + 1
data = bytearray(src[5:])
i = data.find(b"slot=")
assert i >= 0 and data[i+5:i+6] in (b"a", b"b"), "no slot= token in active copy"
data[i+5:i+6] = tgt.encode()
# The promoted data MUST genuinely differ from the standby's stale tail:
# flip-flopping slot= makes it byte-identical to the old standby copy (CRC
# covers data only), so a partial flush would trivially be a COMPLETE write
# and the trial proves nothing (hit 2026-10-06: first torn run landed CRC
# VALID). One byte stamped in the NUL padding beyond the 19-sector window
# makes the tail stale exactly like any real content change would.
data[12000] ^= 0x5A
new = (struct.pack("<I", zlib.crc32(bytes(data)) & 0xFFFFFFFF)
       + bytes([flags & 0xFF]) + bytes(data))
assert len(new) == 16384
open(f"{tmpd}/torn.bin", "wb").write(new[:19*512])   # ~60%: torn mid-copy
PYEOF
        STANDBYOFF=$((STANDBY==0 ? OFF0 : OFF1))
        echo "torn target = copy$STANDBY @0x$(printf '%x' $STANDBYOFF): promoted flags + slot=$TGT, only 19 of 32 sectors flushed"
        rsh "mkdir -p /data/solar-envcut
             echo 'type=torn note=partial-promotion-of-standby' > /data/solar-envcut/marker
             date -u >> /data/solar-envcut/marker
             sync"
        cat "$TMPD/torn.bin" | timeout 60 $SSH "$HOST" \
            "dd of=/dev/mmcblk0 bs=512 seek=$((STANDBYOFF/512)) count=19 2>/dev/null && sync"
        rm -rf "$TMPD"
        echo "torn standby planted (highest flags, dead CRC); rebooting - active copy must win"
        rsh 'reboot' || true
        sleep 40
        ;;
    verdict)
        BOOTED=0
        i=0
        while [ $i -lt 24 ]; do
            if rsh 'true' 2>/dev/null; then BOOTED=1; break; fi
            i=$((i + 1)); sleep 10
        done
        MARKER=$(rsh "cat /data/solar-envcut/marker 2>/dev/null | tr '\n' ' '" || echo marker-lost)
        TRIAL=$(printf '%s' "$MARKER" | grep -o 'type=[a-z]*' | cut -d= -f2 || true)
        fetch_and_decode "$BOOTED" "${TRIAL:-cut}" "$MARKER"
        # A marker is scored exactly once: without this, a second bare verdict
        # after a cut appends a phantom duplicate row (bench 2026-10-06: an arm
        # that never reached the board let verdict re-score the PREVIOUS trial
        # - identical marker timestamp was the tell).
        if [ "$MARKER" = "marker-lost" ]; then
            echo "(no armed trial found - state report only, nothing appended)"
        else
            [ -f "$RESULTS" ] || printf 'verdict\ttrial\tmarker\tobs\troot\tchecks\n' > "$RESULTS"
            tail -n 1 /tmp/envcut-row.txt >> "$RESULTS"
            echo "(appended to $RESULTS)"
            [ "$BOOTED" = 1 ] && rsh "mv /data/solar-envcut/marker /data/solar-envcut/last-marker 2>/dev/null" || true
            echo "(marker consumed - arm -> verdict is strictly one-shot)"
        fi
        ;;
esac
