#include "remote_keymap.h"

#include <QCoreApplication>
#include <QFile>
#include <QRegExp>
#include <QStringList>
#include <QTextStream>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

int parseIntToken(QString token)
{
    token = token.trimmed();
    if (token.startsWith("0x", Qt::CaseInsensitive))
        return token.mid(2).toInt(nullptr, 16);
    bool ok = false;
    int value = token.toInt(&ok, 10);
    return ok ? value : -1;
}

bool isActionToken(const QString &token)
{
    if (token.isEmpty())
        return false;
    if (token.size() == 1 && token.at(0).isDigit())
        return true; // "0".."9"
    const QString c = token.toUpper();
    static const char *actions[] = {
        "LEFT", "RIGHT", "UP", "DOWN", "OK", "ENTER", "SELECT", "DPAD_CENTER",
        "EXIT", "BACK", "DEL", "DELETE", "BACKSPACE", "HOME", "MENU",
        "VOL+", "VOL-", "VOL_PLUS", "VOL_MINUS", "VOLUMEUP", "VOLUMEDOWN",
        "MUTE", "PLAY_PAUSE", "PLAYPAUSE", "PAUSE", "STOP", "MEDIA_STOP",
        "INFO", "CLOCK", "REWIND", "FAST_FORWARD", "PAGEUP", "PAGEDOWN",
        "RED", "GREEN", "YELLOW", "BLUE", "GUIDE", "EPG", "FAVORITE", "FAV", "APP", "TV",
        "ON_OFF", "POWER", "SHUTDOWN", nullptr
    };
    for (int i = 0; actions[i]; ++i) {
        if (c == QLatin1String(actions[i]))
            return true;
    }
    return false;
}

QString normalizeComment(QString comment)
{
    comment = comment.trimmed().toUpper();
    // Keep VOL+ / VOL- recognizable
    if (comment == QLatin1String("VOL+") || comment.startsWith(QLatin1String("VOL+")))
        return QStringLiteral("VOL+");
    if (comment == QLatin1String("VOL-") || comment.startsWith(QLatin1String("VOL-")))
        return QStringLiteral("VOL-");
    comment.replace('-', '_');
    comment.replace(' ', '_');
    comment.replace('/', '_');
    while (comment.contains(QLatin1String("__")))
        comment.replace(QLatin1String("__"), QLatin1String("_"));
    return comment.section(QRegExp(QLatin1String("[\\s,;]+")), 0, 0);
}

bool tokenIs(const QString &c, const char *token)
{
    return c == QLatin1String(token);
}

bool tokenStarts(const QString &c, const char *token)
{
    return c.startsWith(QLatin1String(token));
}

} // namespace

RemoteKeymap &RemoteKeymap::instance()
{
    static RemoteKeymap keymap;
    return keymap;
}

