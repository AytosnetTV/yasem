#include "configserver.h"
#include "magapi.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QProcess>
#include <QRegExp>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>

#include <cstdio>

namespace {

QString runCmd(const QStringList &args, int timeoutMs = 8000)
{
    QProcess p;
    p.start(args.value(0), args.mid(1));
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        return QString();
    }
    return QString::fromUtf8(p.readAllStandardOutput() + p.readAllStandardError());
}

QString pickConnmanService()
{
    const QString out = runCmd(QStringList() << "connmanctl" << "services");
    // Prefer online (*AO / *AR), else first ethernet/wifi id
    QString fallback;
    for (const QString &line : out.split('\n')) {
        const QString t = line.trimmed();
        if (t.isEmpty())
            continue;
        const QStringList parts = t.split(QRegExp("\\s+"), QString::SkipEmptyParts);
        if (parts.isEmpty())
            continue;
        const QString id = parts.last();
        if (!id.startsWith("ethernet_") && !id.startsWith("wifi_"))
            continue;
        if (t.startsWith("*AO") || t.startsWith("*AR") || t.startsWith("*A"))
            return id;
        if (fallback.isEmpty())
            fallback = id;
    }
    return fallback;
}

QString kv(const QString &block, const QString &key)
{
    // Match: Key = value   or Key = [ ... ]
    const QRegExp re(QString("^\\s*%1\\s*=\\s*(.*)$").arg(QRegExp::escape(key)));
    for (const QString &line : block.split('\n')) {
        if (re.indexIn(line) == 0)
            return re.cap(1).trimmed();
    }
    return QString();
}

QString bracketField(const QString &bracket, const QString &field)
{
    // [ Method=dhcp, Address=1.2.3.4, ... ]
    const QRegExp re(QString("%1=([^,\\]]+)").arg(QRegExp::escape(field)));
    if (re.indexIn(bracket) >= 0)
        return re.cap(1).trimmed();
    return QString();
}

} // namespace

ConfigServer::ConfigServer(MagApi *api, QObject *parent)
    : QObject(parent)
    , api(api)
    , server(new QTcpServer(this))
{
    connect(server, &QTcpServer::newConnection, this, &ConfigServer::onNewConnection);
}

bool ConfigServer::listen(quint16 port)
{
    if (server->isListening())
        return true;
    if (!server->listen(QHostAddress::Any, port)) {
        fprintf(stderr, "configserver: listen :%u failed: %s\n",
            port, server->errorString().toUtf8().constData());
        // Fallback 8080 if 80 busy
        if (port == 80 && server->listen(QHostAddress::Any, 8080)) {
            fprintf(stderr, "configserver: listening on :8080\n");
            return true;
        }
        return false;
    }
    fprintf(stderr, "configserver: listening on :%u\n", server->serverPort());
    return true;
}

void ConfigServer::onNewConnection()
{
    while (QTcpSocket *sock = server->nextPendingConnection()) {
        buffers.insert(sock, QByteArray());
        connect(sock, &QTcpSocket::readyRead, this, &ConfigServer::onReadyRead);
        connect(sock, &QTcpSocket::disconnected, this, [this, sock]() {
            buffers.remove(sock);
            sock->deleteLater();
        });
    }
}

void ConfigServer::onReadyRead()
{
    QTcpSocket *sock = qobject_cast<QTcpSocket *>(sender());
    if (!sock)
        return;
    buffers[sock].append(sock->readAll());
    const QByteArray &buf = buffers[sock];
    const int hdrEnd = buf.indexOf("\r\n\r\n");
    if (hdrEnd < 0) {
        if (buf.size() > 65536)
            sock->disconnectFromHost();
        return;
    }
    const QByteArray header = buf.left(hdrEnd);
    const QList<QByteArray> lines = header.split('\n');
    if (lines.isEmpty()) {
        sock->disconnectFromHost();
        return;
    }
    const QList<QByteArray> req = lines.first().trimmed().split(' ');
    if (req.size() < 2) {
        sock->disconnectFromHost();
        return;
    }
    const QString method = QString::fromLatin1(req[0]).toUpper();
    QString path = QString::fromUtf8(req[1]);
    int contentLength = 0;
    for (const QByteArray &line : lines) {
        const QByteArray l = line.trimmed();
        if (l.toLower().startsWith("content-length:"))
            contentLength = l.mid(15).trimmed().toInt();
    }
    const QByteArray body = buf.mid(hdrEnd + 4);
    if (body.size() < contentLength)
        return; // wait for more

    const int q = path.indexOf('?');
    if (q >= 0)
        path = path.left(q);

    QByteArray response = handleRequest(method, path, body.left(contentLength));
    sock->write(response);
    sock->disconnectFromHost();
    buffers.remove(sock);
}

