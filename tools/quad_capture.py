#!/usr/bin/env python3
"""Capture simultanee des 4 cartes de test MeshPayV2.
Reset DTR/RTS de chaque port puis log horodate + prefixe par alias.
A lancer dans l'env IDF (pyserial dispo) :
  source ~/.espressif/v5.4.3/esp-idf/export.sh
  python3 tools/quad_capture.py [duree_s]
"""
import sys, time, threading
import serial

PORTS = {
    "loup-sobre":    "/dev/cu.usbmodem11101",
    "loup-doux":     "/dev/cu.usbmodem11201",
    "orque-curieux": "/dev/cu.usbmodem11301",
    "castor-precis": "/dev/cu.usbmodem11401",
}
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 60.0
T0 = time.time()


def reset_into_app(p):
    # Sequence projet : reset hors download -> boot app
    p.setRTS(True);  p.setDTR(False); time.sleep(0.1)
    p.setDTR(True);  p.setRTS(False); time.sleep(0.05); p.setDTR(False)


def capture(alias, port):
    try:
        p = serial.Serial(port, 115200, timeout=0.2)
    except Exception as e:
        print(f"[{alias}] OPEN ERROR: {e}", flush=True)
        return
    reset_into_app(p)
    buf = b""
    while time.time() - T0 < DURATION:
        try:
            data = p.read(512)
        except Exception as e:
            print(f"[{alias}] READ ERROR: {e}", flush=True)
            break
        if not data:
            continue
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            txt = line.decode("utf-8", "replace").rstrip()
            if txt:
                print(f"{time.time() - T0:7.2f} [{alias:13}] {txt}", flush=True)
    try:
        p.close()
    except Exception:
        pass


threads = [threading.Thread(target=capture, args=(a, p), daemon=True)
           for a, p in PORTS.items()]
for t in threads:
    t.start()
for t in threads:
    t.join()
print(f"--- capture terminee ({DURATION:.0f}s) ---", flush=True)
