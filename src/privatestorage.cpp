#include "privatestorage.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

QString PrivateStorage::dataDirectory() {
    return qEnvironmentVariable("LIVE_SUB_DATA_DIR",
        QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/live-sub");
}

QString PrivateStorage::stateDirectory() {
    return qEnvironmentVariable("LIVE_SUB_STATE_DIR",
        qEnvironmentVariable("XDG_STATE_HOME", QDir::homePath() + "/.local/state") + "/live-sub");
}

QString PrivateStorage::apiToken() {
    const auto path = qEnvironmentVariable("LIVE_SUB_API_TOKEN_FILE", stateDirectory() + "/api-token");
    const int descriptor = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) return {};
    struct stat info {};
    QByteArray bytes;
    if (::fstat(descriptor, &info) == 0 && S_ISREG(info.st_mode) && info.st_uid == ::geteuid()
        && (info.st_mode & 0077) == 0 && info.st_size > 0 && info.st_size < 1024) {
        bytes.resize(static_cast<int>(info.st_size));
        if (::read(descriptor, bytes.data(), static_cast<size_t>(bytes.size())) != bytes.size()) bytes.clear();
    }
    ::close(descriptor);
    return QString::fromLatin1(bytes).trimmed();
}

bool PrivateStorage::ensureDirectory(const QString &path, QString &error, bool tightenExisting) {
    error.clear();
    if (path.isEmpty() || !QDir::isAbsolutePath(path)) {
        error = "A private absolute directory is required: " + path;
        return false;
    }
    const bool existed = QFileInfo::exists(path);
    if (!QDir().mkpath(path)) {
        error = "Cannot create directory: " + path;
        return false;
    }
    // fchmod operates on the checked directory itself, not a subsequently swapped link.
    const int descriptor = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (descriptor < 0) {
        error = path + ": " + QString::fromLocal8Bit(std::strerror(errno));
        return false;
    }
    struct stat info {};
    const bool owned = ::fstat(descriptor, &info) == 0 && info.st_uid == ::geteuid();
    // An override may point at Documents or even /tmp. Never chmod an unrelated
    // existing directory: require the caller to select a private destination.
    const bool mayTighten = !existed || tightenExisting || (info.st_mode & 0077) == 0;
    const bool secured = owned && mayTighten && ::fchmod(descriptor, S_IRWXU) == 0;
    ::close(descriptor);
    if (!secured) error = "Cannot make directory private for this user: " + path;
    return secured;
}
