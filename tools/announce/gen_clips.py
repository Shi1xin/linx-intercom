#!/usr/bin/env python3
"""Generate announce_clips.h for the firmware local announcer.

Speech is synthesized with the ElevenLabs TTS API (eleven_multilingual_v2),
encoded to 48 kHz mono Opus in an ogg container, then split back into raw
20 ms Opus packets — the exact wire format the playback task feeds
audio_opus_decode(). The attention chime and inter-phrase gap are generated
locally with ffmpeg.

API key: .local/elevenlabs.env  (ELEVENLABS_API_KEY=..., gitignored)

Usage:  python3 tools/announce/gen_clips.py
Output: firmware/intercom/main/announce_clips.h  (generated, committed)
"""

import json
import os
import subprocess
import sys
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "firmware/intercom/main/announce_clips.h"
ENV = ROOT / ".local/elevenlabs.env"

VOICE_ID = "EXAVITQu4vr4xnSDxMaL"   # Sarah - Mature, Reassuring, Confident
MODEL_ID = "eleven_multilingual_v2"
VOICE_SETTINGS = {
    "stability": 0.6,
    "similarity_boost": 0.75,
    "style": 0.0,
    "use_speaker_boost": True,
}

# clip name -> spoken text. Digits 0-9 double as Chinese numerals for room
# names ("二号房") and invite-code readout ("四六八九一一").
PHRASES = {
    "entered":  "欢迎进入",
    "room":     "房间",
    "hao_fang": "号房",
    "invite":   "邀请码",
    "left":     "已退出",
    "failed":   "操作失败",
    "offline":  "助手未连接",
    "d0": "零", "d1": "一", "d2": "二", "d3": "三", "d4": "四",
    "d5": "五", "d6": "六", "d7": "七", "d8": "八", "d9": "九",
    "d10": "十",
}


def load_key() -> str:
    key = os.environ.get("ELEVENLABS_API_KEY")
    if key:
        return key
    for line in ENV.read_text().splitlines():
        if line.startswith("ELEVENLABS_API_KEY="):
            return line.split("=", 1)[1].strip()
    sys.exit(f"no ELEVENLABS_API_KEY in env or {ENV}")


def tts(text: str, out: Path) -> None:
    req = urllib.request.Request(
        f"https://api.elevenlabs.io/v1/text-to-speech/{VOICE_ID}"
        "?output_format=mp3_44100_128",
        data=json.dumps({
            "text": text,
            "model_id": MODEL_ID,
            "voice_settings": VOICE_SETTINGS,
        }).encode(),
        headers={
            "xi-api-key": load_key(),
            "Content-Type": "application/json",
        },
        method="POST",
    )
    out.write_bytes(urllib.request.urlopen(req, timeout=60).read())


def chime(out: Path) -> None:
    """Two-tone attention cue, synthesized locally (no TTS chars spent)."""
    subprocess.run([
        "ffmpeg", "-y", "-loglevel", "error",
        "-f", "lavfi", "-i", "sine=frequency=830.61:duration=0.24",
        "-f", "lavfi", "-i", "sine=frequency=1108.73:duration=0.42",
        "-filter_complex",
        "[0]afade=t=in:d=0.02,afade=t=out:st=0.16:d=0.08[a];"
        "[1]afade=t=in:d=0.02,afade=t=out:st=0.28:d=0.14[b];"
        "[a][b]concat=n=2:v=0:a=1,volume=0.6",
        "-ar", "48000", "-ac", "1", str(out),
    ], check=True)


def gap(out: Path) -> None:
    """Short inter-phrase pause so spliced digits don't run together."""
    subprocess.run([
        "ffmpeg", "-y", "-loglevel", "error",
        "-f", "lavfi", "-i", "anullsrc=r=48000:cl=mono:d=0.12",
        "-ar", "48000", "-ac", "1", str(out),
    ], check=True)


