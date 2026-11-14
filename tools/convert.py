#!/usr/bin/env python3
"""Konvertiert badapple.mov in die zwei Dateien, die die Wii-App braucht:
  video.bin  - 1-Bit-Frames (XOR-Delta + Zero-Run-RLE)
  audio.raw  - 32 kHz, 16 Bit, Stereo, Big Endian (Rohdaten fuer das AI-Interface)
Aufruf: python3 convert.py badapple.mov ausgabeordner
Benoetigt: ffmpeg, python3, numpy
"""
import subprocess, struct, sys, os
import numpy as np

W, H, FPS = 320, 240, 30
THRESH = 128


def encode_frame(bits, prev):
    cur = np.packbits(bits.reshape(-1))  # W*H/8 Bytes, MSB zuerst
    delta = cur ^ prev
    d = delta.tolist()
    out = bytearray()
    i, n = 0, len(d)
    while i < n:
        b = d[i]
        if b:
            out.append(b)
            i += 1
        else:
            j = i
            while j < n and d[j] == 0 and j - i < 255:
                j += 1
            out += bytes((0, j - i))
            i = j
    return bytes(out), cur


def main():
    src, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    cmd = ["ffmpeg", "-v", "error", "-i", src, "-vf",
           f"fps={FPS},scale={W}:{H}:flags=area,format=gray",
           "-f", "rawvideo", "-"]
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE)
    fsize = W * H
    prev = np.zeros(fsize // 8, dtype=np.uint8)
    frames = []
    while True:
        buf = p.stdout.read(fsize)
        if len(buf) < fsize:
            break
        img = np.frombuffer(buf, dtype=np.uint8)
        data, prev = encode_frame(img >= THRESH, prev)
        frames.append(data)
    p.wait()
    vpath = os.path.join(outdir, "video.bin")
    with open(vpath, "wb") as f:
        f.write(b"BAD1" + struct.pack(">HHHI", W, H, FPS, len(frames)))
        for d in frames:
            f.write(struct.pack(">H", len(d)) + d)
    print(f"{len(frames)} Frames, video.bin = {os.path.getsize(vpath)/1e6:.2f} MB")
    apath = os.path.join(outdir, "audio.raw")
    subprocess.check_call(["ffmpeg", "-v", "error", "-y", "-i", src, "-vn",
                           "-ac", "2", "-ar", "32000", "-f", "s16be", apath])
    print(f"audio.raw = {os.path.getsize(apath)/1e6:.2f} MB")


main()