void RemoteKeymap::addMapping(int linuxCode, const QString &comment)
{
    if (linuxCode <= 0 || linuxCode >= 512)
        return;

    Entry e;
    e.used = true;
    const QString c = normalizeComment(comment);
    if (c.isEmpty()) {
        e.used = false;
        return;
    }

    if (tokenIs(c, "ON_OFF") || tokenIs(c, "ON/OFF") || tokenIs(c, "POWER") || tokenIs(c, "SHUTDOWN")) {
        e.power = true;
        e.qtKey = Qt::Key_PowerOff;
    } else if (tokenIs(c, "UP")) {
        e.qtKey = Qt::Key_Up;
    } else if (tokenIs(c, "DOWN")) {
        e.qtKey = Qt::Key_Down;
    } else if (tokenIs(c, "LEFT")) {
        e.qtKey = Qt::Key_Left;
    } else if (tokenIs(c, "RIGHT")) {
        e.qtKey = Qt::Key_Right;
    } else if (tokenIs(c, "OK") || tokenIs(c, "ENTER") || tokenIs(c, "SELECT") || tokenIs(c, "DPAD_CENTER")) {
        e.qtKey = Qt::Key_Return;
    } else if (tokenIs(c, "EXIT") || tokenIs(c, "BACK")) {
        // Portal key.EXIT = 27 (Esc) — used to leave layers / go back
        e.qtKey = Qt::Key_Escape;
    } else if (tokenIs(c, "DEL") || tokenIs(c, "DELETE") || tokenIs(c, "BACKSPACE")) {
        // Portal key.BACK = 8 (Backspace)
        e.qtKey = Qt::Key_Backspace;
    } else if (tokenIs(c, "HOME") || tokenIs(c, "MENU")) {
        // Portal has no HOME; main menu is key.MENU = 122 (F11)
        e.qtKey = Qt::Key_F11;
    } else if (tokenIs(c, "TV")) {
        // Portal key.TV = 121 (F10)
        e.qtKey = Qt::Key_F10;
    } else if (tokenIs(c, "PAGEUP") || tokenIs(c, "PAGE_UP") || tokenIs(c, "REWIND")) {
        e.qtKey = Qt::Key_PageUp;
    } else if (tokenIs(c, "PAGEDOWN") || tokenIs(c, "PAGE_DOWN") || tokenIs(c, "FAST_FORWARD")
        || tokenIs(c, "FF")) {
        e.qtKey = Qt::Key_PageDown;
    } else if (tokenIs(c, "VOL+") || tokenIs(c, "VOL_PLUS") || tokenIs(c, "VOLUMEUP") || tokenIs(c, "VOLUME_UP")) {
        e.qtKey = Qt::Key_VolumeUp;
    } else if (tokenIs(c, "VOL-") || tokenIs(c, "VOL_MINUS") || tokenIs(c, "VOLUMEDOWN") || tokenIs(c, "VOLUME_DOWN")
        || tokenIs(c, "VOL_")) {
        e.qtKey = Qt::Key_VolumeDown;
    } else if (tokenIs(c, "MUTE")) {
        // Handled specially in browser (Stalker JS keyCode 219); marker key:
        e.qtKey = Qt::Key_VolumeMute;
    } else if (tokenIs(c, "PLAY_PAUSE") || tokenIs(c, "PLAYPAUSE") || tokenIs(c, "PAUSE")) {
        // Infomir: play/pause = Alt+R
        e.qtKey = Qt::Key_R;
        e.mods = Qt::AltModifier;
    } else if (tokenIs(c, "STOP") || tokenIs(c, "MEDIA_STOP")) {
        // Infomir: stop = Alt+S
        e.qtKey = Qt::Key_S;
        e.mods = Qt::AltModifier;
    } else if (tokenIs(c, "CLOCK")) {
        // Stalker key.CLOCK = 2032 = Alt+Space (32+2000)
        e.qtKey = Qt::Key_Space;
        e.mods = Qt::AltModifier;
    } else if (tokenIs(c, "INFO")) {
        // Infomir: info = Alt+Y → JS 2089
        e.qtKey = Qt::Key_Y;
        e.mods = Qt::AltModifier;
    } else if (tokenIs(c, "RED")) {
        e.qtKey = Qt::Key_F1;
    } else if (tokenIs(c, "GREEN")) {
        e.qtKey = Qt::Key_F2;
    } else if (tokenIs(c, "YELLOW")) {
        e.qtKey = Qt::Key_F3;
    } else if (tokenIs(c, "BLUE")) {
        e.qtKey = Qt::Key_F4;
    } else if (tokenIs(c, "FAVORITE") || tokenIs(c, "FAV") || tokenIs(c, "APP")) {
        // Stalker APP/favorites ≈ F12 → JS 123
        e.qtKey = Qt::Key_F12;
    } else if (tokenIs(c, "GUIDE") || tokenIs(c, "EPG")) {
        // Infomir MAG254: EPG = Qt::Key_F8 (0x01000037) → JS 119
        // (F11/122 is MENU and looks like Home on this portal)
        e.qtKey = Qt::Key_F8;
    } else if (tokenIs(c, "0")) {
        e.qtKey = Qt::Key_0;
    } else if (tokenIs(c, "1")) {
        e.qtKey = Qt::Key_1;
    } else if (tokenIs(c, "2")) {
        e.qtKey = Qt::Key_2;
    } else if (tokenIs(c, "3")) {
        e.qtKey = Qt::Key_3;
    } else if (tokenIs(c, "4")) {
        e.qtKey = Qt::Key_4;
    } else if (tokenIs(c, "5")) {
        e.qtKey = Qt::Key_5;
    } else if (tokenIs(c, "6")) {
        e.qtKey = Qt::Key_6;
    } else if (tokenIs(c, "7")) {
        e.qtKey = Qt::Key_7;
    } else if (tokenIs(c, "8")) {
        e.qtKey = Qt::Key_8;
    } else if (tokenIs(c, "9")) {
        e.qtKey = Qt::Key_9;
    } else if (tokenStarts(c, "UP")) {
        e.qtKey = Qt::Key_Up;
    } else if (tokenStarts(c, "DOWN")) {
        e.qtKey = Qt::Key_Down;
    } else if (tokenStarts(c, "LEFT")) {
        e.qtKey = Qt::Key_Left;
    } else if (tokenStarts(c, "RIGHT")) {
        e.qtKey = Qt::Key_Right;
    } else {
        fprintf(stderr, "remote keymap ignore code %d action '%s'\n",
            linuxCode, c.toUtf8().constData());
        e.used = false;
        return;
    }

    if (!table[linuxCode].used)
        ++mapCount;
    table[linuxCode] = e;
}

