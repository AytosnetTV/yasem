# Yasem

MAG IPTV client for **CoreELEC** — portal UI plus hardware video on Amlogic boxes.

## Origin and acknowledgements

This project is **derived from and developed with deep thanks** to the original Yasem by **Maxim Vasilchuk**:

**https://github.com/mvasilchuk/yasem**

Without that foundation there would be no workable MAG/Qt path on Linux STBs. We keep Yasem’s core idea (MAG API emulation + Qt WebKit portal) and adapt it for real use on **CoreELEC**: boot without the Kodi UI, soft standby, IR, web config, amcodec playback, and a portable package you can install on a box.

If you use this code, please also credit the upstream project above.

## What it is

Yasem opens a Stalker/MAG portal in Qt WebKit and feeds streams into the box’s video stack. The goal is a classic MAG IPTV experience on affordable Amlogic boxes running CoreELEC — without relying on external VLC or CPU-only software playback.

## Player — the same power as Kodi on CoreELEC

On Amlogic CoreELEC, the “heavy lifting” is not a random software player. It is the **hardware path through amcodec** (`/dev/video10`) — the same stack **Kodi** uses on these boxes.

Yasem uses that path directly:

- demux/decode via FFmpeg + **amcodec** (as on CoreELEC/Kodi for Amlogic)
- interlaced H.264, HEVC, and typical IPTV/archive streams
- ALSA for audio
- no VLC; no forced software-only decode on the CPU

So in practice it feels like the strong playback people know from Kodi on CoreELEC: it **plays almost everything** the SoC and codec support (live, archive/timeshift, common IPTV containers). The portal is MAG; the picture muscle is the Amlogic/CoreELEC stack familiar from Kodi.

## Runs on CoreELEC

You can put it on a **CoreELEC box** (Amlogic) as long as you have:

- root over SSH
- working amcodec / `/dev/video10`
- a matching binary architecture (reference build: **32-bit ARM HF**, CoreELEC 19.x Matrix; aarch64 needs a rebuild)

Install is portable: copy `yasem.tar` (or the folder), run `install.sh`, reboot. Yasem can become the main UI (Yasem-only boot, Kodi masked), with portal, IR, soft standby, and web settings at `http://BOX-IP/config`.

Details: [`INSTALL.txt`](INSTALL.txt). Rebuild / recovery prompt: [`PROMPT.txt`](PROMPT.txt).

## Quick install

```sh
wget -O /tmp/yasem.tar http://www.example.com/mag/yasem.tar
tar xf /tmp/yasem.tar -C /tmp
sh /tmp/yasem/install.sh
reboot
```

(Replace the URL with your own package host.)

## Security in this repo

- No passwords or real box IPs.
- Deploy via env only: `YASEM_SSH_PASS`, optional `YASEM_SSH_HOST`.
- Default portal is a placeholder (`portal.example.com`) — set yours in `config/config.json` or via web `/config`.

## Layout

| Path | Contents |
|------|----------|
| `src/` | Source (`yasem.pro`) |
| `*.sh`, `*.service` | Install, boot, standby, LED, IR |
| `build/` | `build-yasem.sh`, `deploy-binary.sh`, `pack-portable.sh` |
| `config/` | Examples (`config.json.example`, `remote.conf`) |

## License / ethics

Upstream: [mvasilchuk/yasem](https://github.com/mvasilchuk/yasem). Use responsibly, respect the original project’s license, and follow the law for any content you play through the portal.
