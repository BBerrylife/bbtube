#include "FileLogger.hpp"
#include "src/db/DbHelper.hpp"

#include <QFile>
#include <QDir>
#include <QDateTime>
#include <QTextStream>
#include <QMutex>
#include <QtGlobal>

// BB10's shared/documents is a fixed, non-sandboxed absolute path (unlike
// QDir::homePath(), which resolves to this app's private data/ folder) --
// every app with the access_shared permission sees the same folder, and
// it's the location users/File Manager/USB mass storage expect to find
// exported files in, so the log ends up somewhere the person can actually
// get to without root/sideloaded tools.
static const char *const SHARED_LOG_DIR = "/accounts/1000/shared/documents/bbtube";
static const char *const LOG_FILE_NAME = "bbtube_debug.log";

static bool s_enabled = false;
// Guards s_enabled and the log file write itself -- qDebug()/qWarning()
// calls can come from any thread (network replies, timers, etc. all
// still funnel through the Qt message handler on whichever thread
// triggered them), so both the flag check and the file I/O need to be
// serialized to avoid interleaved/corrupted lines or a torn read of
// s_enabled.
static QMutex s_mutex;
// The previous handler (if any) is preserved and always called first, so
// installing FileLogger never changes existing console/IDE log
// behavior -- it only adds the optional file copy on top.
static QtMsgHandler s_previousHandler = 0;

static QString typePrefix(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:
        return QString();
    case QtWarningMsg:
        return QString("[warning] ");
    case QtCriticalMsg:
        return QString("[critical] ");
    case QtFatalMsg:
        return QString("[fatal] ");
    default:
        return QString();
    }
}

static void fileLoggerMessageHandler(QtMsgType type, const char *msg)
{
    // Always forward to whatever handler was installed before us first
    // (or Qt's built-in default console handler if there was none) --
    // this preserves normal qDebug()/qWarning() console/IDE behavior
    // exactly as if FileLogger had never been installed. In particular,
    // QtFatalMsg's default handler calls abort() after printing, so this
    // must happen before we touch any Qt containers/file I/O below in
    // case the process is about to go down.
    if (s_previousHandler) {
        s_previousHandler(type, msg);
    } else {
        fprintf(stderr, "%s\n", msg);
    }

    QMutexLocker locker(&s_mutex);
    if (!s_enabled) {
        return;
    }

    QDir dir(SHARED_LOG_DIR);
    if (!dir.exists()) {
        if (!dir.mkpath(SHARED_LOG_DIR)) {
            // Can't create the target folder (permission not granted,
            // shared storage unavailable, etc.) -- disable ourselves
            // rather than retrying this on every single log line.
            s_enabled = false;
            return;
        }
    }

    QFile file(dir.filePath(LOG_FILE_NAME));
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        s_enabled = false;
        return;
    }

    QTextStream out(&file);
    out << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss.zzz") << " "
        << typePrefix(type) << msg << "\n";
}

void FileLogger::install()
{
    QMutexLocker locker(&s_mutex);
    if (s_previousHandler) {
        return; // already installed
    }
    s_enabled = DbHelper::isDebugLogToFile();
    s_previousHandler = qInstallMsgHandler(fileLoggerMessageHandler);
}

void FileLogger::setEnabled(bool enabled)
{
    QMutexLocker locker(&s_mutex);
    s_enabled = enabled;
}

bool FileLogger::isEnabled()
{
    QMutexLocker locker(&s_mutex);
    return s_enabled;
}

QString FileLogger::logFilePath()
{
    return QDir(SHARED_LOG_DIR).filePath(LOG_FILE_NAME);
}
