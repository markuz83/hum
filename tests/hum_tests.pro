QT += core gui quick quickcontrols2 testlib dbus multimedia concurrent
CONFIG += c++17 testcase
TARGET = hum_tests
TEMPLATE = app

INCLUDEPATH += ../src

HEADERS += ../src/analyzer.h ../src/backend.h ../src/coverprovider.h ../src/mpris.h ../src/barlink.h ../src/cli.h ../src/filepicker.h ../src/portalfilepicker.h ../src/theme.h ../src/translator.h
SOURCES += hum_tests.cpp ../src/analyzer.cpp ../src/backend.cpp ../src/mpris.cpp ../src/barlink.cpp ../src/cli.cpp ../src/portalfilepicker.cpp ../src/theme.cpp ../src/translator.cpp
