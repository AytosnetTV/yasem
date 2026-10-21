#include "magapi.h"

#include "player.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLatin1String>
#include <QNetworkInterface>
#include <QProcess>
#include <QTimer>
#include <QXmlStreamReader>

#include <cstdio>

extern "C" {
#include <openssl/evp.h>
#include <openssl/hmac.h>
}

namespace {

struct ImageInfo {
    const char *version;
    const char *description;
    const char *date;
    const char *hardware;
};

ImageInfo imageFor(const QString &model)
{
    if (model == "MAG250")
        return {"218", "0.2.18-250", "Thu Feb 20 11:29:26 EET 2014", "1.7-BD-00"};
    if (model == "MAG256" || model == "MAG322")
        return {"256", "0.2.32-r23", "Fri Dec 18 18:32:00 EET 2015", "2.6-BD-00"};
    return {"255", "0.2.32-r23", "Fri Dec 18 18:32:00 EET 2015", "2.6-BD-00"};
}

QString userAgentFor(const QString &model)
{
    if (model == "MAG250")
        return "Mozilla/5.0 (QtEmbedded; U; Linux; C) AppleWebKit/533.3 (KHTML, like Gecko) MAG200 stbapp ver: 2 rev: 250 Safari/533.3";
    if (model == "MAG256")
        return "Mozilla/5.0 (QtEmbedded; U; Linux; C) AppleWebKit/533.3 (KHTML, like Gecko) MAG256 stbapp ver: 5 rev: 3714 Safari/533.3";
    if (model == "MAG322")
        return "Mozilla/5.0 (QtEmbedded; U; Linux; C) AppleWebKit/533.3 (KHTML, like Gecko) MAG322 stbapp ver: 5 rev: 3714 Safari/533.3";
    return "Mozilla/5.0 (QtEmbedded; U; Linux; C) AppleWebKit/533.3 (KHTML, like Gecko) MAG254 stbapp ver: 4 rev: 2723 Safari/533.3";
}

QString readTextFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll());
}

QString macFromSysfs()
{
    const char *ifaces[] = {"eth0", "eth1", "wlan0", nullptr};
    for (int i = 0; ifaces[i]; ++i) {
        QString mac = readTextFile(QString("/sys/class/net/%1/address").arg(ifaces[i])).trimmed().toUpper();
        if (mac.size() == 17)
            return mac;
    }
    return QString();
}

QString ipFromInterface()
{
    const auto addresses = QNetworkInterface::allAddresses();
    for (const QHostAddress &address : addresses) {
        if (address.protocol() == QAbstractSocket::IPv4Protocol && !address.isLoopback())
            return address.toString();
    }
    return "127.0.0.1";
}

QString normalizePortal(QString url)
{
    url = url.trimmed();
    if (url.isEmpty())
        url = "http://portal.example.com";
    if (!url.contains("://"))
        url.prepend("http://");
    while (url.endsWith('/'))
        url.chop(1);
    return url;
}

void readKodiSettings(QVariantMap &values)
{
    QFile file("/storage/.kodi/userdata/addon_data/plugin.program.mag.portal/settings.xml");
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return;
    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (!xml.isStartElement() || xml.name() != QLatin1String("setting"))
            continue;
        QString id = xml.attributes().value("id").toString();
        QString value = xml.attributes().value("value").toString();
        if (value.isEmpty())
            value = xml.readElementText(QXmlStreamReader::SkipChildElements);
        if (!id.isEmpty())
            values.insert(id, value.trimmed());
    }
}

} // namespace

MagApi::MagApi(HwPlayer *player, QObject *parent)
    : QObject(parent)
    , player(player)
    , topWin(0)
{
}