void RemoteKeymap::addDefaultFallbacks()
{
    struct Def { int code; const char *comment; };
    const Def defs[] = {
        {103, "UP"}, {108, "DOWN"}, {105, "LEFT"}, {106, "RIGHT"},
        {28, "OK"}, {158, "EXIT"}, {14, "DEL"},
        {172, "HOME"}, {46, "MENU"}, {104, "REWIND"}, {109, "FAST_FORWARD"},
        {57, "PLAY_PAUSE"}, {45, "STOP"}, {23, "INFO"},
        {31, "ON/OFF"}, {113, "MUTE"},
        // Do not default-map 114/115 to VOL on MAG boxes (conflicts with D-pad / Qt Right on some remotes).
        {2, "1"}, {3, "2"}, {4, "3"}, {5, "4"}, {6, "5"},
        {7, "6"}, {8, "7"}, {9, "8"}, {10, "9"}, {11, "0"},
    };
    for (const Def &d : defs) {
        if (!table[d.code].used)
            addMapping(d.code, QLatin1String(d.comment));
    }
}

void RemoteKeymap::parseFile(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    bool inKeys = false;
    bool sawBegin = false;
    QTextStream in(&file);
    while (!in.atEnd()) {
        QString line = in.readLine();
        QString trimmed = line.trimmed();
        if (trimmed.isEmpty())
            continue;
        if (trimmed.startsWith(QLatin1Char('#')))
            continue;
        if (trimmed.startsWith(QLatin1String("key_begin"))) {
            inKeys = true;
            sawBegin = true;
            continue;
        }
        if (trimmed.startsWith(QLatin1String("key_end"))) {
            inKeys = false;
            continue;
        }
        // If file has no key_begin, accept all non-comment lines.
        if (sawBegin && !inKeys)
            continue;

        QString action;
        int hash = trimmed.indexOf(QLatin1Char('#'));
        if (hash >= 0) {
            action = trimmed.mid(hash + 1).trimmed();
            trimmed = trimmed.left(hash).trimmed();
        }

        QStringList parts = trimmed.split(QRegExp(QLatin1String("\\s+")), Qt::SkipEmptyParts);
        if (parts.isEmpty())
            continue;

        int linuxCode = -1;
        // Kodi: <ir> <linux> #ACTION — hash action wins over simple parsing (e.g. 0x4e 2 #1).
        if (hash >= 0 && !action.isEmpty() && parts.size() >= 2
            && parseIntToken(parts.at(1)) >= 0) {
            linuxCode = parseIntToken(parts.at(1));
        } else if (parts.size() >= 2 && isActionToken(parts.at(1))
            && parseIntToken(parts.at(0)) >= 0) {
            // Simple: <linux/native code> <ACTION> (ACTION may be 0..9)
            linuxCode = parseIntToken(parts.at(0));
            if (action.isEmpty())
                action = parts.at(1);
        } else if (parts.size() >= 2 && parseIntToken(parts.at(1)) >= 0) {
            // Amlogic/Kodi style without # comment
            linuxCode = parseIntToken(parts.at(1));
            if (action.isEmpty() && parts.size() >= 3)
                action = parts.at(2);
        } else if (parts.size() == 1 && parseIntToken(parts.at(0)) >= 0 && !action.isEmpty()) {
            // <linux_code> #ACTION
            linuxCode = parseIntToken(parts.at(0));
        } else {
            continue;
        }

        if (linuxCode < 0 || action.isEmpty())
            continue;
        addMapping(linuxCode, action);
    }

    path = filePath;
}