QHash<QString, QString> ConfigServer::parseForm(const QByteArray &body)
{
    QHash<QString, QString> out;
    QUrlQuery q(QString::fromUtf8(body));
    for (const auto &p : q.queryItems(QUrl::FullyDecoded))
        out.insert(p.first, p.second);
    return out;
}

QString ConfigServer::htmlEscape(const QString &s)
{
    QString o = s;
    o.replace('&', "&amp;");
    o.replace('<', "&lt;");
    o.replace('>', "&gt;");
    o.replace('"', "&quot;");
    return o;
}

ConfigServer::NetInfo ConfigServer::readNet() const
{
    NetInfo n;
    n.service = pickConnmanService();
    n.mac = api ? api->GetDeviceMacAddress() : QString();

    // Prefer live interface IP
    const auto addrs = QNetworkInterface::allAddresses();
    for (const QHostAddress &a : addrs) {
        if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback()) {
            n.ip = a.toString();
            break;
        }
    }
    if (n.mac.isEmpty()) {
        for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
            if (iface.flags() & QNetworkInterface::IsLoopBack)
                continue;
            if (!(iface.flags() & QNetworkInterface::IsUp))
                continue;
            const QString hw = iface.hardwareAddress().toUpper();
            if (hw.size() == 17) {
                n.mac = hw;
                n.iface = iface.name();
                break;
            }
        }
    }

    if (n.service.isEmpty()) {
        n.method = "unknown";
        n.state = "n/a";
        return n;
    }

    const QString block = runCmd(QStringList() << "connmanctl" << "services" << n.service);
    n.state = kv(block, "State");
    const QString eth = kv(block, "Ethernet");
    if (!eth.isEmpty()) {
        n.iface = bracketField(eth, "Interface");
        const QString addr = bracketField(eth, "Address");
        if (!addr.isEmpty())
            n.mac = addr.toUpper();
    }
    const QString ipv4 = kv(block, "IPv4");
    const QString ipv4cfg = kv(block, "IPv4.Configuration");
    n.method = bracketField(ipv4cfg.isEmpty() ? ipv4 : ipv4cfg, "Method");
    if (n.method.isEmpty())
        n.method = "unknown";
    if (n.ip.isEmpty())
        n.ip = bracketField(ipv4, "Address");
    n.netmask = bracketField(ipv4, "Netmask");
    n.gateway = bracketField(ipv4, "Gateway");
    QString dns = kv(block, "Nameservers");
    if (dns.startsWith('[')) {
        dns = dns.mid(1);
        if (dns.endsWith(']'))
            dns.chop(1);
        dns = dns.trimmed();
    }
    n.dns = dns;
    n.link = (n.state == "online" || n.state == "ready");
    return n;
}

