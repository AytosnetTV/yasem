#ifndef YASEM_CONFIGSERVER_H
#define YASEM_CONFIGSERVER_H

#include <QObject>
#include <QByteArray>
#include <QHash>
#include <QString>

class MagApi;
class QTcpServer;
class QTcpSocket;

// Local HTTP server on :80 — MAG-style status + /config for LAN setup.
class ConfigServer : public QObject {
    Q_OBJECT
public:
    explicit ConfigServer(MagApi *api, QObject *parent = nullptr);
    bool listen(quint16 port = 80);
    QString statusPageHtml(const QString &reason = QString()) const;
    QString configPageHtml(const QString &flash = QString()) const;

signals:
    void portalChanged(const QString &portal);
    void rebootRequested();

private slots:
    void onNewConnection();
    void onReadyRead();

private:
    struct NetInfo {
        QString service;
        QString iface;
        QString method; // dhcp|manual|unknown
        QString ip;
        QString netmask;
        QString gateway;
        QString dns;
        QString mac;
        QString state;
        bool link = false;
    };

    NetInfo readNet() const;
    QByteArray handleRequest(const QString &method, const QString &path,
                             const QByteArray &body) const;
    QByteArray httpResponse(int code, const QByteArray &contentType,
                            const QByteArray &body,
                            const QList<QPair<QByteArray, QByteArray>> &extraHeaders = {}) const;
    static QHash<QString, QString> parseForm(const QByteArray &body);
    static QString htmlEscape(const QString &s);
    bool applyNetwork(const QHash<QString, QString> &form, QString *error) const;
    bool savePortal(const QString &portal, QString *error) const;
    QString pageShell(const QString &title, const QString &body) const;

    MagApi *api = nullptr;
    QTcpServer *server = nullptr;
    QHash<QTcpSocket *, QByteArray> buffers;
};

#endif
