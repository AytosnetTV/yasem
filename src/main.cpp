#include "browser.h"
#include "configserver.h"
#include "magapi.h"
#include "player.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QNetworkInterface>
#include <QTimer>
#include <QUrl>
#include <QWebFrame>
#include <QWebSettings>
#include <QWebView>

#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <linux/fb.h>
#include <netdb.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <QAbstractSocket>
#include <QHostAddress>

static QApplication *application = nullptr;

static void handleSignal(int)
{
    if (application)
        application->quit();
}

static void panFramebuffer()
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0)
        return;
    fb_var_screeninfo info;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &info) == 0) {
        info.yoffset = 0;
        info.activate = FB_ACTIVATE_NOW;
        ioctl(fd, FBIOPAN_DISPLAY, &info);
    }
    close(fd);
}

int main(int argc, char *argv[])
{
    QString root = QFileInfo(argv[0]).absolutePath();
    qputenv("QT_QPA_PLATFORM", "linuxfb:fb=/dev/fb0");
    qputenv("QT_QPA_FONTDIR", (root + "/fonts").toUtf8());
    qputenv("QT_PLUGIN_PATH", (root + "/plugins").toUtf8());
    qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", (root + "/plugins/platforms").toUtf8());
    if (qEnvironmentVariableIsEmpty("QT_QPA_GENERIC_PLUGINS"))
        qputenv("QT_QPA_GENERIC_PLUGINS", "evdevkeyboard");
    panFramebuffer();

    QApplication app(argc, argv);
    application = &app;
    signal(SIGTERM, handleSignal);
    signal(SIGINT, handleSignal);

    QWebSettings *settings = QWebSettings::globalSettings();
    settings->setAttribute(QWebSettings::JavascriptEnabled, true);
    settings->setAttribute(QWebSettings::LocalStorageEnabled, true);
    settings->setAttribute(QWebSettings::OfflineStorageDatabaseEnabled, true);
    settings->setAttribute(QWebSettings::OfflineWebApplicationCacheEnabled, true);
    settings->setAttribute(QWebSettings::PluginsEnabled, false);
    settings->setAttribute(QWebSettings::AcceleratedCompositingEnabled, false);
    settings->setAttribute(QWebSettings::WebGLEnabled, false);
    settings->setAttribute(QWebSettings::LocalContentCanAccessRemoteUrls, true);
    settings->setAttribute(QWebSettings::DnsPrefetchEnabled, true);
    settings->setObjectCacheCapacities(1024 * 1024, 4 * 1024 * 1024, 8 * 1024 * 1024);
    settings->setMaximumPagesInCache(2);
    settings->setFontFamily(QWebSettings::StandardFont, "DejaVu Sans");
    settings->setFontSize(QWebSettings::DefaultFontSize, 18);

    HwPlayer player;
    MagApi api(&player);
    api.loadIdentity();
    QString storage = api.configDir();
    settings->setLocalStoragePath(storage);
    settings->setOfflineStoragePath(storage);
    settings->enablePersistentStorage(storage);

    MagView view(&api);
    view.setFocus();
    view.showFullScreen();
    view.setHtml(QStringLiteral("<html><body style='margin:0;background:#000'></body></html>"));

    ConfigServer configServer(&api);
    configServer.listen(80);

    QObject::connect(&api, &MagApi::loadRequested, &view, [&view](const QString &url) {
        view.load(QUrl(url));
    });
    QObject::connect(&player, &HwPlayer::playbackEvent, &view, [&view](int code) {
        view.page()->mainFrame()->evaluateJavaScript(
            QString("if(window.stbEvent){stbEvent.event=%1;if(typeof stbEvent.onEvent==='function')stbEvent.onEvent(%1);}")
                .arg(code));
        view.update();
    });

    auto hasLanIp = []() -> bool {
        const auto addrs = QNetworkInterface::allAddresses();
        for (const QHostAddress &addr : addrs) {
            if (addr.protocol() != QAbstractSocket::IPv4Protocol || addr.isLoopback())
                continue;
            return true;
        }
        return false;
    };
    auto hostResolves = [](const QString &host) -> bool {
        if (host.isEmpty())
            return false;
        addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo *res = nullptr;
        const int rc = getaddrinfo(host.toUtf8().constData(), nullptr, &hints, &res);
        if (res)
            freeaddrinfo(res);
        return rc == 0;
    };

    QElapsedTimer bootClock;
    bootClock.start();
    bool *portalOk = new bool(false);
    bool *showingStatus = new bool(false);
    int *attempts = new int(0);

    auto statusReason = [&]() -> QString {
        if (!hasLanIp())
            return QStringLiteral("Няма мрежов IP — провери кабела или настройките");
        return QStringLiteral("Мрежата е активна. Порталът ще се зареди след рестарт на бокса.");
    };

    auto showStatus = [&view, &configServer, showingStatus](const QString &reason) {
        *showingStatus = true;
        fprintf(stderr, "show status page: %s\n", reason.toUtf8().constData());
        view.setHtml(configServer.statusPageHtml(reason));
    };

    auto tryLoadPortal = [&]() {
        // Once status page is locked, never auto-start portal until full box reboot.
        if (*showingStatus)
            return false;
        const QString portal = api.portalUrl();
        const QUrl url(portal);
        const QString host = url.host();
        if (!hasLanIp() || !hostResolves(host))
            return false;
        fprintf(stderr, "load %s\n", portal.toUtf8().constData());
        view.load(url);
        return true;
    };

    QTimer *portalTimer = new QTimer(&view);
    QObject::connect(portalTimer, &QTimer::timeout, &view, [&]() {
        ++(*attempts);
        if (*portalOk)
            return;

        // Locked on status page: only refresh IP/MAC/etc., do not load portal.
        if (*showingStatus) {
            if ((*attempts % 2) == 0)
                showStatus(statusReason());
            if (portalTimer->interval() != 5000)
                portalTimer->setInterval(5000);
            return;
        }

        if (tryLoadPortal()) {
            portalTimer->stop();
            return;
        }

        // After 10s without portal — lock onto MAG status until reboot
        if (bootClock.elapsed() >= 10000) {
            showStatus(statusReason());
            portalTimer->setInterval(5000);
            return;
        }

        if ((*attempts % 5) == 1) {
            fprintf(stderr, "wait network/dns for %s (try %d, %lld ms)\n",
                api.portalUrl().toUtf8().constData(), *attempts,
                static_cast<long long>(bootClock.elapsed()));
        }
    });

    QObject::connect(&view, &QWebView::loadFinished, &view, [&](bool ok) {
        if (*showingStatus)
            return;
        if (ok) {
            *portalOk = true;
            *attempts = 0;
            fprintf(stderr, "portal load ok\n");
            return;
        }
        fprintf(stderr, "portal load failed, retry (%d)\n", *attempts);
        if (bootClock.elapsed() >= 10000 && !*showingStatus) {
            showStatus(statusReason());
            portalTimer->setInterval(5000);
        }
        if (!portalTimer->isActive())
            portalTimer->start(2000);
    });

    QObject::connect(&configServer, &ConfigServer::portalChanged, &view, [&](const QString &portal) {
        fprintf(stderr, "portal changed -> %s\n", portal.toUtf8().constData());
        if (*showingStatus) {
            // Stay on status until reboot; just refresh shown portal URL.
            showStatus(QStringLiteral("Порталът е записан. Рестартирай бокса за зареждане."));
            return;
        }
        *portalOk = false;
        bootClock.restart();
        *attempts = 0;
        portalTimer->setInterval(1000);
        if (!portalTimer->isActive())
            portalTimer->start();
        tryLoadPortal();
    });

    portalTimer->setInterval(1000);
    portalTimer->start();

    int code = app.exec();
    application = nullptr;
    return code;
}