void MagApi::loadIdentity()
{
    configDirectory = QCoreApplication::applicationDirPath() + "/config";
    QDir().mkpath(configDirectory);
    // Corrupt early SaveUserData wrote userdata.dat and broke portal UI — drop it.
    QFile::remove(configDirectory + QStringLiteral("/userdata.dat"));
    QFile::remove(configDirectory + QStringLiteral("/userdata.dat.tmp"));

    QVariantMap settings;
    readKodiSettings(settings);

    QJsonObject config;
    QFile configFile(configDirectory + "/config.json");
    if (configFile.open(QIODevice::ReadOnly)) {
        config = QJsonDocument::fromJson(configFile.readAll()).object();
        configFile.close();
    }

    auto pick = [&](const char *jsonKey, const char *kodiKey, const QString &fallback) {
        QString fromJson = config.value(jsonKey).toString().trimmed();
        if (!fromJson.isEmpty())
            return fromJson;
        QString fromKodi = settings.value(kodiKey).toString().trimmed();
        if (!fromKodi.isEmpty())
            return fromKodi;
        return fallback;
    };

    model = pick("model", "mag_model", "MAG254");
    if (model != "MAG250" && model != "MAG254" && model != "MAG256" && model != "MAG322")
        model = "MAG254";
    mac = pick("mac", "mac_address", macFromSysfs());
    if (mac.isEmpty())
        mac = "00:1A:79:00:00:01";
    serial = pick("serial", "serial_number", "SN" + QString(mac).remove(':'));
    deviceId = pick("device_id", "device_id", QString(mac).remove(':'));
    deviceId2 = pick("device_id2", "device_id2", QString());
    signature = pick("signature", "signature", QString());
    timezone = pick("timezone", "timezone", "Europe/Sofia");
    language = pick("language", "language", "bg");
    portal = normalizePortal(pick("portal", "portal_url", "http://portal.example.com"));
    ipAddress = ipFromInterface();

    ImageInfo image = imageFor(model);
    imageVersion = image.version;
    imageDescription = image.description;
    imageDate = image.date;
    hardwareVersion = image.hardware;

    QFile envFile(configDirectory + "/env.json");
    if (envFile.open(QIODevice::ReadOnly)) {
        QJsonObject stored = QJsonDocument::fromJson(envFile.readAll()).object();
        envFile.close();
        for (auto it = stored.begin(); it != stored.end(); ++it)
            env.insert(it.key(), it.value().toString());
    }
    env.insert("portal1", portal);
    if (!env.contains("portal2"))
        env.insert("portal2", "");
    if (!env.contains("language"))
        env.insert("language", language);
    if (!env.contains("timezone"))
        env.insert("timezone", timezone);
    if (!env.contains("timezone_conf"))
        env.insert("timezone_conf", timezone);
    env.insert("ntpurl", env.value("ntpurl", "pool.ntp.org").toString());
    env.insert("graphicres", "1280");
    env.insert("tvsystem", env.value("tvsystem", "1080p-50").toString());
    env.insert("ethaddr", mac);

    fprintf(stderr, "portal %s model %s mac %s serial %s ip %s\n",
        portal.toUtf8().constData(), model.toUtf8().constData(),
        mac.toUtf8().constData(), serial.toUtf8().constData(),
        ipAddress.toUtf8().constData());
}

QString MagApi::portalUrl() const
{
    return portal;
}

void MagApi::setPortalUrl(const QString &url)
{
    portal = normalizePortal(url);
    env.insert(QStringLiteral("portal1"), portal);
    saveEnvFile();
}

QString MagApi::userAgent() const
{
    return userAgentFor(model);
}

QString MagApi::envValue(const QString &key) const
{
    return env.value(key).toString();
}

void MagApi::rememberEnv(const QString &key, const QString &value)
{
    env.insert(key, value);
    saveEnvFile();
}

void MagApi::saveEnvFile() const
{
    QJsonObject object;
    for (auto it = env.cbegin(); it != env.cend(); ++it)
        object.insert(it.key(), it.value().toString());
    QFile file(configDirectory + "/env.json");
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return;
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
}

bool MagApi::Play(const QString &playStr, const QString &proxyParams)
{
    fprintf(stderr, "Play %s\n", playStr.toUtf8().constData());
    bool ok = player->commandPlay(playStr, proxyParams);
    emit layoutChanged();
    return ok;
}

bool MagApi::PlaySolution(const QString &solution, const QString &url)
{
    return Play(solution + " " + url);
}

bool MagApi::Stop()
{
    player->commandStop();
    emit layoutChanged();
    return true;
}

bool MagApi::Pause()
{
    player->commandPause();
    return true;
}

bool MagApi::Continue()
{
    player->commandContinue();
    return true;
}

bool MagApi::IsPlaying()
{
    return player->isPlaying();
}

int MagApi::GetPosTime()
{
    return player->positionSeconds();
}

int MagApi::GetPosTimeEx()
{
    return player->positionMs();
}

bool MagApi::SetPosTime(int seconds)
{
    player->seekSeconds(seconds);
    return true;
}

bool MagApi::SetPosTimeEx(int ms)
{
    player->seekSeconds(ms / 1000);
    return true;
}

int MagApi::GetPosPercent()
{
    return player->positionPercent();
}

