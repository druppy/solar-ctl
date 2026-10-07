#!/bin/sh
# Zero-hardware check of modbus-inverter-emu.py: socat PTY pair,
# slave on one end, master poller on the other. Temp file, delete after.
set -e
rm -f /tmp/emu-pa /tmp/emu-pb
socat pty,raw,echo=0,link=/tmp/emu-pa pty,raw,echo=0,link=/tmp/emu-pb &
SOCAT=$!
trap 'kill $SOCAT 2>/dev/null' EXIT

python3 fw/tools/modbus-inverter-emu.py /tmp/emu-pa --baud 9600 --addr 1 >/tmp/emu-slave.log 2>&1 &
SLAVE=$!
sleep 0.5

timeout 6 python3 fw/tools/modbus-inverter-emu.py /tmp/emu-pb --baud 9600 --addr 1 --master --interval 0.4 >/tmp/emu-master.log 2>&1 || true

kill $SLAVE 2>/dev/null
echo "===== MASTER ====="
cat /tmp/emu-master.log
echo "===== SLAVE ====="
cat /tmp/emu-slave.log
