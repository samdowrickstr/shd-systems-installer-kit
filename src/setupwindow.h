// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: (c) 2026 SHD Systems Ltd

#pragma once

#include "manifest.h"

#include <QDialog>
#include <QPair>
#include <QPoint>
#include <QString>

#include "hostcapability.h"
#include <QVector>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
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

// One selectable physics module, built from the manifest at runtime.
//
// NOT hard-coded, deliberately: the whole point of driving this from the
// manifest is that adding structural analysis is a publishing change, not an
// installer release. A module appears here because some component in the
// manifest said it serves it.
struct ModuleEntry {
    QString id;                          // "fluids"
    QString label;                       // "Fluids" — derived, or from the manifest
    QString description;
    QList<shdkit::Component> components; // what to fetch if this is ticked
    bool defaultOn = false;
    bool embedded = false;               // already in the payload (offline build)
    QCheckBox *check = nullptr;

    // True when any component here has to run in a container, so the module is
    // only usable on a machine that can virtualise. Drives the capability note
    // on the page — and nothing else: a machine that cannot run it can still
    // download it, because selection is a bandwidth choice.
    bool needsVirtualisation() const {
        for (const shdkit::Component &c : components)
            if (c.needsProvisioning()) return true;
        return false;
    }

    qint64 downloadBytes() const {
        qint64 n = 0;
        for (const shdkit::Component &c : components) n += c.size;
        return n;
    }
};

// Where the installer fetches components from, and what it will believe.
//
// Absent from a config -> the kit behaves exactly as it always has: everything
// embedded, no network, no selection page. Other SHD products use this kit and
// none of them asked for a downloader, so the embedded path stays the default
// rather than becoming a special case of the new one.
struct DownloadConfig {
    bool enabled = false;
    QString baseUrl;        // e.g. https://dl.shd-sim.com
    QString manifestKey;    // e.g. shdsim/stable/1.0.1/release.json
    QString publicKey;      // base64 Ed25519, COMPILED IN via config, never fetched
    QString promptTitle;    // "CHOOSE YOUR PHYSICS"
    QString promptHint;
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
    DownloadConfig download;

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
    // The install is a wizard: one page per decision, in the order the user
    // makes them. Values are the QStackedWidget indices.
    enum Page { PageLocation = 0, PageSoftware = 1, PageProgress = 2, PageComplete = 3 };

    void buildInstallUi();
    QWidget *buildStepper();
    QWidget *buildLocationPage();
    QWidget *buildSoftwarePage();
    QWidget *buildProgressPage();
    QWidget *buildCompletePage();
    void updateStepper();
    Page currentPage() const;
    void showPage(Page page);
    void onPrimaryClicked();
    void goBack();
    // Path checks, run when leaving the location page rather than at the end,
    // so a bad path is rejected where it was typed.
    bool validateLocation(QString *targetOut = nullptr);
    void optionError(const QString &message, int silentCode, Page page);
    void runRequestedAction();

    // --- component selection + fetching ---
    // Reads the manifest and turns it into ModuleEntry rows. Returns false when
    // the manifest could not be had or could not be trusted; the caller carries
    // on WITHOUT a selection page rather than refusing to install, because the
    // application is embedded and does not need the network.
    bool loadModules(QString *whyNot);
    void buildModuleUi(QWidget *card, QVBoxLayout *layout);
    // Fetches the manifest and builds the module rows the first time the user
    // reaches the software page — NOT at construction. The fetch is a blocking
    // round trip, and page one needs nothing from it, so doing it here is the
    // difference between a window that appears at once and one that appears
    // when a website says so.
    void ensureModulesUi();
    QList<shdkit::Component> selectedComponents() const;
    // Downloads and unpacks selected components into the install directory.
    // Returns the components that FAILED — an empty list means everything
    // arrived. A non-empty list is not an install failure (see
    // `writeBackendState`); it is a note for the app to show in Settings.
    QList<shdkit::Component> fetchSelectedComponents(const QString &targetDir);
    // The same download-and-unpack loop over an EXPLICIT list, so the change
    // page can fetch what was just ticked rather than what the install page's
    // checkboxes say — in maintenance mode those checkboxes do not exist.
    // `alreadyInstalled` is carried into components.json unchanged: the state
    // file is the whole picture, and writing only what this run fetched is how
    // adding one backend would erase the record of the other four.
    QList<shdkit::Component> fetchComponents(const QString &targetDir,
                                             const QList<shdkit::Component> &wanted,
                                             const QList<shdkit::Component> &alreadyInstalled = {});
    bool unpackComponent(const shdkit::Component &component, const QString &archivePath,
                         const QString &targetDir, QString *error);
    // Records what is installed and what is missing, for the app to read.
    void writeBackendState(const QString &targetDir,
                           const QList<shdkit::Component> &installed,
                           const QList<shdkit::Component> &failed) const;
    void finishSilent(int code);
    void failSilent(int code, const QString &message);
    void refreshFooter();
    void browseForFolder();
    void startInstall();
    bool copyPayload(const QString &targetDir, int &doneOut, int total);
    bool copyPayloadFile(const QString &sourcePath, const QString &destPath, QString *errorMessage) const;
    int countPayloadFiles() const;
    void finishInstall(const QString &targetDir, const QList<shdkit::Component> &failed = {});
    // Ends on the outcome page in its failed state, offering a way back to the
    // options rather than leaving the user on a dead progress bar.
    void showInstallFailure(const QString &message);

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

