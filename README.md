# BAD APPLE!!! – Wii Screensaver

Spielt Bad Apple!! in Endlosschleife mit Musik. Startet automatisch, HOME beendet.

## Bedienung
- Start: Video und Musik laufen in Endlosschleife.
- HOME (Wiimote) bzw. START (GameCube-Pad): zurueck ins Wii-Menue.
- 1 + 2 + Pfeil oben gleichzeitig: Umschalten zwischen Video und scrollender Code-Ansicht
  (die Musik laeuft weiter). Dieselbe Kombination schaltet zurueck.

## Installation (wenn du die fertige boot.dol hast)
Kopiere den Ordner `sdcard/apps/badapple/` nach `SD:/apps/badapple/`
(Inhalt: boot.dol, meta.xml, icon.png, video.bin, audio.raw). Dann im Homebrew Channel starten.

## boot.dol bauen (einmalig)
**Variante A – Docker (am einfachsten):**
    docker run --rm -v "$PWD":/project -w /project devkitpro/devkitppc make install
**Variante B – GitHub:** Projekt in ein Repo hochladen, der Workflow `.github/workflows/build.yml`
baut die boot.dol; unter "Actions -> Artifacts" die Datei `boot-dol` laden.
**Variante C – devkitPro lokal:** devkitPPC + libogc installieren, dann `make install`.

## Eigenes Video verwenden
    python3 tools/convert.py meinvideo.mov sdcard/apps/badapple
(braucht ffmpeg, python3, numpy). Format: 320x240, 30 fps, 1 Bit s/w; Audio 32 kHz Stereo.

## Technik
- Video: 1-Bit-Frames, XOR zum Vorgaenger + Zero-Run-RLE (9,7 MB fuer 6572 Frames)
- Audio: Rohdaten per DMA-Doppelpuffer, die Audioposition ist die Uhr fuer das Video
- Anzeige: 2x-Skalierung direkt in den XFB, bei 16:9-Wii bleibt das Bild 4:3
- Schleife: Audio und Video starten gemeinsam neu

## Optional: eigener Wii-Menue-Channel
Eine WAD braucht zusaetzliche Tools (z. B. einen Forwarder-Builder), die hier nicht dabei sind.
Der Homebrew-Channel-Eintrag deckt "Eigener App-Eintrag" komplett ab.
