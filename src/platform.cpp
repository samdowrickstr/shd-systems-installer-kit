#include "platform.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <objbase.h>
#else
#include <csignal>
#include <sys/types.h>
#endif

namespace platform {

// ---------------------------------------------------------------------------
// Names and locations
// ---------------------------------------------------------------------------

QString shortcutFileName(const QString &displayName)
{
#ifdef Q_OS_WIN
    return displayName + QStringLiteral(".lnk");
#else
    // A .desktop basename is an identifier, not a title. The title lives in the
    // Name= line inside the file, where it can carry spaces and capitals
    // without becoming a filename anybody has to type.
    QString id = displayName.toLower();
    id.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    while (id.endsWith('-')) id.chop(1);
    while (id.startsWith('-')) id.remove(0, 1);
    if (id.isEmpty()) id = QStringLiteral("application");
    return id + QStringLiteral(".desktop");
#endif
}

QString menuDir(const QString &vendorFolder)
{
#ifdef Q_OS_WIN
    const QString programs =
        QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    return vendorFolder.isEmpty() ? programs : QDir(programs).filePath(vendorFolder);
#else
    Q_UNUSED(vendorFolder);
    // XDG: ~/.local/share/applications. Per-user, no root, which is the same
    // promise the Windows side keeps by writing to HKCU and %LOCALAPPDATA%.
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(data).filePath(QStringLiteral("applications"));
#endif
}

QString desktopDir()
{
    // Legitimately empty on a headless or minimal session, and the caller
    // already handles "no desktop shortcut" because it is a checkbox.
    return QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
}

QString executableName(const QString &base)
{
#ifdef Q_OS_WIN
    return base.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive) ? base
                                                                     : base + QStringLiteral(".exe");
#else
    // Configs are written for Windows first and carry ".exe" in the app entry.
    // Stripping it here means one installer.json describes both platforms.
    return base.endsWith(QStringLiteral(".exe"), Qt::CaseInsensitive) ? base.chopped(4) : base;
#endif
}

QString installRecordPath(const QString &publisher, const QString &registryKey)
{
#ifdef Q_OS_WIN
    Q_UNUSED(publisher);
    return QStringLiteral(
               "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\")
           + registryKey;
#else
    const QString data = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(data).filePath(
        QStringLiteral("%1/%2/install.ini").arg(publisher.isEmpty() ? QStringLiteral("shd") : publisher,
                                                registryKey));
#endif
}

bool installRecordIsRegistry()
{
#ifdef Q_OS_WIN
    return true;
#else
    return false;
#endif
}

// ---------------------------------------------------------------------------
// Shortcuts
// ---------------------------------------------------------------------------

bool createShortcut(const QString &linkPath, const QString &target, const QString &args,
                    const QString &workingDir, const QString &description)
{
    QDir().mkpath(QFileInfo(linkPath).absolutePath());

#ifdef Q_OS_WIN
    bool ok = false;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW *psl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, reinterpret_cast<void **>(&psl)))) {
        psl->SetPath(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(target).utf16()));
        // Point the shortcut icon explicitly at the target exe's first icon.
        psl->SetIconLocation(
            reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(target).utf16()), 0);
        psl->SetWorkingDirectory(
            reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(workingDir).utf16()));
        psl->SetDescription(reinterpret_cast<const wchar_t *>(description.utf16()));
        if (!args.isEmpty()) {
            psl->SetArguments(reinterpret_cast<const wchar_t *>(args.utf16()));
        }
        IPersistFile *ppf = nullptr;
        if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, reinterpret_cast<void **>(&ppf)))) {
            ok = SUCCEEDED(ppf->Save(reinterpret_cast<const wchar_t *>(linkPath.utf16()), TRUE));
            ppf->Release();
        }
        psl->Release();
    }
    CoUninitialize();
    return ok;