bool MagApi::SetPosPercent(int percent)
{
    int duration = player->durationSeconds();
    if (duration > 0)
        player->seekSeconds(duration * percent / 100);
    return true;
}

int MagApi::GetMediaLen()
{
    return player->durationSeconds();
}

int MagApi::GetMediaLenEx()
{
    return player->durationMs();
}

int MagApi::GetVolume()
{
    return player->volume();
}

bool MagApi::SetVolume(int volume)
{
    player->setVolume(volume);
    rememberEnv("volume", QString::number(player->volume()));
    return true;
}

bool MagApi::GetMute()
{
    return player->muted();
}

bool MagApi::SetMute(int mute)
{
    player->setMuted(mute != 0);
    return true;
}

bool MagApi::SetViewport(int width, int height, int x, int y)
{
    fprintf(stderr, "SetViewport %d %d %d %d\n", width, height, x, y);
    if (width > 0 && height > 0 && width <= 1280 && height <= 720) {
        x = x * 3 / 2;
        y = y * 3 / 2;
        width = width * 3 / 2;
        height = height * 3 / 2;
    }
    player->setViewport(width, height, x, y);
    emit layoutChanged();
    return true;
}

bool MagApi::SetViewportEx(int, int, int, int)
{
    return true;
}

bool MagApi::SetTopWin(int win)
{
    fprintf(stderr, "SetTopWin %d\n", win);
    topWin = win;
    emit layoutChanged();
    return true;
}

int MagApi::GetTopWin()
{
    return topWin;
}

QString MagApi::GetDeviceMacAddress() { return mac; }
QString MagApi::GetDeviceSerialNumber() { return serial; }
QString MagApi::GetDeviceModel() { return model; }
QString MagApi::GetDeviceModelExt() { return model; }
QString MagApi::GetDeviceVendor() { return "Infomir"; }
QString MagApi::GetDeviceVersionHardware() { return hardwareVersion; }
QString MagApi::GetDeviceImageVersion() { return imageVersion; }
QString MagApi::GetDeviceImageDesc() { return imageDescription; }
QString MagApi::GetDeviceImageVersionCurrent() { return imageDescription + " " + imageDate; }
QString MagApi::GetDeviceActiveBank() { return "0"; }
bool MagApi::GetLanLinkStatus() { return true; }
bool MagApi::GetWifiLinkStatus() { return false; }
int MagApi::GetHDMIConnectionState() { return 2; }
bool MagApi::GetStandByStatus() { return standByActive; }

QString MagApi::RDir(const QString &name)
{
    if (name == "SerialNumber" || name == "stb_sn") return serial;
    if (name == "MACAddress" || name == "stb_mac") return mac;
    if (name == "IPAddress" || name == "WiFi_ip") return ipAddress;
    if (name == "HardwareVersion") return hardwareVersion;
    if (name == "Vendor") return "Infomir";
    if (name == "Model" || name == "stb_type") return model;
    if (name == "ImageVersion") return imageVersion;
    if (name == "ImageDescription") return imageDescription;
    if (name == "ImageDate") return imageDate;
    if (name == "get_storage_info") return GetStorageInfo();
    if (name == "vmode" || name == "gmode") return QStringLiteral("720");
    return QString();
}

bool MagApi::previewHole(QRect *rect) const
{
    if (!player->isPlaying())
        return false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    player->viewport(&width, &height, &x, &y);
    // Fullscreen needs no hole; small PiP must punch through the OSD.
    if (width < 32 || height < 32 || width > 1400 || height > 900)
        return false;
    if (rect)
        *rect = QRect(x, y, width, height);
    return true;
}

QString MagApi::GetEnv(const QString &data)
{
    QStringList requested;
    QJsonDocument doc = QJsonDocument::fromJson(data.toUtf8());
    if (doc.isObject()) {
        QJsonArray list = doc.object().value("varList").toArray();
        for (const QJsonValue &item : list)
            requested.append(item.toString());
    }
    QJsonObject result;
    for (const QString &name : requested)
        result.insert(name, envValue(name));
    QJsonObject payload;
    payload.insert("result", result);
    payload.insert("errMsg", "");
    payload.insert("errCode", 0);
    return QString::fromUtf8(QJsonDocument(payload).toJson(QJsonDocument::Compact));
}

