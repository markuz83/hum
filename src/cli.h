#pragma once

#include <QJsonObject>
#include <QStringList>
#include <QTextStream>

// Commands that control a running Hum from a terminal or a script, through
// the same MPRIS interface media keys use. No window is created.
namespace Cli {

// True when `word` names a command, so `hum status` isn't mistaken for a folder.
bool isCommand(const QString &word);

// Text for a terminal, with control characters (and so escape sequences) removed.
QString printable(const QString &text);

// The player's state, or an object saying it isn't running. `service` is the
// D-Bus name to ask, org.mpris.MediaPlayer2.hum unless a test passes its own.
QJsonObject status(const QString &service, bool *running = nullptr);

// Runs one command (the arguments after `hum`). Returns the exit status: 0
// on success, 1 when Hum isn't running or the command was wrong, with the
// reason written to `err`.
int run(const QStringList &args, QTextStream &out, QTextStream &err, const QString &service = {});

QString usage();

}  // namespace Cli
