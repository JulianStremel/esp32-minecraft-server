import serial, sys, time
# send one console line without resetting the board (DTR/RTS held low before opening)
s = serial.Serial()
s.port = sys.argv[1]
s.baudrate = 115200
s.dtr = False
s.rts = False
s.timeout = 0.2
s.open()
s.write((sys.argv[2] + '\n').encode())
end = time.time() + float(sys.argv[3] if len(sys.argv) > 3 else 1.5)
out = b''
while time.time() < end:
    out += s.read(4096)
s.close()
sys.stdout.write(out.decode('utf-8', 'replace'))
