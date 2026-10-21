#ifndef YASEM_PLAYER_H
#define YASEM_PLAYER_H

#include <QObject>
#include <QString>
#include <QThread>

class HwPlayer : public QObject {
    Q_OBJECT
public:
    explicit HwPlayer(QObject *parent = nullptr);
    ~HwPlayer() override;

    bool commandPlay(const QString &playStr, const QString &proxyParams = QString());
    void commandStop();
    void commandPause();
    void commandContinue();
    bool isPlaying() const;
    int positionSeconds() const;
    int positionMs() const;
    int durationSeconds() const;
    int durationMs() const;
    int positionPercent() const;
    void seekSeconds(int seconds);
    void setVolume(int volume);
    int volume() const;
    void setMuted(bool muted);
    bool muted() const;
    void setViewport(int width, int height, int x, int y);
    void viewport(int *width, int *height, int *x, int *y) const;

public slots:
    void relay(int code);

signals:
    void playbackEvent(int code);

private:
    class Engine;
    Engine *engine;
};

#endif
