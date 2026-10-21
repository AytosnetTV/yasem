QT += core gui widgets network webkit webkitwidgets
CONFIG += c++11
TARGET = yasem
TEMPLATE = app
SOURCES += src/main.cpp src/player.cpp src/magapi.cpp src/browser.cpp src/remote_keymap.cpp src/configserver.cpp
HEADERS += src/player.h src/magapi.h src/browser.h src/remote_keymap.h src/configserver.h

AMCODEC = $$(AMCODEC)
FFMPEG = $$(FFMPEG)
ALSA = $$(ALSA)
OPENSSL = $$(OPENSSL)
CELIBS = $$(CELIBS)

INCLUDEPATH += $$AMCODEC $$FFMPEG $$ALSA/include $$OPENSSL/include
LIBS += -L$$CELIBS -lamcodec -lavformat -lavcodec -lswresample -lswscale -lavutil -lasound -lcrypto -ldl -lpthread -lm -lrt
QMAKE_LFLAGS += -Wl,--allow-shlib-undefined
QMAKE_CXXFLAGS += -march=armv7-a -mfpu=neon -mfloat-abi=hard -std=gnu++14
QMAKE_CFLAGS += -march=armv7-a -mfpu=neon -mfloat-abi=hard
