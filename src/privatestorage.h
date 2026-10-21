#pragma once
#include <QString>

namespace PrivateStorage {
QString dataDirectory();
QString stateDirectory();
// Refuse symlink targets and directories owned by another user. Existing application
// directories are tightened, never widened for group/other users.
bool ensureDirectory(const QString &path, QString &error, bool tightenExisting = true);
}
