import sys, time, serial
port = "/dev/cu.usbserial-0001"
secs = float(sys.argv[1]) if len(sys.argv) > 1 else 10
reset = "--reset" in sys.argv

s = serial.Serial(port, 115200, timeout=0.2)
if reset:                      # pulse EN via DTR/RTS to force a clean boot
    s.setDTR(False); s.setRTS(True); time.sleep(0.15)
    s.setRTS(False); time.sleep(0.05)
    s.reset_input_buffer()

end = time.time() + secs
while time.time() < end:
    data = s.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
s.close()
