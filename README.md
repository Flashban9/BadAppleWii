# BAD APPLE!!! for the Wii

Bad Apple now for the wii!!! Created by Flashban9 :P

A Wii Homebrew Channel app that plays Bad Apple!! with music in an endless loop.
It starts automatically, no menus.

## Controls
- **HOME** - leave back to the Wii menu
- **A** - change the aspect ratio (16:9 stretched / original 4:3 / 16:9 cropped)

## Installation
1. Copy the folder `apps/badapple/` to `SD:/apps/badapple/` (or `USB:/apps/badapple/`).
   It must contain: `boot.dol`, `meta.xml`, `icon.png`, `video.bin`, `audio.raw`.
2. Start the Homebrew Channel and launch **BAD APPLE!!!**.

Tip: set your Wii to 16:9 in the Wii settings (Wii Options > Wii Settings > Screen > Widescreen Settings)
to use the full widescreen picture.

## Building boot.dol
**Option A - GitHub Actions (no setup needed):** push/upload this repository to GitHub.
The workflow in `.github/workflows/build.yml` builds `boot.dol`; download it from
*Actions > latest run > Artifacts > boot-dol*.

**Option B - Docker:**

    docker run --rm -v "$PWD":/project -w /project devkitpro/devkitppc make install

**Option C - local devkitPro:** install devkitPPC and libogc, then run `make install`.

## Using your own video
    python3 tools/convert.py myvideo.mov sdcard/apps/badapple

Requires ffmpeg, python3 and numpy. The converter creates `video.bin`
(320x240, 30 fps, 1-bit, XOR delta + zero-run RLE) and `audio.raw`
(32 kHz, 16-bit, stereo, big endian).

## How it works
- Video: 1-bit frames, XOR against the previous frame + zero-run RLE (about 9.7 MB for 6572 frames).
- Audio: streamed through ASND by a separate thread, so drawing can never starve the sound.
- Sync: the audio position is the master clock, the video follows it.
- Display: scaled straight into the XFB; on a 16:9 Wii the 4:3 mode keeps the correct picture shape.
- Loop: audio and video restart together.

## Credits
Player by Flashban9. Bad Apple!! and its shadow-art video belong to their original creators.
