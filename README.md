# Yasem

MAG IPTV клиент за **CoreELEC** — портал + хардуерно видео на Amlogic кутии.

## Произход и благодарности

Този проект е **взаимстван и доразвит с огромна благодарност** към оригиналния Yasem на **Maxim Vasilchuk**:

**https://github.com/mvasilchuk/yasem**

Без тази основа нямаше да има работещ MAG/Qt път на Linux STB. Тук запазваме идеята на Yasem (емулация на MAG API + Qt WebKit портал) и я адаптираме за реална експлоатация върху **CoreELEC**: boot без Kodi UI, soft standby, IR, web конфиг, amcodec плейбек и преносим пакет за инсталация на кутия.

Ако ползвате този код — отдайте заслугата и на upstream проекта по-горе.

## Какво е това

Yasem отваря Stalker/MAG портал в Qt WebKit и подава потоците към видео стека на кутията. Целта е класическото MAG IPTV усещане върху евтини Amlogic боксове с CoreELEC, без да разчитате на външен VLC или софтуерен-only плейбек.

## Плейър — същата мощ като при Kodi на CoreELEC

На Amlogic CoreELEC „супер мощният“ плейбек не е случаен софтуерен плейър, а **хардуерният път през amcodec** (`/dev/video10`) — същият стек, върху който стъпва и **Kodi** на тези кутии.

Yasem ползва този път директно:

- demux/decode през FFmpeg + **amcodec** (като при CoreELEC/Kodi на Amlogic)
- interlaced H.264, HEVC и типичните IPTV/archive потоци
- ALSA за звук
- без VLC; без емулация „на сила“ върху CPU

Затова на практика се държи като познатия силен плейбек на Kodi на CoreELEC: **пуска почти всичко**, което кутията и кодекът поддържат (live, archive/timeshift, обичайните IPTV контейнери). Порталът е MAG; мускулите на картинката са от Amlogic/CoreELEC стека, познат от Kodi.

## Работи на CoreELEC

Може да се сложи на **CoreELEC кутия** (Amlogic), стига да има:

- root по SSH
- работещ amcodec / `/dev/video10`
- подходяща архитектура за бинарния пакет (референцията е **32-bit ARM HF**, CoreELEC 19.x Matrix; за aarch64 трябва прекомпилация)

Инсталацията е преносима: копирате `yasem.tar` (или папката), пускате `install.sh`, рестарт — Yasem може да тръгне като основен UI (Yasem-only boot, Kodi masked), с портал, IR, soft standby и web настройки на `http://IP-на-кутията/config`.

Подробни стъпки: [`INSTALL.txt`](INSTALL.txt). Промпт за компилация/възстановяване: [`PROMPT.txt`](PROMPT.txt).

## Бърза инсталация

```sh
wget -O /tmp/yasem.tar http://www.example.com/mag/yasem.tar
tar xf /tmp/yasem.tar -C /tmp
sh /tmp/yasem/install.sh
reboot
```

(Заменете URL с вашия хост на пакета.)

## Сигурност в това репо

- Няма пароли и реални IP на кутии.
- Deploy само през env: `YASEM_SSH_PASS`, опционално `YASEM_SSH_HOST`.
- Порталът по подразбиране е placeholder (`portal.example.com`) — задайте своя в `config/config.json` или през web `/config`.

## Структура

| Път | Съдържание |
|-----|------------|
| `src/` | Изходен код (`yasem.pro`) |
| `*.sh`, `*.service` | Инсталация, boot, standby, LED, IR |
| `build/` | `build-yasem.sh`, `deploy-binary.sh`, `pack-portable.sh` |
| `config/` | Примери (`config.json.example`, `remote.conf`) |

## Лиценз / етика

Upstream: [mvasilchuk/yasem](https://github.com/mvasilchuk/yasem). Ползвайте отговорно, спазвайте лиценза на оригиналния проект и законите за съдържанието, което пускате през портала.