QString ConfigServer::pageShell(const QString &title, const QString &body) const
{
    return QStringLiteral(
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>%1</title>"
        "<style>"
        "html,body{margin:0;padding:0;background:#0b1c3a;color:#e8eef8;"
        "font-family:DejaVu Sans,Arial,sans-serif;}"
        ".wrap{max-width:920px;margin:0 auto;padding:28px 24px 48px;}"
        "h1{font-size:28px;font-weight:700;margin:0 0 8px;color:#fff;}"
        ".sub{color:#8fa6c9;margin:0 0 24px;font-size:15px;}"
        ".card{background:linear-gradient(180deg,#14305a,#102848);border:1px solid #2a4a7a;"
        "border-radius:4px;padding:18px 20px;margin:0 0 16px;}"
        ".row{display:flex;justify-content:space-between;gap:16px;padding:8px 0;"
        "border-bottom:1px solid rgba(255,255,255,.08);font-size:16px;}"
        ".row:last-child{border-bottom:0;}"
        ".k{color:#8fa6c9;}"
        ".v{color:#fff;font-weight:600;text-align:right;word-break:break-all;}"
        "a.btn,button,.btn{display:inline-block;background:#e67e22;color:#fff;text-decoration:none;"
        "border:0;border-radius:3px;padding:12px 18px;font-size:16px;font-weight:700;cursor:pointer;}"
        "a.btn.secondary{background:#2a4a7a;}"
        "label{display:block;margin:12px 0 4px;color:#8fa6c9;font-size:14px;}"
        "input,select{width:100%;box-sizing:border-box;padding:10px 12px;border-radius:3px;"
        "border:1px solid #2a4a7a;background:#0b1c3a;color:#fff;font-size:16px;}"
        ".actions{margin-top:18px;display:flex;flex-wrap:wrap;gap:10px;}"
        ".ok{background:#1e4d2b;border-color:#2f7a45;color:#b6f0c4;padding:12px;margin-bottom:14px;}"
        ".err{background:#4d1e1e;border-color:#7a2f2f;color:#f0b6b6;padding:12px;margin-bottom:14px;}"
        ".hint{color:#8fa6c9;font-size:13px;margin-top:8px;}"
        "</style></head><body><div class='wrap'>%2</div></body></html>"
    ).arg(htmlEscape(title), body);
}

QString ConfigServer::statusPageHtml(const QString &reason) const
{
    const NetInfo n = readNet();
    const QString portal = api ? api->portalUrl() : QString();
    const QString ip = n.ip.isEmpty() ? QStringLiteral("—") : n.ip;
    const QString mac = n.mac.isEmpty() ? QStringLiteral("—") : n.mac;
    const QString configUrl = n.ip.isEmpty()
        ? QStringLiteral("http://<IP>/config")
        : QStringLiteral("http://%1/config").arg(n.ip);
    const QString why = reason.isEmpty()
        ? QStringLiteral("Изчакване на мрежа или портал")
        : reason;

    QString body;
    body += QStringLiteral("<h1>Yasem</h1>");
    body += QStringLiteral("<p class='sub'>%1</p>").arg(htmlEscape(why));
    body += QStringLiteral("<div class='card'>");
    body += QStringLiteral("<div class='row'><span class='k'>Състояние</span><span class='v'>%1</span></div>")
                .arg(htmlEscape(n.state.isEmpty() ? QStringLiteral("няма връзка") : n.state));
    body += QStringLiteral("<div class='row'><span class='k'>IP адрес</span><span class='v'>%1</span></div>")
                .arg(htmlEscape(ip));
    body += QStringLiteral("<div class='row'><span class='k'>MAC адрес</span><span class='v'>%1</span></div>")
                .arg(htmlEscape(mac));
    body += QStringLiteral("<div class='row'><span class='k'>Интерфейс</span><span class='v'>%1</span></div>")
                .arg(htmlEscape(n.iface.isEmpty() ? QStringLiteral("—") : n.iface));
    body += QStringLiteral("<div class='row'><span class='k'>Режим</span><span class='v'>%1</span></div>")
                .arg(htmlEscape(n.method));
    body += QStringLiteral("<div class='row'><span class='k'>Шлюз</span><span class='v'>%1</span></div>")
                .arg(htmlEscape(n.gateway.isEmpty() ? QStringLiteral("—") : n.gateway));
    body += QStringLiteral("<div class='row'><span class='k'>DNS</span><span class='v'>%1</span></div>")
                .arg(htmlEscape(n.dns.isEmpty() ? QStringLiteral("—") : n.dns));
    body += QStringLiteral("<div class='row'><span class='k'>Портал</span><span class='v'>%1</span></div>")
                .arg(htmlEscape(portal));
    body += QStringLiteral("</div>");
    body += QStringLiteral("<div class='actions'>");
    body += QStringLiteral("<a class='btn' href='%1'>Настройки /config</a>").arg(htmlEscape(configUrl));
    body += QStringLiteral("<a class='btn secondary' href='/'>Обнови</a>");
    body += QStringLiteral("</div>");
    body += QStringLiteral("<p class='hint'>Отвори от телефон/PC: <b>%1</b></p>")
                .arg(htmlEscape(configUrl));
    body += QStringLiteral("<p class='hint'>Страницата се показва ако мрежата/порталът не са готови за ~10 сек. "
                           "След възстановяване на мрежата тази страница се обновява, но порталът "
                           "<b>не</b> се стартира — нужен е рестарт на бокса.</p>");
    return pageShell(QStringLiteral("Yasem — мрежа"), body);
}