#else
    QFile f(linkPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return false;

    // Exec= is a command line, and the spec reserves a handful of characters in
    // it. Quoting the program path covers the only one that occurs in practice
    // here — a space in the install directory — and %% escapes a literal
    // percent, which would otherwise be read as a field code.
    QString exec = QStringLiteral("\"%1\"").arg(QString(target).replace(QLatin1Char('%'),
                                                                        QStringLiteral("%%")));
    if (!args.isEmpty()) exec += QLatin1Char(' ') + args;

    QTextStream ts(&f);
    ts.setEncoding(QStringConverter::Utf8);
    ts << "[Desktop Entry]\n"
       << "Type=Application\n"
       << "Version=1.0\n"
       << "Name=" << QFileInfo(linkPath).completeBaseName() << "\n";
    if (!description.isEmpty()) ts << "Comment=" << description << "\n";
    ts << "Exec=" << exec << "\n"
       << "Path=" << workingDir << "\n"
       << "Terminal=false\n"
       << "Categories=Science;Engineering;\n";

    // An icon shipped beside the binary, referenced by absolute path. The
    // themed-name form needs the file installed into an icon theme directory,
    // which a per-user install into ~/.local cannot rely on.
    const QString png = QDir(workingDir).filePath(QStringLiteral("app.png"));
    if (QFile::exists(png)) ts << "Icon=" << png << "\n";

    ts.flush();
    f.close();

    // Without the executable bit GNOME and KDE show this as an untrusted file
    // with a warning rather than as an application, which a user reads as a
    // broken install rather than as a security feature.
    return f.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner
                            | QFileDevice::ExeOwner | QFileDevice::ReadGroup
                            | QFileDevice::ExeGroup | QFileDevice::ReadOther
                            | QFileDevice::ExeOther);
#endif
}

bool removeShortcut(const QString &linkPath)
{
    if (!QFile::exists(linkPath)) return true;
    return QFile::remove(linkPath);
}

// ---------------------------------------------------------------------------
// Deleting the directory we are running from
// ---------------------------------------------------------------------------

void scheduleSelfDelete(const QString &dir)
{
#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(dir);
    const QString ps1 =
        QDir::toNativeSeparators(QDir(QDir::tempPath()).filePath(QStringLiteral("appsetup_cleanup.ps1")));
    QFile sf(ps1);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Text)) return;
    QTextStream ts(&sf);
    ts << "Wait-Process -Id " << QCoreApplication::applicationPid()
       << " -Timeout 30 -ErrorAction SilentlyContinue\r\n"
       << "Start-Sleep -Milliseconds 400\r\n"
       << "Remove-Item -LiteralPath '" << native << "' -Recurse -Force -ErrorAction SilentlyContinue\r\n"
       << "Remove-Item -LiteralPath '" << ps1 << "' -Force -ErrorAction SilentlyContinue\r\n";
    sf.close();

    std::wstring cmdLine =
        QStringLiteral("powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%1\"")
            .arg(ps1)
            .toStdWString();
    std::wstring cwd = QDir::toNativeSeparators(QDir::tempPath()).toStdWString();
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       cwd.c_str(), &si, &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
#else
    const QString sh = QDir(QDir::tempPath()).filePath(QStringLiteral("appsetup_cleanup.sh"));
    QFile sf(sh);
    if (!sf.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return;

    // `kill -0` tests for the process's existence without signalling it, which
    // is the portable equivalent of Wait-Process here. Bounded at 30 seconds
    // for the same reason the Windows side is: if the installer has somehow not
    // exited by then, deleting the directory out from under it is worse than
    // leaving it.
    //
    // The script removes itself last. It cannot be removed by the thing that
    // launched it, because that has exited by the time this runs.
    QTextStream ts(&sf);
    ts.setEncoding(QStringConverter::Utf8);
    ts << "#!/bin/sh\n"
       << "pid=" << QCoreApplication::applicationPid() << "\n"
       << "i=0\n"
       << "while kill -0 \"$pid\" 2>/dev/null && [ $i -lt 300 ]; do\n"
       << "  sleep 0.1\n"
       << "  i=$((i+1))\n"
       << "done\n"
       << "sleep 0.4\n"
       << "rm -rf -- '" << QString(dir).replace(QLatin1Char('\''), QStringLiteral("'\\''")) << "'\n"
       << "rm -f -- \"$0\"\n";
    ts.flush();
    sf.close();
    sf.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    // Detached, and deliberately not through a shell string: startDetached with
    // an argument list means a directory containing a space or a quote cannot
    // change what runs.
    QProcess::startDetached(QStringLiteral("/bin/sh"), {sh});
#endif
}

// ---------------------------------------------------------------------------
// Processes running out of the install directory
// ---------------------------------------------------------------------------

