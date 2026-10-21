#pragma once
#include <QString>

namespace PrivateStorage {
QString dataDirectory();
QString stateDirectory();
// Refuse symlink targets and directories owned by another user. Existing application
// directories are tightened, never widened for group/other users.
// Per-run bearer token written by bin/models-start. Empty when absent or not a private regular file.
QString apiToken();
bool ensureDirectory(const QString &path, QString &error, bool tightenExisting = true);
}
