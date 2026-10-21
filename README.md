# Yasem

Portable MAG IPTV (Qt 5.15 + QtWebKit) client for **CoreELEC** on Amlogic (32-bit ARM HF), amcodec video path.

## Security notes

- No passwords or real box IPs are stored in this repository.
- SSH deploy uses env only: `YASEM_SSH_PASS` and optional `YASEM_SSH_HOST` (placeholder `YOUR_BOX_IP`).
- Default portal URL is a placeholder (`portal.example.com`) — set yours in `config/config.json` or via `http://BOX/config`.

## Layout

- `src/` — application source (`yasem.pro`)
- `*.sh`, `*.service` — install / boot / standby on the box
- `build/` — `build-yasem.sh`, `deploy-binary.sh`, `pack-portable.sh`
- `INSTALL.txt` / `PROMPT.txt` — install and rebuild notes

## Quick install (on CoreELEC)

```sh
# after you publish your own yasem.tar somewhere:
wget -O /tmp/yasem.tar http://www.example.com/mag/yasem.tar
tar xf /tmp/yasem.tar -C /tmp
sh /tmp/yasem/install.sh
reboot
```

## License / use

Intended for CoreELEC Amlogic boxes. Requires amcodec (`/dev/video10`). Not a drop-in for aarch64 without rebuild.