def to_ogg(src: Path, ogg: Path) -> None:
    subprocess.run([
        "ffmpeg", "-y", "-loglevel", "error", "-i", str(src),
        "-af", "highpass=f=90,"
               "acompressor=threshold=-18dB:ratio=3:attack=8:release=120:makeup=4dB,"
               "silenceremove=start_periods=1:start_threshold=-45dB:start_silence=0.03,"
               "areverse,"
               "silenceremove=start_periods=1:start_threshold=-45dB:start_silence=0.05,"
               "areverse,"
               "loudnorm=I=-18:TP=-2",
        "-ar", "48000", "-ac", "1",
        "-c:a", "libopus", "-b:a", "32k", "-frame_duration", "20",
        str(ogg),
    ], check=True)


def ogg_packets(path: Path) -> list[bytes]:
    """Minimal ogg demuxer: concat page segments into packets."""
    data = path.read_bytes()
    packets, cur, i = [], bytearray(), 0
    while i + 27 <= len(data):
        assert data[i:i + 4] == b"OggS", f"bad ogg magic at {i}"
        nsegs = data[i + 26]
        segtab = data[i + 27:i + 27 + nsegs]
        payload = data[i + 27 + nsegs:
                       i + 27 + nsegs + sum(segtab)]
        pos = 0
        for seg in segtab:
            cur += payload[pos:pos + seg]
            pos += seg
            if seg < 255:
                packets.append(bytes(cur))
                cur = bytearray()
        i += 27 + nsegs + sum(segtab)
    return packets


def emit(name: str, packets: list[bytes]) -> str:
    # skip the ogg Skeleton/metadata headers: keep only packets that look like
    # audio frames (>=2 bytes, and not the OpusHead/OpusTags headers)
    audio = [p for p in packets
             if len(p) >= 2 and not p.startswith(b"OpusHead") and not p.startswith(b"OpusTags")]
    lines = []
    for idx, p in enumerate(audio):
        hexs = ", ".join(f"0x{b:02x}" for b in p)
        lines.append(f"static const uint8_t clip_{name}_{idx}[] = {{{hexs}}};")
    arr = ", ".join(f"{{clip_{name}_{i}, sizeof(clip_{name}_{i})}}"
                    for i in range(len(audio)))
    lines.append(f"static const opus_frame_t clip_{name}[] = {{{arr}}};")
    lines.append(f"#define CLIP_{name.upper()}_FRAMES {len(audio)}")
    return "\n".join(lines), len(audio)


def main() -> None:
    sections = [
        "/* announce_clips.h — GENERATED by tools/announce/gen_clips.py\n"
        " * ElevenLabs TTS + local chime; 48 kHz mono Opus, 20 ms frames\n"
        " * (libopus 32k). Do not edit. */",
        "#pragma once",
        "#include <stdint.h>",
    ]
    total = 0
    with tempfile.TemporaryDirectory() as td:
        td = Path(td)
        local = {"chime": chime, "gap": gap}
        for name, src in {**local, **PHRASES}.items():
            ext = "wav" if name in local else "mp3"
            raw, ogg = td / f"{name}.{ext}", td / f"{name}.ogg"
            if name in local:
                src(raw)
            else:
                tts(src, raw)
            if name == "gap":
                # pure silence must bypass the trim/compress chain
                subprocess.run([
                    "ffmpeg", "-y", "-loglevel", "error", "-i", str(raw),
                    "-ar", "48000", "-ac", "1",
                    "-c:a", "libopus", "-b:a", "32k", "-frame_duration", "20",
                    str(ogg),
                ], check=True)
            else:
                to_ogg(raw, ogg)
            section, nframes = emit(name, ogg_packets(ogg))
            label = src if name in PHRASES else "(local)"
            sections.append(f"\n/* {label} */\n{section}")
            total += nframes
            print(f"{name:10} {label:8} {nframes:3} frames", file=sys.stderr)
    sections.append("")
    OUT.write_text("\n".join(sections))
    print(f"wrote {OUT} ({total} frames)", file=sys.stderr)


if __name__ == "__main__":
    main()
