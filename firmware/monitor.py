#!/usr/bin/env python3
"""Prints a device's serial log. Usage: monitor.py PORT [SECONDS]"""
import sys, time
import serial  # pyserial

port = sys.argv[1]
duration = float(sys.argv[2]) if len(sys.argv) > 2 else 0
# Open without asserting DTR/RTS: on USB-Serial-JTAG they reset the chip
# into download mode.
ser = serial.Serial()
ser.port, ser.baudrate, ser.timeout = port, 115200, 0.5
ser.dtr = ser.rts = False
ser.open()
with ser as s:
    end = time.time() + duration if duration else None
    while end is None or time.time() < end:
        line = s.readline()
        if line:
            sys.stdout.write(line.decode(errors="replace"))
            sys.stdout.flush()
