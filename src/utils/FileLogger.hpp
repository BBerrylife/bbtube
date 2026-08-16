#ifndef FILELOGGER_HPP_
#define FILELOGGER_HPP_

// qDebug()/qWarning() output on BB10 goes to a console log buffer (visible
// in Momentics, or via slog2info on-device) that only keeps the last N
// lines. Long-running operations that log a lot -- most notably the
// StreamingRemuxSession fragment-discovery debug output, which can print
// many hundreds of lines for a long video -- push the actual start (and
// sometimes the final pass/fail outcome) out of that buffer before anyone
// gets a chance to read it.
//
// FileLogger installs a Qt message handler (qInstallMsgHandler, the
// Qt4.8 API) that behaves exactly like the default one -- it still prints
// to the console/IDE exactly as before -- but additionally appends every
// message to a plain-text file when the "debugLogToFile" setting is
// enabled, so a full session's log can be recovered afterwards even if
// the console buffer truncated it.
//
// The log file is written under the device's shared documents folder
// (/accounts/1000/shared/documents/bbtube/bbtube_debug.log) rather than
// the app's private data folder so it's easy to find and pull off the
// device (File Manager, USB mass storage, Sync) without needing a
// sideloaded file browser with app-data access. This requires the
// "access_shared" permission (see bar-descriptor.xml).
//
// Call FileLogger::install() once, as early as possible in
// ApplicationUI's constructor -- right after DbHelper::runMigrations()
// so the Settings table (and therefore the "debugLogToFile" flag) is
// guaranteed to exist -- and before anything else has a chance to log.
// Whether logging-to-file is currently active can be toggled at runtime
// via setEnabled() (wired up from SettingsSheet), without needing to
// reinstall the handler.
//
// C++03/GNU++98 only (matches bbtube's QNX/gcc 4.6.3 toolchain).
class FileLogger
{
public:
    // Installs the message handler. Reads the current "debugLogToFile"
    // setting from DbHelper to decide whether to start out active.
    static void install();

    // Turns file logging on/off for the remainder of the process, without
    // touching the persisted DbHelper setting -- callers that also want
    // the change to persist across restarts should call
    // DbHelper::setDebugLogToFile() themselves (SettingsSheet does this).
    static void setEnabled(bool enabled);
    static bool isEnabled();

    // Absolute path to the log file, for display in Settings (e.g. "log
    // saved to <path>") -- valid whether or not logging is currently
    // enabled.
    static QString logFilePath();

private:
    FileLogger(); // static-only utility class, not constructible
};

#endif /* FILELOGGER_HPP_ */