void RemoteKeymap::load()
{
    std::memset(table, 0, sizeof(table));
    mapCount = 0;
    path.clear();

    QStringList candidates;
    const char *env = getenv("YASEM_REMOTE_CONF");
    if (env && env[0])
        candidates << QString::fromLocal8Bit(env);

    const QString appDir = QCoreApplication::applicationDirPath();
    candidates << (appDir + QStringLiteral("/config/remote.conf"));
    candidates << QStringLiteral("/storage/yasem/config/remote.conf");
    // Fallback only if Yasem file is missing
    candidates << QStringLiteral("/storage/.config/remote.conf");

    for (const QString &candidate : candidates) {
        if (!QFile::exists(candidate))
            continue;
        parseFile(candidate);
        if (mapCount > 0)
            break;
        path.clear();
        std::memset(table, 0, sizeof(table));
        mapCount = 0;
    }

    addDefaultFallbacks();
    fprintf(stderr, "remote keymap %s mappings %d\n",
        path.isEmpty() ? "(defaults)" : path.toUtf8().constData(), mapCount);
}

static bool isNavKey(int key)
{
    return key == Qt::Key_Left || key == Qt::Key_Right || key == Qt::Key_Up || key == Qt::Key_Down
        || key == Qt::Key_Return || key == Qt::Key_Enter;
}

// Qt evdevkeyboard reports nativeScanCode = linux_keycode + 8 (X11 convention).
static int linuxFromNative(int nativeScanCode)
{
    if (nativeScanCode >= 8)
        return nativeScanCode - 8;
    return nativeScanCode;
}

QKeyEvent RemoteKeymap::translate(const QKeyEvent *event) const
{
    const int native = event->nativeScanCode();
    const int linuxCode = linuxFromNative(native);
    int key = event->key();
    Qt::KeyboardModifiers mods = event->modifiers();

    // remote.conf stores Linux KEY_* codes (same as Kodi remotecfg / learn session).
    if (linuxCode > 0 && linuxCode < 512 && table[linuxCode].used) {
        const Entry &e = table[linuxCode];
        if (e.power) {
            key = Qt::Key_PowerOff;
            mods = Qt::NoModifier;
        } else {
            key = e.qtKey;
            mods = static_cast<Qt::KeyboardModifiers>(e.mods);
        }
    } else if (isNavKey(key)) {
        mods = Qt::NoModifier;
    } else if (key == Qt::Key_Escape || key == Qt::Key_Back) {
        key = Qt::Key_Backspace;
    }

    return QKeyEvent(event->type(), key, mods, event->nativeScanCode(),
        event->nativeVirtualKey(), event->nativeModifiers());
}

bool RemoteKeymap::isPower(int nativeScanCode) const
{
    const int linuxCode = linuxFromNative(nativeScanCode);
    if (linuxCode > 0 && linuxCode < 512 && table[linuxCode].used)
        return table[linuxCode].power;
    return linuxCode == 31;
}

bool RemoteKeymap::isPower(const QKeyEvent *event) const
{
    if (!event)
        return false;
    if (event->key() == Qt::Key_PowerOff || event->key() == Qt::Key_PowerDown
        || event->key() == Qt::Key_Hangup)
        return true;
    return isPower(event->nativeScanCode());
}
