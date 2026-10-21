#ifndef YASEM_BROWSER_H
#define YASEM_BROWSER_H

#include <QPaintEvent>
#include <QWebPage>
#include <QWebView>

class MagApi;

class MagPage : public QWebPage {
    Q_OBJECT
public:
    explicit MagPage(const QString &userAgent, QObject *parent = nullptr);

protected:
    QString userAgentForUrl(const QUrl &url) const override;
    bool acceptNavigationRequest(QWebFrame *frame, const QNetworkRequest &request, NavigationType type) override;

private:
    QString agent;
};

class MagView : public QWebView {
    Q_OBJECT
public:
    explicit MagView(MagApi *api, QWidget *parent = nullptr);

protected:
    void paintEvent(QPaintEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void keyReleaseEvent(QKeyEvent *event) override;

private slots:
    void attachObjects();
    void onLoadFinished(bool ok);

private:
    void injectClockPersist();
    void injectSettingsFullscreen();
    MagApi *api;
};

#endif