    // --- change flow (add / remove backends on an existing install) ---
    //
    // Repair and Update deliberately do not touch components: repair copies the
    // payload back, and the payload is the application. That left no way at all
    // to add a backend to an install that skipped it, or to reclaim the 624 MB
    // one takes — the first-run page was the only place the question was ever
    // asked, and it is asked once.
    // Maintenance is a wizard, like the install: the change page is a decision
    // with a Back, not a modal interruption. Both pages live in one stack under
    // one button row, so the progress bar and status line below them are shared
    // and the window never resizes under the user.
    //
    // A QDialog was tried first, only because makeFrame() puts the layout on the
    // window itself and Qt will not accept a second one — but that is an
    // argument for a stack, which is what the install flow already uses, not for
    // a second window.
    enum MaintPage { MaintChoice = 0, MaintChange = 1 };
    QWidget *buildMaintChoicePage();
    QWidget *buildMaintChangePage();
    void showMaintPage(MaintPage page);
    // Fills the change page from the manifest. Deferred until the page is asked
    // for: the maintenance page needs nothing from the network, and fetching a
    // manifest to build a page nobody opened is the difference between a window
    // that appears at once and one that appears when a website says so.
    bool populateChangePage();
    // One connection each for the life of the window; the page decides what the
    // button means. Re-wiring a button as the flow moves is a bug you only find
    // by clicking it.
    void onMaintPrimary();
    void onMaintSecondary();
    void doChange(const QList<shdkit::Component> &wanted);
    // What <dir>/components.json says is installed. Read rather than inferred
    // from the directories on disk: a half-unpacked folder is a directory too.
    QStringList installedComponentNames(const QString &dir) const;
    bool removeComponent(const QString &targetDir, const QString &name, QString *error) const;

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
    QPushButton *m_changeButton = nullptr;
    QStackedWidget *m_maintPages = nullptr;
    QVBoxLayout *m_changeList = nullptr;   // the checkboxes go in here
    QLabel *m_changeHint = nullptr;
    bool m_changeLoaded = false;
    // Module id -> its checkbox on the change dialog. The same unit the
    // first-run page offers, so adding Thermal later is the same question,
    // worded the same way, as choosing it during the install.
    QList<QPair<QString, QCheckBox *>> m_changeChecks;

    QCheckBox *m_desktopCheck = nullptr;
    QCheckBox *m_startMenuCheck = nullptr;
    QProgressBar *m_progress = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_primaryButton = nullptr;
    QPushButton *m_secondaryButton = nullptr;

    // --- wizard chrome (install mode only; null in maintenance/uninstall) ---
    QStackedWidget *m_pages = nullptr;
    QPushButton *m_backButton = nullptr;
    QLabel *m_sizeLabel = nullptr;
    QVector<Page> m_stepPages;      // pages that get a step in the header
    QVector<QLabel *> m_stepLabels; // one per entry in m_stepPages
    // A product with no apps and no downloads has nothing to ask on page two,
    // so it does not get one.
    bool m_hasSoftwarePage = false;

    QWidget *m_moduleHost = nullptr;

    // Gathered once, when the modules load. Re-read after the elevated fix,
    // because the page must not go on describing facts that have changed.
    shdkit::VirtualisationFacts m_hostFacts;
    QVBoxLayout *m_moduleLayout = nullptr;
    QLabel *m_progressTitle = nullptr;
    QLabel *m_completeTitle = nullptr;
    QLabel *m_completeBody = nullptr;
    QLabel *m_completeDetail = nullptr;
    QString m_launchPath;
    bool m_installFailed = false;

    // Why each component failed, in the order they failed, as "Label — reason".
    //
    // These reasons used to go to qWarning() and nowhere else: the stub links as
    // a GUI-subsystem binary, so there is no stderr, and the finish page listed
    // only the component names. A rename bug that reported "could not be
    // downloaded" after downloading and unpacking perfectly therefore took a
    // system-wide OutputDebugString capture to read a message the installer had
    // already written. The user gets the reason now.
    QStringList m_componentFailures;

    shdkit::Manifest m_manifest;
    QVector<ModuleEntry> m_modules;
    bool m_modulesLoaded = false;
    QLabel *m_downloadSummary = nullptr;
};