QString ConfigServer::configPageHtml(const QString &flash) const
{
    const NetInfo n = readNet();
    const QString portal = api ? api->portalUrl() : QString();
    const bool isDhcp = (n.method.compare(QStringLiteral("dhcp"), Qt::CaseInsensitive) == 0
                         || n.method.compare(QStringLiteral("auto"), Qt::CaseInsensitive) == 0);

    QString body;
    body += QStringLiteral("<h1>Настройки</h1>");
    body += QStringLiteral("<p class='sub'>Мрежа · Портал · Рестарт</p>");
    if (!flash.isEmpty()) {
        const bool err = flash.startsWith(QStringLiteral("ERR:"));
        body += QStringLiteral("<div class='%1'>%2</div>")
                    .arg(err ? QStringLiteral("err") : QStringLiteral("ok"),
                         htmlEscape(err ? flash.mid(4) : flash));
    }

    body += QStringLiteral("<div class='card'>");
    body += QStringLiteral("<div class='row'><span class='k'>IP</span><span class='v'>%1</span></div>")
                .arg(htmlEscape(n.ip.isEmpty() ? QStringLiteral("—") : n.ip));
    body += QStringLiteral("<div class='row'><span class='k'>MAC</span><span class='v'>%1</span></div>")
                .arg(htmlEscape(n.mac.isEmpty() ? QStringLiteral("—") : n.mac));
    body += QStringLiteral("<div class='row'><span class='k'>Състояние</span><span class='v'>%1</span></div>")
                .arg(htmlEscape(n.state.isEmpty() ? QStringLiteral("—") : n.state));
    body += QStringLiteral("</div>");

    body += QStringLiteral("<form class='card' method='POST' action='/config/network'>");
    body += QStringLiteral("<h2 style='margin:0 0 8px;font-size:18px;'>Мрежова връзка</h2>");
    body += QStringLiteral("<label>Режим</label><select name='mode' id='mode'>"
                           "<option value='dhcp'%1>DHCP (автоматично)</option>"
                           "<option value='static'%2>Статичен IP</option></select>")
                .arg(isDhcp ? QStringLiteral(" selected") : QString(),
                     isDhcp ? QString() : QStringLiteral(" selected"));
    body += QStringLiteral("<label>IP адрес</label><input name='ip' value='%1'>")
                .arg(htmlEscape(n.ip));
    body += QStringLiteral("<label>Мрежова маска</label><input name='netmask' value='%1'>")
                .arg(htmlEscape(n.netmask.isEmpty() ? QStringLiteral("255.255.255.0") : n.netmask));
    body += QStringLiteral("<label>Шлюз</label><input name='gateway' value='%1'>")
                .arg(htmlEscape(n.gateway));
    body += QStringLiteral("<label>DNS (разделени с интервал)</label><input name='dns' value='%1'>")
                .arg(htmlEscape(n.dns));
    body += QStringLiteral("<div class='actions'><button type='submit'>Запази мрежата</button></div>");
    body += QStringLiteral("<p class='hint'>Статичните полета се ползват само при режим „Статичен IP“.</p>");
    body += QStringLiteral("</form>");

    body += QStringLiteral("<form class='card' method='POST' action='/config/portal'>");
    body += QStringLiteral("<h2 style='margin:0 0 8px;font-size:18px;'>Портал</h2>");
    body += QStringLiteral("<label>URL на портала</label><input name='portal' value='%1'>")
                .arg(htmlEscape(portal));
    body += QStringLiteral("<div class='actions'><button type='submit'>Запази портала</button></div>");
    body += QStringLiteral("</form>");

    body += QStringLiteral("<form class='card' method='POST' action='/config/reboot' "
                           "onsubmit=\"return confirm('Рестарт на CoreELEC?');\">");
    body += QStringLiteral("<h2 style='margin:0 0 8px;font-size:18px;'>Система</h2>");
    body += QStringLiteral("<p class='hint'>Рестартира целия бокс (CoreELEC).</p>");
    body += QStringLiteral("<div class='actions'><button type='submit'>Рестарт</button> "
                           "<a class='btn secondary' href='/'>Статус</a></div>");
    body += QStringLiteral("</form>");

    return pageShell(QStringLiteral("Yasem — настройки"), body);
}