bool MagApi::SetEnv(const QString &data)
{
    QJsonDocument doc = QJsonDocument::fromJson(data.toUtf8());
    if (!doc.isObject())
        return false;
    const QJsonObject object = doc.object();
    for (auto it = object.begin(); it != object.end(); ++it)
        env.insert(it.key(), it.value().toVariant().toString());
    saveEnvFile();
    return true;
}

bool MagApi::SetEnv(const QString &data, const QString &value)
{
    rememberEnv(data, value);
    return true;
}

QString MagApi::GetUID()
{
    return deviceId;
}

QString MagApi::GetUID(const QString &)
{
    return signature;
}

QString MagApi::GetUID(const QString &, const QString &)
{
    return deviceId2;
}

QString MagApi::GetHashVersion1(const QString &secret, const QString &key)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    QByteArray secretBytes = secret.toUtf8();
    QByteArray keyBytes = key.toUtf8();
    HMAC(EVP_sha1(), secretBytes.constData(), secretBytes.size(),
        reinterpret_cast<const unsigned char *>(keyBytes.constData()), (size_t)keyBytes.size(),
        digest, &length);
    return QByteArray(reinterpret_cast<char *>(digest), (int)length).toHex();
}

QString MagApi::Version()
{
    return "JS API version: 328; STB API version: 134; Player Engine version: 0x566";
}

bool MagApi::ExecAction(const QString &action)
{
    fprintf(stderr, "ExecAction %s\n", action.toUtf8().constData());
    const QString cmd = action.trimmed();
    if (cmd.startsWith(QLatin1String("graphicres"))) {
        QString value = cmd.section(' ', 1, 1);
        if (!value.isEmpty())
            rememberEnv("graphicres", value);
        return true;
    }
    if (cmd.compare(QLatin1String("reboot"), Qt::CaseInsensitive) == 0
        || cmd.startsWith(QLatin1String("reboot "))) {
        // Settings "Reboot device" → confirm OK calls ExecAction("reboot").
        QTimer::singleShot(500, qApp, []() {
            fprintf(stderr, "reboot now\n");
            if (!QProcess::startDetached(QStringLiteral("/sbin/reboot"), QStringList()))
                QProcess::startDetached(QStringLiteral("reboot"), QStringList());
        });
        return true;
    }
    return true;
}
QString MagApi::GetVideoInfo()
{
    return "{\"frameRate\":0,\"frameWidth\":1920,\"frameHeight\":1080,\"pictureWidth\":1920,\"pictureHeight\":1080,\"aspectRatio\":1.777}";
}
QString MagApi::GetAudioPIDs() { return "[]"; }
QString MagApi::GetAudioPIDsEx() { return "[]"; }
QString MagApi::GetSubtitlePIDs() { return "[]"; }
QString MagApi::GetMetadataInfo() { return "{}"; }
int MagApi::GetBufferLoad() { return 100; }
int MagApi::GetAspect() { return 0; }
int MagApi::GetSpeed() { return 1; }
int MagApi::GetAlphaLevel() { return 255; }
bool MagApi::Debug(const QString &msg)
{
    fprintf(stderr, "js %s\n", msg.toUtf8().constData());
    return true;
}
bool MagApi::InitPlayer() { return true; }
bool MagApi::DeinitPlayer() { return true; }
bool MagApi::LoadURL(const QString &url)
{
    // Stalker builds settings URL with nested ? in ajax_loader; normalize like Infomir JS.
    QString fixed = url;
    const int firstQ = fixed.indexOf(QLatin1Char('?'));
    const int lastQ = fixed.lastIndexOf(QLatin1Char('?'));
    if (firstQ >= 0 && lastQ > firstQ)
        fixed[lastQ] = QLatin1Char('&');
    fprintf(stderr, "LoadURL %s\n", fixed.toUtf8().constData());
    emit loadRequested(fixed);
    return true;
}
bool MagApi::SetWebReturnUrl(const QString &url)
{
    webReturnUrl = url.trimmed();
    fprintf(stderr, "SetWebReturnUrl %s\n", webReturnUrl.toUtf8().constData());
    return true;
}
QString MagApi::GetWebReturnUrl()
{
    return webReturnUrl;
}
bool MagApi::ClearWebReturnUrl()
{
    webReturnUrl.clear();
    return true;
}
bool MagApi::ShowVideoImmediately() { return true; }
QString MagApi::GetHLSInfo() { return "{\"currentVariant\":0,\"variants\":[]}"; }
QString MagApi::GetStatistics() { return "{}"; }
bool MagApi::ClearStatistics() { return true; }
bool MagApi::EnableSpatialNavigation() { return true; }
bool MagApi::IsVirtualKeyboardActive() { return false; }
namespace {

void purgeLegacyUserData(const QString &configDirectory)
{
    // Bad 1-arg SaveUserData("ad.json") wrote the filename as content and broke UI.
    QFile::remove(configDirectory + QStringLiteral("/userdata.dat"));
    QFile::remove(configDirectory + QStringLiteral("/userdata.dat.tmp"));
    QFile::remove(configDirectory + QStringLiteral("/userdata.v1"));
    QFile::remove(configDirectory + QStringLiteral("/userdata.v1.tmp"));
}

QString sanitizeUserDataName(QString name)
{
    name = name.trimmed();
    if (name.isEmpty())
        name = QStringLiteral("default");
    for (int i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        const bool ok = c.isLetterOrNumber() || c == QLatin1Char('.') || c == QLatin1Char('_')
            || c == QLatin1Char('-');
        if (!ok)
            name[i] = QLatin1Char('_');
    }
    return name;
}

QString userDataFilePath(const QString &configDirectory, const QString &name)
{
    return configDirectory + QStringLiteral("/userdata/") + sanitizeUserDataName(name);
}

} // namespace