namespace {

QString comparablePath(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}

#ifdef Q_OS_WIN
QString processImagePath(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return QString();
    wchar_t buffer[32768] = {};
    DWORD size = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    QString path;
    if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
        path = QString::fromWCharArray(buffer, static_cast<int>(size));
    }
    CloseHandle(process);
    return path;
}

BOOL CALLBACK postCloseToProcessWindow(HWND window, LPARAM param)
{
    DWORD windowPid = 0;
    GetWindowThreadProcessId(window, &windowPid);
    if (windowPid == static_cast<DWORD>(param)) {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}
#endif

}  // namespace

QList<RunningProcess> processesRunningFrom(const QString &dir, const QStringList &exeNames)
{
    QList<RunningProcess> found;
    if (dir.isEmpty() || exeNames.isEmpty()) return found;

    // Exact full paths, not a prefix test on the directory. A prefix would also
    // match a process running out of a SUBdirectory of the install — a bundled
    // solver, say — and killing the user's running solver because it happens to
    // live under the folder being updated is a different and much worse action
    // than closing the application they have open.
    QStringList wantedPaths;
    for (const QString &exe : exeNames) {
        wantedPaths << comparablePath(QDir(dir).filePath(exe));
    }

#ifdef Q_OS_WIN
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return found;

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const QString exeName = QString::fromWCharArray(entry.szExeFile);
            if (!exeNames.contains(exeName, Qt::CaseInsensitive)) continue;
            const QString path = processImagePath(entry.th32ProcessID);
            if (path.isEmpty()) continue;
            if (!wantedPaths.contains(comparablePath(path))) continue;
            found.append({static_cast<qint64>(entry.th32ProcessID), exeName,
                          QDir::fromNativeSeparators(path)});
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
#else
    // /proc/<pid>/exe is a symlink to the running image, and reading it needs no
    // privilege for our own processes — which is all we can act on anyway.
    //
    // It resolves through deletions: a binary replaced under a running process
    // reads as "/path/to/app (deleted)". That suffix is stripped rather than
    // rejected, because a process running a deleted copy of our binary is
    // precisely the state this check exists to notice.
    const QDir proc(QStringLiteral("/proc"));
    const QStringList entries =
        proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::NoSort);
    for (const QString &name : entries) {
        bool numeric = false;
        const qint64 pid = name.toLongLong(&numeric);
        if (!numeric || pid <= 0) continue;

        QString path = QFile::symLinkTarget(QStringLiteral("/proc/%1/exe").arg(pid));
        if (path.isEmpty()) continue;
        if (path.endsWith(QStringLiteral(" (deleted)"))) path.chop(10);

        const QString exeName = QFileInfo(path).fileName();
        if (!exeNames.contains(exeName, Qt::CaseSensitive)) continue;
        if (!wantedPaths.contains(comparablePath(path))) continue;
        found.append({pid, exeName, path});
    }
#endif
    return found;
}

void requestProcessesClose(const QList<RunningProcess> &processes)
{
    for (const RunningProcess &p : processes) {
#ifdef Q_OS_WIN
        EnumWindows(postCloseToProcessWindow, static_cast<LPARAM>(p.pid));
#else
        // SIGTERM is the honest counterpart of a posted WM_CLOSE: a request the
        // application can catch, act on and decline. Qt turns it into no such
        // thing by default — the default disposition terminates — but an app
        // that installs a handler gets the same chance to save that its Windows
        // build gets, and one that does not was going to be killed anyway.
        ::kill(static_cast<pid_t>(p.pid), SIGTERM);
#endif
    }
}

void terminateProcesses(const QList<RunningProcess> &processes)
{
    for (const RunningProcess &p : processes) {
#ifdef Q_OS_WIN
        HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(p.pid));
        if (h) {
            TerminateProcess(h, 0);
            CloseHandle(h);
        }
#else
        ::kill(static_cast<pid_t>(p.pid), SIGKILL);
#endif
    }
}

void notifyApplicationsChanged(const QString &menuDirectory)
{
#ifdef Q_OS_WIN
    Q_UNUSED(menuDirectory);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
#else
    // Absent on a minimal system and not worth reporting: without it the menu
    // catches up at the next login rather than immediately. Detached so a
    // desktop database that takes its time cannot hold up the wizard.
    const QString tool = QStandardPaths::findExecutable(QStringLiteral("update-desktop-database"));
    if (!tool.isEmpty()) QProcess::startDetached(tool, {menuDirectory});
#endif
}

}  // namespace platform
