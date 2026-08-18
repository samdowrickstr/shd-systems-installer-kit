#pragma once

// The three things an installer has to do that have no portable Qt answer.
//
// Everything else in this kit — the wizard, the manifest, the fetcher, the
// Ed25519 verification — is plain Qt and compiles anywhere. These three are
// where "install an application" stops meaning the same thing on two operating
// systems, and each is a genuinely different mechanism rather than a different
// spelling of one:
//
//   a shortcut         a .lnk written through IShellLink and a COM persist
//                      interface, or a .desktop file written with QTextStream.
//
//   the install record the HKCU Uninstall key that Add/Remove Programs reads,
//                      or a file, because Linux has no such registry and the
//                      thing that makes an app appear in a menu is the .desktop
//                      entry that is already being written for the shortcut.
//
//   self-deletion      a running executable cannot delete the directory it is
//                      running from on either platform, so both spawn a helper
//                      that waits for this process to exit. The helper is
//                      PowerShell or /bin/sh, and the waiting is Wait-Process
//                      or a kill(2) poll.
//
// ── Why a namespace of free functions and not a class ──────────────────────
// There is no state. A class would exist only to be constructed somewhere and
// passed around, and the call sites in setupwindow.cpp read better as
// `platform::createShortcut(...)` than through a member that has to be reached.
//
// ── The rule these functions follow ────────────────────────────────────────
// A function whose concept does not exist on a platform returns an empty
// string or false rather than asserting. `desktopDir()` on a headless Linux
// box is legitimately absent, and the caller already has to handle "the user
// declined a desktop shortcut" — one more empty path is the same branch.

#include <QList>
#include <QString>
#include <QStringList>

namespace platform {

// One process running out of the directory we are about to write to.
//
// `pid` is qint64 rather than the platform's own type: DWORD on Windows and
// pid_t on Linux are both integers and neither header belongs in a file every
// translation unit includes.
struct RunningProcess {
    qint64 pid = 0;
    QString exeName;
    QString path;
};

// Which of `exeNames` are running out of `dir` right now.
//
// This is the check that stops an install writing over a binary the user has
// open, which on Windows fails outright and on Linux succeeds and leaves them
// running a deleted inode — the second is worse, because nothing reports it
// until the application does something inexplicable an hour later.
QList<RunningProcess> processesRunningFrom(const QString &dir, const QStringList &exeNames);

// Ask each process to close, the way a user closing the window would.
//
// WM_CLOSE posted to the process's windows, or SIGTERM. Both are a REQUEST: the
// application gets to run its shutdown, prompt about unsaved work, and refuse.
// That is the intent, and it is why this is separate from the forced kill below.
void requestProcessesClose(const QList<RunningProcess> &processes);

// Stop them without asking. Only after requestProcessesClose() and a wait.
void terminateProcesses(const QList<RunningProcess> &processes);

// Tell the desktop that installed applications have changed.
//
// SHChangeNotify so Explorer picks up a new icon, or update-desktop-database so
// the freedesktop menu picks up a new .desktop entry. Best-effort on both:
// nothing is broken if it does not happen, the menu just lags until the next
// login.
void notifyApplicationsChanged(const QString &menuDirectory);

// The on-disk name a launcher takes: "SHD Sim.lnk" or "shd-sim.desktop".
//
// Lowercased and hyphenated on Linux, because a .desktop file name is an
// identifier that ends up in menus, MIME associations and the session's
// bookkeeping — "SHD Sim.desktop" is legal and works, and every convention on
// the platform says not to.
QString shortcutFileName(const QString &displayName);

// Where application menu entries live.
//
// `vendorFolder` is the Start Menu subfolder on Windows and is IGNORED on
// Linux: the freedesktop menu is flat and grouped by the Categories= line
// inside each entry, so creating a directory per vendor would produce a folder
// nothing reads. Kept in the signature so the call site does not have to know
// that.
QString menuDir(const QString &vendorFolder);

// Where a desktop shortcut goes, or empty when the platform or session has no
// desktop directory.
QString desktopDir();

// Write a launcher. `args` may be empty.
//
// On Linux the file is made executable: a .desktop that is not marked as such
// is shown by GNOME and KDE as an untrusted file with a warning rather than as
// an application, which reads to the user as a broken install.
bool createShortcut(const QString &linkPath, const QString &target, const QString &args,
                    const QString &workingDir, const QString &description);

// Remove a launcher created by createShortcut. Returns true if it is gone
// afterwards, including when it was never there.
bool removeShortcut(const QString &linkPath);

// Arrange for `dir` to be deleted once this process has exited.
//
// Best-effort by nature and reports nothing: it runs after the only code that
// could show an error message has gone. A failure leaves a directory in the
// user's temp, which the OS clears eventually.
void scheduleSelfDelete(const QString &dir);

// The file name of an executable: "shd-simcfd-app.exe" or "shd-simcfd-app".
QString executableName(const QString &base);

// Absolute path of the install record, and whether it is a registry path.
//
// Windows: the HKCU Uninstall key, read by Add/Remove Programs.
// Linux:   an INI file under the user's data directory. Nothing else reads it;
//          it exists so an upgrade or a repair can find the previous install,
//          which on Windows is what the registry key is actually used for here.
QString installRecordPath(const QString &publisher, const QString &registryKey);

// True when installRecordPath() returns a registry path, so the caller knows
// which QSettings::Format to open it with.
bool installRecordIsRegistry();

}  // namespace platform