bool ConfigServer::applyNetwork(const QHash<QString, QString> &form, QString *error) const
{
    const QString service = pickConnmanService();
    if (service.isEmpty()) {
        if (error)
            *error = QStringLiteral("Няма активна connman услуга (ethernet/wifi).");
        return false;
    }
    const QString mode = form.value(QStringLiteral("mode")).trimmed().toLower();
    QString out;
    if (mode == QLatin1String("dhcp") || mode == QLatin1String("auto")) {
        out = runCmd(QStringList() << "connmanctl" << "config" << service
                                   << "--ipv4" << "dhcp",
                     12000);
    } else {
        const QString ip = form.value(QStringLiteral("ip")).trimmed();
        const QString mask = form.value(QStringLiteral("netmask")).trimmed();
        const QString gw = form.value(QStringLiteral("gateway")).trimmed();
        if (ip.isEmpty() || mask.isEmpty() || gw.isEmpty()) {
            if (error)
                *error = QStringLiteral("Попълни IP, маска и шлюз за статичен режим.");
            return false;
        }
        out = runCmd(QStringList() << "connmanctl" << "config" << service
                                   << "--ipv4" << "manual" << ip << mask << gw,
                     12000);
        const QString dns = form.value(QStringLiteral("dns")).trimmed();
        if (!dns.isEmpty()) {
            QStringList dnsArgs = QStringList() << "connmanctl" << "config" << service << "--nameservers";
            for (const QString &d : dns.split(QRegExp("\\s+"), QString::SkipEmptyParts))
                dnsArgs << d;
            runCmd(dnsArgs, 8000);
        }
    }
    if (out.contains(QStringLiteral("Error"), Qt::CaseInsensitive)) {
        if (error)
            *error = out.simplified().left(200);
        return false;
    }
    return true;
}

bool ConfigServer::savePortal(const QString &portalIn, QString *error) const
{
    if (!api) {
        if (error)
            *error = QStringLiteral("API липсва");
        return false;
    }
    QString portal = portalIn.trimmed();
    if (portal.isEmpty()) {
        if (error)
            *error = QStringLiteral("Празен портал");
        return false;
    }
    if (!portal.contains(QStringLiteral("://")))
        portal.prepend(QStringLiteral("http://"));
    while (portal.endsWith('/'))
        portal.chop(1);

    const QString path = api->configDir() + QStringLiteral("/config.json");
    QJsonObject obj;
    QFile in(path);
    if (in.open(QIODevice::ReadOnly)) {
        obj = QJsonDocument::fromJson(in.readAll()).object();
        in.close();
    }
    obj.insert(QStringLiteral("portal"), portal);
    if (!obj.contains(QStringLiteral("model")))
        obj.insert(QStringLiteral("model"), QStringLiteral("MAG254"));
    if (!obj.contains(QStringLiteral("timezone")))
        obj.insert(QStringLiteral("timezone"), QStringLiteral("Europe/Sofia"));
    if (!obj.contains(QStringLiteral("language")))
        obj.insert(QStringLiteral("language"), QStringLiteral("bg"));

    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("Не мога да запиша config.json");
        return false;
    }
    out.write(QJsonDocument(obj).toJson(QJsonDocument::Indented));
    out.close();
    api->setPortalUrl(portal);
    return true;
}