QString MagApi::LoadUserData()
{
    return LoadUserData(QStringLiteral("default"));
}

QString MagApi::LoadUserData(const QString &name)
{
    purgeLegacyUserData(configDirectory);
    QFile file(userDataFilePath(configDirectory, name));
    if (!file.open(QIODevice::ReadOnly)) {
        fprintf(stderr, "LoadUserData(%s) empty\n", name.toUtf8().constData());
        return QString();
    }
    const QByteArray raw = file.readAll();
    file.close();
    fprintf(stderr, "LoadUserData(%s) %d bytes\n", name.toUtf8().constData(), raw.size());
    return QString::fromUtf8(raw);
}

bool MagApi::SaveUserData(const QString &data)
{
    // Portal sometimes calls SaveUserData(filename, payload). If only one arg
    // arrives and it looks like a filename, ignore (do not store the name as data).
    if (data.endsWith(QLatin1String(".json")) || data.endsWith(QLatin1String(".txt"))
        || data.endsWith(QLatin1String(".dat"))) {
        fprintf(stderr, "SaveUserData skip filename-only arg: %s\n", data.toUtf8().constData());
        return true;
    }
    return SaveUserData(QStringLiteral("default"), data);
}

bool MagApi::SaveUserData(const QString &name, const QString &data)
{
    purgeLegacyUserData(configDirectory);
    if (data.startsWith(QLatin1String("[object")) || data == QLatin1String("undefined")) {
        fprintf(stderr, "SaveUserData(%s) skip non-string JS value\n", name.toUtf8().constData());
        return true;
    }
    const QByteArray raw = data.toUtf8();
    if (raw.size() > 64 * 1024) {
        fprintf(stderr, "SaveUserData(%s) too large (%d)\n", name.toUtf8().constData(), raw.size());
        return false;
    }

    const QString dir = configDirectory + QStringLiteral("/userdata");
    QDir().mkpath(dir);
    const QString path = userDataFilePath(configDirectory, name);
    const QString tmp = path + QStringLiteral(".tmp");
    QFile file(tmp);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        fprintf(stderr, "SaveUserData(%s) open failed\n", name.toUtf8().constData());
        return false;
    }
    if (file.write(raw) != raw.size()) {
        file.close();
        QFile::remove(tmp);
        return false;
    }
    file.close();
    QFile::remove(path);
    if (!QFile::rename(tmp, path)) {
        fprintf(stderr, "SaveUserData(%s) rename failed\n", name.toUtf8().constData());
        return false;
    }
    fprintf(stderr, "SaveUserData(%s) %d bytes\n", name.toUtf8().constData(), raw.size());
    return true;
}
bool MagApi::IsFileExist(const QString &) { return false; }
QString MagApi::ListDir(const QString &) { return "var dirs = []; var files = [];"; }
QString MagApi::GetStorageInfo() { return "{\"result\":[],\"errCode\":0,\"errMsg\":\"\"}"; }
QString MagApi::GetExtProtocolList() { return "[]"; }
bool MagApi::SetLoop(int) { return true; }
bool MagApi::SetSpeed(int) { return true; }
bool MagApi::SetAspect(int) { return true; }
bool MagApi::SetAudioPID(int) { return true; }
int MagApi::GetAudioPID() { return 0; }
bool MagApi::SetSubtitlePID(int) { return true; }
int MagApi::GetSubtitlePID() { return 0; }
bool MagApi::SetSubtitles(int) { return true; }
bool MagApi::SetMode(int) { return true; }
bool MagApi::SetPIG(int mode, int x, int y, int width)
{
    fprintf(stderr, "SetPIG %d %d %d %d\n", mode, x, y, width);
    if (mode == 0)
        return true;
    if (x < 0 || y < 0 || width < 0) {
        player->setViewport(1920, 1080, 0, 0);
        emit layoutChanged();
        return true;
    }
    int height = width * 3 / 4;
    if (width > 0 && width <= 1280) {
        x = x * 3 / 2;
        y = y * 3 / 2;
        width = width * 3 / 2;
        height = height * 3 / 2;
    }
    player->setViewport(width, height, x, y);
    emit layoutChanged();
    return true;
}
bool MagApi::GetPIG() { return false; }
bool MagApi::SetWinAlphaLevel(int) { return true; }
bool MagApi::SetWinMode(int) { return true; }
bool MagApi::SetAlphaLevel(int) { return true; }
bool MagApi::SetTransparentColor(int) { return true; }
bool MagApi::SetChromaKey(int) { return true; }
bool MagApi::SetVideoControl(int) { return true; }
bool MagApi::SetVideoState(int) { return true; }
namespace {
void writeSysfs(const char *path, const char *value)
{
    QFile f(QString::fromLatin1(path));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return;
    f.write(value);
    f.write("\n");
    f.close();
}
} // namespace

