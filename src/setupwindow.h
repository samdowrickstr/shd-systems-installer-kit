// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: (c) 2026 SHD Systems Ltd

#pragma once

#include <QDialog>
#include <QPoint>
#include <QString>
#include <QVector>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QVBoxLayout;
QT_END_NAMESPACE

// One installable application in the bundle. A bundle may contain any number of
// these; each gets a checkbox on the install page and its own shortcuts.
struct AppEntry {
    QString exe;          // file name of the executable, e.g. "myapp.exe"
    QString name;         // display name, e.g. "My App"
    QString description;  // short one-liner shown next to the checkbox
    bool defaultOn = true;
    QCheckBox *check = nullptr; // filled in when the install UI is built
};

// All per-project data, loaded from the embedded :/setup/config.json. This is
// what makes the installer reusable: nothing about the product is hard-coded.
struct InstallerConfig {
    QString appName;      // canonical product name (install folder, reg entry)
    QString displayName;  // title shown in the header (may omit brand if in logo)
    QString subtitle;     // small caps line under the title, e.g. "SETUP"
    QString publisher;
    QString version;
    QString registryKey;  // leaf name under ...\Uninstall\<registryKey>
    QString accentColor;  // hex, e.g. "#1CA3C2"
    bool desktopShortcut = true;
    bool startMenuShortcut = true;
    QVector<AppEntry> apps;

    // Full registry path for the Add/Remove Programs entry.
    QString uninstallRegPath() const;
    // Names of every app executable (used to detect running instances).
    QStringList appExeNames() const;
};

enum class SetupAction { Auto, Install, Update, Repair, Uninstall };

enum SetupExitCode {
    SetupExitSuccess = 0,
    SetupExitFailed = 1,
    SetupExitCancelled = 2,
    SetupExitAlreadyCurrent = 3,
    SetupExitNotInstalled = 4,
    SetupExitInvalidArguments = 5
};

// Bespoke, config-driven installer. Runs in three modes: install (default),
// maintenance (repair/update when already installed) and uninstall
// (--uninstall, used by the copy of this exe placed in the install folder).
class SetupWindow : public QDialog {
    Q_OBJECT
public:
    explicit SetupWindow(const InstallerConfig &config, SetupAction action = SetupAction::Auto,
                         bool silent = false,
                         QWidget *parent = nullptr);

protected:
    // Frameless windows have no title bar, so allow dragging from anywhere.
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;

private:
    void centreOnPrimary();
    // Builds the translucent rounded frame + header (logo, title, close button)
    // and returns the content layout to populate.
    QVBoxLayout *makeFrame(bool showSubtitle);

    // --- shared ---
    QPixmap brandLogo(int height) const;
    void applyStyle();
    QString defaultInstallDir() const;

    // --- install flow ---
    void buildInstallUi();
    void runRequestedAction();
    void finishSilent(int code);
    void failSilent(int code, const QString &message);
    void refreshFooter();
    void browseForFolder();
    void startInstall();
    bool copyPayload(const QString &targetDir, int &doneOut, int total);
    bool copyPayloadFile(const QString &sourcePath, const QString &destPath, QString *errorMessage) const;
    int countPayloadFiles() const;
    void finishInstall(const QString &targetDir);

    // --- maintenance flow (repair / update / uninstall) ---
    bool readInstalledInfo();   // populate m_installedDir/Version from registry
    void buildMaintenanceUi();
    void doRepairOrUpdate();
    bool closeRunningInstalledApps(const QString &targetDir);

    // --- uninstall flow ---
    void buildUninstallUi();
    void startUninstall();

    // --- helpers (Win32) ---
    bool createShortcut(const QString &linkPath, const QString &target, const QString &args,
                        const QString &workingDir, const QString &description) const;
    void writeUninstallInfo(const QString &targetDir, int sizeKb) const;
    void removeUninstallInfo() const;
    QStringList shortcutPaths() const;     // candidate .lnk paths we may have made
    QString startMenuDir() const;

    // True if the given file should be installed. App executables are gated by
    // their checkboxes; every other file (the shared Qt runtime) is always
    // installed. Checkboxes are null in maintenance mode -> treated as selected.
    bool wantExe(const QString &exeName) const;
    // The AppEntry whose exe matches name, or nullptr if it is not an app exe.
    const AppEntry *appForExe(const QString &exeName) const;

    enum class Mode { Install, Maintenance, Uninstall };

    InstallerConfig m_config;
    SetupAction m_requestedAction = SetupAction::Auto;
    bool m_silent = false;
    QString m_sourceDir;   // folder this setup exe runs from (contains payload)
    Mode m_mode = Mode::Install;
    QString m_installedDir;
    QString m_installedVersion;
    QPoint m_dragPos;

    QLineEdit *m_pathEdit = nullptr;
    QCheckBox *m_desktopCheck = nullptr;
    QCheckBox *m_startMenuCheck = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_primaryButton = nullptr;
    QPushButton *m_secondaryButton = nullptr;
};
