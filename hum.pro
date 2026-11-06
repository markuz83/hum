QT += core gui qml quick quickcontrols2 dbus multimedia concurrent

CONFIG += c++17 release
TARGET = hum
TEMPLATE = app

HEADERS += \
    src/analyzer.h \
    src/backend.h \
    src/theme.h \
    src/translator.h

SOURCES += \
    src/main.cpp \
    src/analyzer.cpp \
    src/backend.cpp \
    src/theme.cpp \
    src/translator.cpp

RESOURCES += src/resources.qrc