bool MagApi::StandBy(int on)
{
    // Soft standby: stop A/V, blank OSD + HDMI. Keep Ethernet/Wi‑Fi up.
    // Full suspend-to-RAM is avoided — IR wake is unreliable on this SoC.
    if (on) {
        if (standByActive)
            return true;
        fprintf(stderr, "standby enter\n");
        player->commandStop();
        // Detach FB console so IR is not drawn as text on the black screen
        writeSysfs("/sys/class/vtconsole/vtcon1/bind", "0");
        writeSysfs("/sys/class/vtconsole/vtcon0/bind", "0");
        writeSysfs("/sys/class/graphics/fbcon/cursor_blink", "0");
        writeSysfs("/sys/class/video/disable_video", "1");
        writeSysfs("/sys/class/graphics/fb0/blank", "1");
        writeSysfs("/sys/class/graphics/fb1/blank", "1");
        writeSysfs("/sys/devices/virtual/amhdmitx/amhdmitx0/avmute", "1");
        writeSysfs("/sys/class/amhdmitx/amhdmitx0/avmute", "1");
        // Best-effort HDMI PHY off (ignored if unsupported)
        writeSysfs("/sys/devices/virtual/amhdmitx/amhdmitx0/phy", "0");
        standByActive = true;
    } else {
        if (!standByActive)
            return true;
        fprintf(stderr, "standby leave\n");
        writeSysfs("/sys/devices/virtual/amhdmitx/amhdmitx0/phy", "1");
        writeSysfs("/sys/devices/virtual/amhdmitx/amhdmitx0/avmute", "0");
        writeSysfs("/sys/class/amhdmitx/amhdmitx0/avmute", "0");
        writeSysfs("/sys/class/graphics/fb0/blank", "0");
        writeSysfs("/sys/class/graphics/fb1/blank", "0");
        writeSysfs("/sys/class/video/disable_video", "0");
        // Re-assert display mode used by Yasem UI
        writeSysfs("/sys/class/display/mode", "1080i50hz");
        standByActive = false;
        emit layoutChanged();
    }
    return true;
}
bool MagApi::SetScreenSaverTime(int) { return true; }
bool MagApi::ShowVirtualKeyboard() { return true; }
bool MagApi::HideVirtualKeyboard() { return true; }
bool MagApi::EnableVKButton(int enable)
{
    fprintf(stderr, "EnableVKButton %d\n", enable);
    return true;
}
bool MagApi::SetAutoFrameRate(int mode)
{
    fprintf(stderr, "SetAutoFrameRate %d\n", mode);
    return true;
}
bool MagApi::SendEventToPortal(const QString &json)
{
    fprintf(stderr, "SendEventToPortal %s\n", json.toUtf8().constData());
    return true;
}
bool MagApi::SetWebProxy(const QString &, int) { return true; }
bool MagApi::ResetWebProxy() { return true; }
