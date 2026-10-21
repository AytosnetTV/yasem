#ifndef YASEM_MAGAPI_H
#define YASEM_MAGAPI_H

#include <QObject>
#include <QRect>
#include <QVariantMap>

class HwPlayer;

class MagApi : public QObject {
    Q_OBJECT
public:
    explicit MagApi(HwPlayer *player, QObject *parent = nullptr);

    void loadIdentity();
    bool previewHole(QRect *rect) const;
    QString portalUrl() const;
    void setPortalUrl(const QString &url);
    QString userAgent() const;
    QString configDir() const { return configDirectory; }

public slots:
    bool Play(const QString &playStr, const QString &proxyParams = QString());
    bool PlaySolution(const QString &solution, const QString &url);
    bool Stop();
    bool Pause();
    bool Continue();
    bool IsPlaying();
    int GetPosTime();
    int GetPosTimeEx();
    bool SetPosTime(int seconds);
    bool SetPosTimeEx(int ms);
    int GetPosPercent();
    bool SetPosPercent(int percent);
    int GetMediaLen();
    int GetMediaLenEx();
    int GetVolume();
    bool SetVolume(int volume);
    bool GetMute();
    bool SetMute(int mute);
    bool SetViewport(int width, int height, int x, int y);
    bool SetViewportEx(int, int, int, int);
    bool SetTopWin(int win);
    int GetTopWin();
    QString GetDeviceMacAddress();
    QString GetDeviceSerialNumber();
    QString GetDeviceModel();
    QString GetDeviceModelExt();
    QString GetDeviceVendor();
    QString GetDeviceVersionHardware();
    QString GetDeviceImageVersion();
    QString GetDeviceImageDesc();
    QString GetDeviceImageVersionCurrent();
    QString GetDeviceActiveBank();
    bool GetLanLinkStatus();
    bool GetWifiLinkStatus();
    int GetHDMIConnectionState();
    bool GetStandByStatus();
    bool isStandBy() const { return standByActive; }
    QString RDir(const QString &name);
    QString GetEnv(const QString &data);
    bool SetEnv(const QString &data);
    bool SetEnv(const QString &data, const QString &value);
    QString GetUID();
    QString GetUID(const QString &arg1);
    QString GetUID(const QString &arg1, const QString &arg2);
    QString GetHashVersion1(const QString &secret, const QString &key);
    QString Version();
    QString GetVideoInfo();
    QString GetAudioPIDs();
    QString GetAudioPIDsEx();
    QString GetSubtitlePIDs();
    QString GetMetadataInfo();
    int GetBufferLoad();
    int GetAspect();
    int GetSpeed();
    int GetAlphaLevel();
    bool Debug(const QString &msg);
    bool InitPlayer();
    bool DeinitPlayer();
    bool LoadURL(const QString &url);
    bool SetWebReturnUrl(const QString &url);
    QString GetWebReturnUrl();
    bool ClearWebReturnUrl();
    bool ShowVideoImmediately();
    QString GetHLSInfo();
    QString GetStatistics();
    bool ClearStatistics();
    bool EnableSpatialNavigation();
    bool IsVirtualKeyboardActive();
    QString LoadUserData();
    QString LoadUserData(const QString &name);
    bool SaveUserData(const QString &data);
    bool SaveUserData(const QString &name, const QString &data);
    bool IsFileExist(const QString &);
    QString ListDir(const QString &);
    QString GetStorageInfo();
    QString GetExtProtocolList();
    bool SetLoop(int);
    bool SetSpeed(int);
    bool SetAspect(int);
    bool SetAudioPID(int);
    int GetAudioPID();
    bool SetSubtitlePID(int);
    int GetSubtitlePID();
    bool SetSubtitles(int);
    bool SetMode(int);
    bool SetPIG(int, int, int, int);
    bool GetPIG();
    bool SetWinAlphaLevel(int);
    bool SetWinMode(int);
    bool SetAlphaLevel(int);
    bool SetTransparentColor(int);
    bool SetChromaKey(int);
    bool SetVideoControl(int);
    bool SetVideoState(int);
    bool StandBy(int);
    bool SetScreenSaverTime(int);
    bool ShowVirtualKeyboard();
    bool HideVirtualKeyboard();
    bool EnableVKButton(int enable = 1);
    bool SetAutoFrameRate(int mode = 0);
    bool SendEventToPortal(const QString &json);
    bool SetWebProxy(const QString &, int);
    bool ResetWebProxy();
    bool ExecAction(const QString &action);

signals:
    void loadRequested(const QString &url);
    void layoutChanged();

private:
    QString envValue(const QString &key) const;
    void rememberEnv(const QString &key, const QString &value);
    void saveEnvFile() const;

    HwPlayer *player;
    QString configDirectory;
    QString model;
    QString mac;
    QString serial;
    QString deviceId;
    QString deviceId2;
    QString signature;
    QString portal;
    QString timezone;
    QString language;
    QString imageVersion;
    QString imageDescription;
    QString imageDate;
    QString hardwareVersion;
    QString ipAddress;
    QVariantMap env;
    QString webReturnUrl;
    int topWin;
    bool standByActive = false;
};

#endif