QByteArray ConfigServer::httpResponse(int code, const QByteArray &contentType,
                                      const QByteArray &body,
                                      const QList<QPair<QByteArray, QByteArray>> &extraHeaders) const
{
    QByteArray status = "200 OK";
    if (code == 302)
        status = "302 Found";
    else if (code == 400)
        status = "400 Bad Request";
    else if (code == 404)
        status = "404 Not Found";
    else if (code == 500)
        status = "500 Internal Server Error";

    QByteArray r;
    r += "HTTP/1.0 " + status + "\r\n";
    r += "Content-Type: " + contentType + "\r\n";
    r += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    r += "Connection: close\r\n";
    r += "Cache-Control: no-store\r\n";
    for (const auto &h : extraHeaders)
        r += h.first + ": " + h.second + "\r\n";
    r += "\r\n";
    r += body;
    return r;
}

QByteArray ConfigServer::handleRequest(const QString &method, const QString &path,
                                       const QByteArray &body) const
{
    if (method == QLatin1String("GET") && (path == QLatin1String("/") || path == QLatin1String("/status"))) {
        const QByteArray html = statusPageHtml().toUtf8();
        return httpResponse(200, "text/html; charset=utf-8", html);
    }
    if (method == QLatin1String("GET") && path == QLatin1String("/config")) {
        const QByteArray html = configPageHtml().toUtf8();
        return httpResponse(200, "text/html; charset=utf-8", html);
    }

    if (method == QLatin1String("POST") && path == QLatin1String("/config/network")) {
        QString err;
        if (!applyNetwork(parseForm(body), &err)) {
            const QByteArray html = configPageHtml(QStringLiteral("ERR:") + err).toUtf8();
            return httpResponse(400, "text/html; charset=utf-8", html);
        }
        const QByteArray html = configPageHtml(QStringLiteral("Мрежата е записана. Изчакай няколко секунди и обнови.")).toUtf8();
        return httpResponse(200, "text/html; charset=utf-8", html);
    }

    if (method == QLatin1String("POST") && path == QLatin1String("/config/portal")) {
        const auto form = parseForm(body);
        QString err;
        const QString portal = form.value(QStringLiteral("portal"));
        if (!savePortal(portal, &err)) {
            const QByteArray html = configPageHtml(QStringLiteral("ERR:") + err).toUtf8();
            return httpResponse(400, "text/html; charset=utf-8", html);
        }
        emit const_cast<ConfigServer *>(this)->portalChanged(api->portalUrl());
        const QByteArray html = configPageHtml(QStringLiteral("Порталът е записан. Зареждането започва отново.")).toUtf8();
        return httpResponse(200, "text/html; charset=utf-8", html);
    }

    if (method == QLatin1String("POST") && path == QLatin1String("/config/reboot")) {
        const QByteArray html = configPageHtml(QStringLiteral("Рестарт…")).toUtf8();
        QByteArray resp = httpResponse(200, "text/html; charset=utf-8", html);
        // Delay reboot so response can flush
        QTimer::singleShot(800, qApp, []() {
            QProcess::startDetached(QStringLiteral("reboot"), QStringList());
        });
        emit const_cast<ConfigServer *>(this)->rebootRequested();
        return resp;
    }

    const QByteArray html = pageShell(QStringLiteral("404"),
        QStringLiteral("<h1>404</h1><p><a class='btn' href='/config'>Настройки</a></p>")).toUtf8();
    return httpResponse(404, "text/html; charset=utf-8", html);
}
