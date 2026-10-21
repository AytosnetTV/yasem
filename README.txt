YASEM — преносим MAG IPTV емулатор за CoreELEC (Amlogic), пакет 2026-10-08

Какво е
  Qt5/WebKit MAG254 клиент + HW видео през CoreELEC libamcodec.
  Работи без Kodi: yasem.service при boot (Kodi се маскира).
  Soft standby (Power), LED, IR, web config :80, portal Settings, archive TV.

Изисквания
  CoreELEC 19.x Matrix, 32-bit ARM HF, Amlogic (S905/S905L…)
  HDMI 1080i50hz (install/start го настройват)
  Портал по подразбиране: http://portal.example.com

Бърза инсталация от интернет (след като качиш yasem.tar)
  На кутията (root SSH):

    wget -O /tmp/yasem.tar http://www.example.com/mag/yasem.tar
    tar xf /tmp/yasem.tar -C /tmp
    sh /tmp/yasem/install.sh
    reboot

  Или с включения скрипт (ако вече имаш стара инсталация):

    sh /storage/yasem/install-from-url.sh http://www.example.com/mag/yasem.tar

Бърза инсталация от PC
  scp -r yasem-portable root@BOX_IP:/tmp/yasem
  ssh root@BOX_IP 'sh /tmp/yasem/install.sh'
  reboot

Подробно упътване:  INSTALL.txt
Промпт / компилация: PROMPT.txt

Конфиг
  http://IP-НА-КУТИЯТА/config     — LAN UI (мрежа, портал, reboot)
  /storage/yasem/config/config.json   — portal, model, mac…
  /storage/yasem/config/remote.conf — дистанционно
  /storage/yasem/led-status.sh      — LED on/standby

Power
  Изключва: черен екран + червен LED, мрежата работи
  Включва: Yasem + син LED

Логове
  /storage/yasem/yasem.log
  /storage/yasem/standby.log

Качване на сайта
  След pack-portable.sh: /path/to/yasem-project/yasem.tar
  → копирай като https://www.example.com/mag/yasem.tar

Бекъп на PC
  /path/to/yasem-project/yasem-portable\
  /path/to/yasem-project/yasem.tar
  /path/to/yasem-project/backup/yasem-YYYY-MM-DD\
  /path/to/yasem-project/backup/yasem-YYYY-MM-DD-HHMM.tar.gz
