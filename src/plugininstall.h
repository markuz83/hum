#pragma once

#include <QString>

// `hum plugin install [--enable]`: puts the Omarchy bar plugin where the shell
// looks for plugins. Returns the exit code.
int installBarPlugin(const QStringList &arguments);
