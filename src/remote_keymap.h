#ifndef YASEM_REMOTE_KEYMAP_H
#define YASEM_REMOTE_KEYMAP_H

#include <QKeyEvent>
#include <QString>

// Loads CoreELEC/Kodi Amlogic remote.conf (IR → linux keycode + comment)
// and translates those linux keycodes to MAG/Qt keys for the portal.
class RemoteKeymap {
public:
    static RemoteKeymap &instance();

    void load();
    bool loaded() const { return mapCount > 0; }
    int mappingCount() const { return mapCount; }
    QString sourcePath() const { return path; }

    QKeyEvent translate(const QKeyEvent *event) const;
    bool isPower(int nativeScanCode) const;
    bool isPower(const QKeyEvent *event) const;

private:
    RemoteKeymap() = default;
    void addDefaultFallbacks();
    void parseFile(const QString &filePath);
    void addMapping(int linuxCode, const QString &comment);

    struct Entry {
        int qtKey = 0;
        int mods = 0;
        bool power = false;
        bool used = false;
    };

    Entry table[512];
    int mapCount = 0;
    QString path;
};

#endif
