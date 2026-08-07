import sys, time, serial

# Set this to whichever port the board enumerates as.
#   /dev/cu.usbserial-*  -> the "UART" port (CP210x/CH340 bridge)
#   /dev/cu.usbmodem*    -> the "USB" port (native USB-Serial-JTAG, CDC)
port = "/dev/cu.usbserial-0001"

secs = float(sys.argv[1]) if len(sys.argv) > 1 else 10
reset = "--reset" in sys.argv

s = serial.Serial(port, 115200, timeout=0.2)

# On the native USB port the ESP32-S3 runs USB CDC, and the Arduino USBCDC
# class only emits once the host raises DTR. Without this the port opens
# fine and you just get silence, which looks exactly like a dead sketch.
s.dtr = True
s.rts = False
time.sleep(0.3)

if reset:                      # pulse EN via DTR/RTS to force a clean boot
    s.setDTR(False); s.setRTS(True); time.sleep(0.15)
    s.setRTS(False); time.sleep(0.05)
    s.setDTR(True)
    s.reset_input_buffer()

end = time.time() + secs
while time.time() < end:
    data = s.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
s.close()
