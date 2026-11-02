"""Reset the ESP8266 and print its serial output for a few seconds.

Usage: readserial.py [port] [seconds]
Run with the PlatformIO venv's Python, which has pyserial:
    ~/.local/share/pipx/venvs/platformio/bin/python -I tools/readserial.py /dev/ttyUSB0 6
"""

import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "/dev/ttyUSB0"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 6

s = serial.Serial(port, 115200, timeout=0.2)
# Pulse RTS with DTR released: the NodeMCU auto-reset circuit restarts into normal boot
s.dtr = False
s.rts = True
time.sleep(0.1)
s.rts = False

end = time.time() + seconds
while time.time() < end:
    data = s.read(256)
    if data:
        sys.stdout.write(data.decode(errors="replace"))
        sys.stdout.flush()
s.close()
