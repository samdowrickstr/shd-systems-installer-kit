#include "setupwindow.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QLineEdit>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTextStream>
#include <QTimer>
#include <QVBoxLayout>

#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>

#include <string>

// ---------------------------------------------------------------------------
// InstallerConfig
// ---------------------------------------------------------------------------
QString InstallerConfig::uninstallRegPath() const
{
    return QStringLiteral(
               "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\")
           + registryKey;
}

QStringList InstallerConfig::appExeNames() const
{
    QStringList names;
    for (const AppEntry &app : apps) {
        names << app.exe;
    }
    return names;
}

namespace {

struct RunningAppProcess {
    DWORD pid = 0;
    QString exeName;
    QString path;
};

QString semVerWithoutBuild(const QString &version)
{
    return version.trimmed().section('+', 0, 0);
}

bool isNumericIdentifier(const QString &identifier)
{
    if (identifier.isEmpty()) {
        return false;
    }
    for (const QChar ch : identifier) {
        if (!ch.isDigit()) {
            return false;
        }
    }
    return true;
}

int compareIdentifierLists(const QString &a, const QString &b)
{
    const QStringList pa = a.split('.');
    const QStringList pb = b.split('.');
    const int n = qMin(pa.size(), pb.size());
    for (int i = 0; i < n; ++i) {
        if (pa.at(i) == pb.at(i)) {
            continue;
        }

        const bool aNum = isNumericIdentifier(pa.at(i));
        const bool bNum = isNumericIdentifier(pb.at(i));
        if (aNum && bNum) {
            const qlonglong ai = pa.at(i).toLongLong();
            const qlonglong bi = pb.at(i).toLongLong();
            if (ai != bi) {
                return ai < bi ? -1 : 1;
            }
        } else if (aNum != bNum) {
            return aNum ? -1 : 1;
        } else {
            const int cmp = QString::compare(pa.at(i), pb.at(i), Qt::CaseSensitive);
            if (cmp != 0) {
                return cmp < 0 ? -1 : 1;
            }
        }
    }

    if (pa.size() == pb.size()) {
        return 0;
    }
    return pa.size() < pb.size() ? -1 : 1;
}

int compareSemVerPrecedence(const QString &a, const QString &b)
{
    const QString aNoBuild = semVerWithoutBuild(a);
    const QString bNoBuild = semVerWithoutBuild(b);
    const QStringList aCore = aNoBuild.section('-', 0, 0).split('.');
    const QStringList bCore = bNoBuild.section('-', 0, 0).split('.');

    for (int i = 0; i < 3; ++i) {
        const int av = i < aCore.size() ? aCore.at(i).toInt() : 0;
        const int bv = i < bCore.size() ? bCore.at(i).toInt() : 0;
        if (av != bv) {
            return av < bv ? -1 : 1;
        }
    }

    const QString aPre = aNoBuild.contains('-') ? aNoBuild.section('-', 1) : QString();
    const QString bPre = bNoBuild.contains('-') ? bNoBuild.section('-', 1) : QString();
    if (aPre.isEmpty() && bPre.isEmpty()) {
        return 0;
    }
    if (aPre.isEmpty() != bPre.isEmpty()) {
        return aPre.isEmpty() ? 1 : -1;
    }
    return compareIdentifierLists(aPre, bPre);
}

bool shouldUpdateToInstaller(const QString &installedVersion, const QString &installerVersion)
{
    const int precedence = compareSemVerPrecedence(installedVersion, installerVersion);
    if (precedence < 0) {
        return true;
    }
    if (precedence > 0) {
        return false;
    }

    // SemVer build metadata does not affect precedence, but a different build
    // of the same version should still be installable as an update.
    return installedVersion.trimmed() != installerVersion.trimmed();
}

QString cleanComparablePath(const QString &path)
{
    return QDir::cleanPath(QDir::fromNativeSeparators(path)).toCaseFolded();
}

QString processImagePath(DWORD pid)
{
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return QString();
    }

    wchar_t buffer[32768] = {};
    DWORD size = static_cast<DWORD>(sizeof(buffer) / sizeof(buffer[0]));
    QString path;
    if (QueryFullProcessImageNameW(process, 0, buffer, &size)) {
        path = QString::fromWCharArray(buffer, static_cast<int>(size));
    }
    CloseHandle(process);
    return QDir::fromNativeSeparators(path);
}

// Enumerate processes whose image is one of our app exes running from targetDir.
QList<RunningAppProcess> runningInstalledAppProcesses(const QString &targetDir,
                                                      const QStringList &appExes)
{
    QStringList wantedPaths;
    for (const QString &exe : appExes) {
        wantedPaths << cleanComparablePath(QDir(targetDir).filePath(exe));
    }

    QList<RunningAppProcess> processes;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return processes;
    }

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            const QString exeName = QString::fromWCharArray(entry.szExeFile);
            bool isAppExe = false;
            for (const QString &exe : appExes) {
                if (exeName.compare(exe, Qt::CaseInsensitive) == 0) {
                    isAppExe = true;
                    break;
                }
            }
            if (!isAppExe) {
                continue;
            }

            const QString path = processImagePath(entry.th32ProcessID);
            if (wantedPaths.contains(cleanComparablePath(path))) {
                processes.append({entry.th32ProcessID, exeName, path});
            }
        } while (Process32NextW(snapshot, &entry));
    }

    CloseHandle(snapshot);
    return processes;
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

void requestProcessesClose(const QList<RunningAppProcess> &processes)
{
    for (const RunningAppProcess &process : processes) {
        EnumWindows(postCloseToProcessWindow, static_cast<LPARAM>(process.pid));
    }
}

bool waitForInstalledAppsToExit(const QString &targetDir, const QStringList &appExes, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (runningInstalledAppProcesses(targetDir, appExes).isEmpty()) {
            return true;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        Sleep(100);
    }
    return runningInstalledAppProcesses(targetDir, appExes).isEmpty();
}

void terminateInstalledApps(const QString &targetDir, const QStringList &appExes)
{
    for (const RunningAppProcess &processInfo : runningInstalledAppProcesses(targetDir, appExes)) {
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, processInfo.pid);
        if (process) {
            TerminateProcess(process, 0);
            CloseHandle(process);
        }
    }
}
} // namespace

SetupWindow::SetupWindow(const InstallerConfig &config, bool uninstallMode, QWidget *parent)
    : QDialog(parent)
    , m_config(config)
{
    m_sourceDir = QCoreApplication::applicationDirPath();
    const bool installed = readInstalledInfo();
    if (uninstallMode) {
        m_mode = Mode::Uninstall;
        m_installedDir = QCoreApplication::applicationDirPath(); // uninstall.exe sits in it
    } else if (installed) {
        m_mode = Mode::Maintenance;
    } else {
        m_mode = Mode::Install;
    }

    setWindowTitle(m_mode == Mode::Uninstall ? m_config.appName + " Uninstall"
                                             : m_config.appName + " Setup");
    setWindowFlags(Qt::Dialog | Qt::FramelessWindowHint);
    setAttribute(Qt::WA_TranslucentBackground);
    applyStyle();

    switch (m_mode) {
    case Mode::Uninstall:    buildUninstallUi();    break;
    case Mode::Maintenance:  buildMaintenanceUi();  break;
    case Mode::Install:      buildInstallUi();      break;
    }
    centreOnPrimary();
}

QString SetupWindow::defaultInstallDir() const
{
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    return QDir(base).filePath("Programs/" + m_config.appName);
}

const AppEntry *SetupWindow::appForExe(const QString &exeName) const
{
    for (const AppEntry &app : m_config.apps) {
        if (app.exe.compare(exeName, Qt::CaseInsensitive) == 0) {
            return &app;
        }
    }
    return nullptr;
}

bool SetupWindow::wantExe(const QString &exeName) const
{
    // App exes are gated by their checkboxes; everything else (the shared Qt
    // runtime) is always installed. Checkboxes are null in maintenance mode ->
    // keep whatever the payload has.
    const AppEntry *app = appForExe(exeName);
    if (!app) {
        return true;
    }
    return !app->check || app->check->isChecked();
}

bool SetupWindow::readInstalledInfo()
{
    QSettings reg(m_config.uninstallRegPath(), QSettings::NativeFormat);
    m_installedDir = QDir::fromNativeSeparators(reg.value("InstallLocation").toString());
    m_installedVersion = reg.value("DisplayVersion").toString();
    if (m_installedDir.isEmpty() || !QDir(m_installedDir).exists()) {
        return false;
    }
    for (const AppEntry &app : m_config.apps) {
        if (QFile::exists(QDir(m_installedDir).filePath(app.exe))) {
            return true;
        }
    }
    return false;
}

void SetupWindow::centreOnPrimary()
{
    const QRect scr = QGuiApplication::primaryScreen()->availableGeometry();
    move(scr.center().x() - width() / 2, scr.center().y() - height() / 2);
}

void SetupWindow::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_dragPos = event->globalPosition().toPoint() - frameGeometry().topLeft();
    }
    QDialog::mousePressEvent(event);
}

void SetupWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton) {
        move(event->globalPosition().toPoint() - m_dragPos);
    }
    QDialog::mouseMoveEvent(event);
}

QVBoxLayout *SetupWindow::makeFrame(bool showSubtitle)
{
    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 16, 16, 16); // room for the drop shadow

    auto *frame = new QFrame(this);
    frame->setObjectName("frame");
    auto *shadow = new QGraphicsDropShadowEffect(frame);
    shadow->setBlurRadius(38);
    shadow->setColor(QColor(15, 23, 42, 90));
    shadow->setOffset(0, 8);
    frame->setGraphicsEffect(shadow);
    outer->addWidget(frame);

    auto *content = new QVBoxLayout(frame);
    content->setContentsMargins(24, 18, 24, 22);
    content->setSpacing(16);

    auto *header = new QHBoxLayout();
    header->setSpacing(14);
    auto *logo = new QLabel(frame);
    logo->setPixmap(brandLogo(36));
    logo->setAlignment(Qt::AlignVCenter);
    header->addWidget(logo, 0, Qt::AlignVCenter);
    auto *titles = new QVBoxLayout();
    titles->setSpacing(0);
    titles->addStretch(1);
    auto *title = new QLabel(m_config.displayName, frame);
    title->setObjectName("title");
    titles->addWidget(title);
    if (showSubtitle && !m_config.subtitle.isEmpty()) {
        auto *sub = new QLabel(m_config.subtitle, frame);
        sub->setObjectName("subtitle");
        titles->addWidget(sub);
    }
    titles->addStretch(1);
    header->addLayout(titles);
    header->addStretch(1);
    auto *closeBtn = new QPushButton(QString::fromUtf8("\xE2\x9C\x95"), frame); // ✕
    closeBtn->setObjectName("close");
    closeBtn->setFixedSize(28, 28);
    closeBtn->setCursor(Qt::PointingHandCursor);
    connect(closeBtn, &QPushButton::clicked, this, &QDialog::reject);
    header->addWidget(closeBtn, 0, Qt::AlignTop);
    content->addLayout(header);

    return content;
}

QPixmap SetupWindow::brandLogo(int height) const
{
    QSvgRenderer renderer;
    if (renderer.load(QStringLiteral(":/setup/logo.svg"))) {
        const QSizeF native = renderer.defaultSize();
        const qreal ratio = native.height() > 0 ? native.width() / native.height() : 2.4;
        const qreal dpr = devicePixelRatioF();
        const int width = qRound(height * ratio);
        QPixmap pm(QSize(qRound(width * dpr), qRound(height * dpr)));
        pm.setDevicePixelRatio(dpr);
        pm.fill(Qt::transparent);
        QPainter p(&pm);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        renderer.render(&p, QRectF(0, 0, width, height));
        p.end();
        return pm;
    }

    // Fall back to a raster logo (logo.png) if no SVG is bundled.
    QPixmap raster(QStringLiteral(":/setup/logo.png"));
    if (!raster.isNull()) {
        return raster.scaledToHeight(qRound(height * devicePixelRatioF()),
                                     Qt::SmoothTransformation);
    }
    return QPixmap();
}

void SetupWindow::applyStyle()
{
    const QString accent = m_config.accentColor.isEmpty() ? QStringLiteral("#1CA3C2")
                                                          : m_config.accentColor;
    setStyleSheet(QString(R"(
        QWidget { color:#15314c; font-family:"Segoe UI",Arial,sans-serif; font-size:10pt; }
        QFrame#frame { background:#eef2f6; border-radius:14px; }
        QPushButton#close { background:transparent; color:#8392a6; border:0; border-radius:14px;
                            font-size:12pt; font-weight:700; padding:0; }
        QPushButton#close:hover { background:#dde4ec; color:#15314c; }
        QLabel { background:transparent; }
        QLabel#title { font-size:17pt; font-weight:700; color:#15314c; }
        QLabel#subtitle { font-size:9pt; font-weight:700; letter-spacing:2px; color:%1; }
        QLabel#section { color:#8392a6; font-size:8pt; font-weight:700; letter-spacing:1px; }
        QFrame#card { background:#ffffff; border:1px solid #e1e7ef; border-radius:12px; }
        QLineEdit { background:#ffffff; border:1px solid #cdd6e2; border-radius:8px; padding:7px 9px; }
        QLineEdit:focus { border:1px solid %1; }
        QCheckBox { color:#15314c; font-weight:600; spacing:8px; background:transparent; }
        QCheckBox::indicator { width:18px; height:18px; border:1px solid #cdd6e2; border-radius:5px; background:#fff; }
        QCheckBox::indicator:checked { background:%1; border:1px solid %1; image:url(:/setup/check.svg); }
        QPushButton { background:#15314c; color:#fff; border:0; border-radius:8px; padding:9px 18px; font-weight:600; }
        QPushButton:hover:enabled { background:#20405c; }
        QPushButton:disabled { background:#dbe2ec; color:#9aa7b8; }
        QPushButton#ghost { background:#fff; color:#15314c; border:1px solid #cdd6e2; }
        QPushButton#ghost:hover:enabled { background:#f1f5f9; }
        QProgressBar { border:1px solid #e1e7ef; border-radius:7px; background:#eef2f6; text-align:center; min-height:12px; color:#52606d; }
        QProgressBar::chunk { background:%1; border-radius:7px; }
    )").arg(accent));
}

// ---------------------------------------------------------------------------
// Install UI
// ---------------------------------------------------------------------------
void SetupWindow::buildInstallUi()
{
    // Size grows with the number of apps so the app list never crowds.
    const int base = 384;
    const int perApp = 26;
    setFixedSize(564, base + m_config.apps.size() * perApp + 114);
    auto *root = makeFrame(true);

    auto *card = new QFrame(this);
    card->setObjectName("card");
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(20, 18, 20, 20);
    cl->setSpacing(12);

    auto *locLabel = new QLabel("INSTALL LOCATION", card);
    locLabel->setObjectName("section");
    cl->addWidget(locLabel);

    auto *locRow = new QHBoxLayout();
    locRow->setSpacing(10);
    m_pathEdit = new QLineEdit(QDir::toNativeSeparators(defaultInstallDir()), card);
    locRow->addWidget(m_pathEdit, 1);
    auto *browse = new QPushButton("Browse…", card);
    browse->setObjectName("ghost");
    connect(browse, &QPushButton::clicked, this, &SetupWindow::browseForFolder);
    locRow->addWidget(browse);
    cl->addLayout(locRow);

    // App checkboxes: one per configured app. When there is only a single app we
    // still show it, but a lone always-on app could also be hidden by config.
    if (!m_config.apps.isEmpty()) {
        cl->addSpacing(6);
        auto *appLabel = new QLabel("APPS TO INSTALL", card);
        appLabel->setObjectName("section");
        cl->addWidget(appLabel);

        for (AppEntry &app : m_config.apps) {
            const QString label = app.description.isEmpty()
                                      ? app.name
                                      : app.name + "  —  " + app.description;
            app.check = new QCheckBox(label, card);
            app.check->setChecked(app.defaultOn);
            cl->addWidget(app.check);
            connect(app.check, &QCheckBox::toggled, this, &SetupWindow::refreshFooter);
        }
    }

    cl->addSpacing(6);
    auto *optLabel = new QLabel("OPTIONS", card);
    optLabel->setObjectName("section");
    cl->addWidget(optLabel);

    m_desktopCheck = new QCheckBox("Create desktop shortcut(s)", card);
    m_desktopCheck->setChecked(m_config.desktopShortcut);
    cl->addWidget(m_desktopCheck);
    m_startMenuCheck = new QCheckBox("Create Start Menu shortcut(s)", card);
    m_startMenuCheck->setChecked(m_config.startMenuShortcut);
    cl->addWidget(m_startMenuCheck);

    root->addWidget(card);
    root->addStretch(1);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progress->hide();
    root->addWidget(m_progress);

    m_status = new QLabel(this);
    m_status->setStyleSheet("color:#5a6b80;");
    root->addWidget(m_status);
    refreshFooter();

    auto *btnRow = new QHBoxLayout();
    auto *attribution = new QLabel(
        "Powered by the PlannerDay Installer Kit  \u00b7  \u00a9 PlannerDay Ltd (MIT)", this);
    attribution->setStyleSheet("color:#9aa7b8; font-size:11px;");
    btnRow->addWidget(attribution);
    btnRow->addStretch(1);
    m_secondaryButton = new QPushButton("Cancel", this);
    m_secondaryButton->setObjectName("ghost");
    connect(m_secondaryButton, &QPushButton::clicked, this, &QWidget::close);
    btnRow->addWidget(m_secondaryButton);
    m_primaryButton = new QPushButton("Install", this);
    m_primaryButton->setCursor(Qt::PointingHandCursor);
    m_primaryButton->setDefault(true);
    connect(m_primaryButton, &QPushButton::clicked, this, &SetupWindow::startInstall);
    btnRow->addWidget(m_primaryButton);
    root->addLayout(btnRow);
}

void SetupWindow::browseForFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, "Choose install location", m_pathEdit->text());
    if (!dir.isEmpty()) {
        m_pathEdit->setText(QDir::toNativeSeparators(QDir(dir).filePath(m_config.appName)));
    }
}

void SetupWindow::refreshFooter()
{
    const QString self = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    qint64 bytes = 0;
    QDirIterator it(m_sourceDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString name = it.fileName();
        if (name.compare(self, Qt::CaseInsensitive) == 0) continue;
        if (!wantExe(name)) continue;
        bytes += it.fileInfo().size();
    }
    const double mb = bytes / (1024.0 * 1024.0);
    m_status->setText(QString("Requires about %1 MB of disk space   ·   Version %2")
                          .arg(QString::number(mb, 'f', 0), m_config.version));
}

int SetupWindow::countPayloadFiles() const
{
    const QString self = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    int n = 0;
    QDirIterator it(m_sourceDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString name = it.fileName();
        if (name.compare(self, Qt::CaseInsensitive) == 0) continue;
        if (!wantExe(name)) continue;
        ++n;
    }
    return n;
}

bool SetupWindow::copyPayload(const QString &targetDir, int &doneOut, int total)
{
    const QString self = QFileInfo(QCoreApplication::applicationFilePath()).fileName();
    QDir src(m_sourceDir);
    QDirIterator it(m_sourceDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        const QString name = it.fileName();
        if (name.compare(self, Qt::CaseInsensitive) == 0) continue;
        if (!wantExe(name)) continue;

        const QString rel = src.relativeFilePath(it.filePath());
        const QString dest = QDir(targetDir).filePath(rel);
        QString errorMessage;
        if (!copyPayloadFile(it.filePath(), dest, &errorMessage)) {
            QMessageBox::critical(this, m_config.appName + " Setup", errorMessage);
            return false;
        }
        ++doneOut;
        if (total > 0) {
            m_progress->setValue(qRound(100.0 * doneOut / total));
        }
        QCoreApplication::processEvents();
    }
    return true;
}

bool SetupWindow::copyPayloadFile(const QString &sourcePath, const QString &destPath,
                                  QString *errorMessage) const
{
    const QFileInfo srcInfo(sourcePath);
    const QFileInfo destInfo(destPath);
    const QString nativeDest = QDir::toNativeSeparators(destPath);

    if (srcInfo.absoluteFilePath().compare(destInfo.absoluteFilePath(), Qt::CaseInsensitive) == 0) {
        return true;
    }

    if (!QDir().mkpath(destInfo.absolutePath())) {
        if (errorMessage) {
            *errorMessage = "Could not create:\n" + QDir::toNativeSeparators(destInfo.absolutePath());
        }
        return false;
    }

    const QString tempPath = destPath + QString(".setup-update-%1.tmp").arg(QCoreApplication::applicationPid());
    QFile::remove(tempPath);

    QFile source(sourcePath);
    if (!source.copy(tempPath)) {
        if (errorMessage) {
            *errorMessage = QString("Failed to prepare:\n%1\n\nDetails: %2")
                                .arg(nativeDest, source.errorString());
        }
        return false;
    }

    if (QFile::exists(destPath)) {
        QFile existing(destPath);
        if (!existing.remove()) {
            QFile::remove(tempPath);
            if (errorMessage) {
                const QString fileName = destInfo.fileName();
                const bool appExecutable = appForExe(fileName) != nullptr;
                if (appExecutable) {
                    *errorMessage =
                        QString("Could not update %1 because Windows is still using this file:\n%2\n\n"
                                "Close any running app windows, then try again.\n\n"
                                "Details: %3")
                            .arg(m_config.appName, nativeDest, existing.errorString());
                } else {
                    *errorMessage = QString("Could not replace:\n%1\n\nDetails: %2")
                                        .arg(nativeDest, existing.errorString());
                }
            }
            return false;
        }
    }

    QFile temp(tempPath);
    if (!temp.rename(destPath)) {
        const QString detail = temp.errorString();
        QFile::remove(tempPath);
        if (errorMessage) {
            *errorMessage = QString("Failed to copy:\n%1\n\nDetails: %2")
                                .arg(nativeDest, detail);
        }
        return false;
    }

    return true;
}

void SetupWindow::startInstall()
{
    const QString targetDir = QDir::cleanPath(QDir::fromNativeSeparators(m_pathEdit->text().trimmed()));
    if (targetDir.isEmpty()) {
        QMessageBox::warning(this, m_config.appName + " Setup", "Please choose an install location.");
        return;
    }
    if (QDir(targetDir) == QDir(m_sourceDir)) {
        QMessageBox::warning(this, m_config.appName + " Setup",
                             "Please choose a different folder from the installer's own location.");
        return;
    }

    bool anySelected = m_config.apps.isEmpty();
    for (const AppEntry &app : m_config.apps) {
        if (app.check && app.check->isChecked()) {
            anySelected = true;
            break;
        }
    }
    if (!anySelected) {
        QMessageBox::warning(this, m_config.appName + " Setup",
                             "Select at least one app to install.");
        return;
    }

    m_pathEdit->setEnabled(false);
    for (const AppEntry &app : m_config.apps) {
        if (app.check) app.check->setEnabled(false);
    }
    m_desktopCheck->setEnabled(false);
    m_startMenuCheck->setEnabled(false);
    m_primaryButton->setEnabled(false);
    m_secondaryButton->setEnabled(false);
    m_progress->show();
    m_status->setText("Installing…");
    QCoreApplication::processEvents();

    auto reenable = [this] {
        m_pathEdit->setEnabled(true);
        for (const AppEntry &app : m_config.apps) {
            if (app.check) app.check->setEnabled(true);
        }
        m_desktopCheck->setEnabled(true);
        m_startMenuCheck->setEnabled(true);
        m_primaryButton->setEnabled(true);
        m_secondaryButton->setEnabled(true);
    };

    if (!QDir().mkpath(targetDir)) {
        QMessageBox::critical(this, m_config.appName + " Setup",
                              "Could not create:\n" + QDir::toNativeSeparators(targetDir));
        reenable();
        return;
    }

    if (!closeRunningInstalledApps(targetDir)) {
        reenable();
        m_progress->hide();
        m_status->setText("Install cancelled. Close the app and try again.");
        return;
    }

    const int total = countPayloadFiles();
    int done = 0;
    if (!copyPayload(targetDir, done, total)) {
        reenable();
        m_status->setText("Install failed. Close any running app windows and try again.");
        return;
    }

    // Drop a copy of ourselves as the uninstaller.
    const QString uninstPath = QDir(targetDir).filePath("uninstall.exe");
    QString uninstError;
    if (!copyPayloadFile(QCoreApplication::applicationFilePath(), uninstPath, &uninstError)) {
        QMessageBox::critical(this, m_config.appName + " Setup", uninstError);
        reenable();
        m_status->setText("Install failed. Close any running app windows and try again.");
        return;
    }

    finishInstall(targetDir);
}

void SetupWindow::finishInstall(const QString &targetDir)
{
    QString launchPath; // first installed app
    for (const AppEntry &app : m_config.apps) {
        if (!wantExe(app.exe)) continue;
        const QString exePath = QDir(targetDir).filePath(app.exe);
        if (m_desktopCheck->isChecked()) {
            const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
            createShortcut(QDir(desktop).filePath(app.name + ".lnk"), exePath, QString(), targetDir, app.name);
        }
        if (m_startMenuCheck->isChecked()) {
            const QString smDir = startMenuDir();
            QDir().mkpath(smDir);
            createShortcut(QDir(smDir).filePath(app.name + ".lnk"), exePath, QString(), targetDir, app.name);
        }
        if (launchPath.isEmpty()) {
            launchPath = exePath;
        }
    }

    if (m_startMenuCheck->isChecked()) {
        const QString smDir = startMenuDir();
        QDir().mkpath(smDir);
        createShortcut(QDir(smDir).filePath("Uninstall " + m_config.appName + ".lnk"),
                       QDir(targetDir).filePath("uninstall.exe"), "--uninstall", targetDir,
                       "Uninstall " + m_config.appName);
    }

    // Estimated size (KB) for Add/Remove Programs.
    qint64 bytes = 0;
    QDirIterator it(targetDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); bytes += it.fileInfo().size(); }
    writeUninstallInfo(targetDir, static_cast<int>(bytes / 1024));

    // Ask the shell to refresh icons so the new app/shortcut icon shows without
    // waiting for the Windows icon cache to expire.
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);

    m_progress->setValue(100);
    m_status->setText(m_config.appName + " has been installed.");
    m_primaryButton->setText("Launch");
    m_primaryButton->setEnabled(!launchPath.isEmpty());
    m_secondaryButton->setText("Close");
    m_secondaryButton->setEnabled(true);
    m_primaryButton->disconnect();
    connect(m_primaryButton, &QPushButton::clicked, this, [this, launchPath] {
        if (!launchPath.isEmpty()) {
            QProcess::startDetached(launchPath, {});
        }
        close();
    });
}

// ---------------------------------------------------------------------------
// Maintenance UI / flow (shown when an install already exists)
// ---------------------------------------------------------------------------
void SetupWindow::buildMaintenanceUi()
{
    setFixedSize(520, 372);
    auto *root = makeFrame(false);

    const bool update = shouldUpdateToInstaller(m_installedVersion, m_config.version);

    auto *msg = new QLabel(m_config.appName + " is already installed on this computer.", this);
    msg->setWordWrap(true);
    msg->setStyleSheet("color:#15314c; font-weight:600; font-size:11pt;");
    root->addWidget(msg);

    auto *info = new QLabel(this);
    info->setWordWrap(true);
    info->setStyleSheet("color:#5a6b80;");
    const QString ver = m_installedVersion.isEmpty() ? QString()
                                                     : QString("Installed version %1   ·   ").arg(m_installedVersion);
    info->setText(ver + QDir::toNativeSeparators(m_installedDir));
    root->addWidget(info);

    auto *choose = new QLabel(this);
    choose->setWordWrap(true);
    choose->setStyleSheet("color:#5a6b80;");
    choose->setText(update
        ? QString("This installer is version %1. Choose <b>Update</b> to upgrade, or <b>Uninstall</b> to remove it.").arg(m_config.version)
        : QString("Choose <b>Repair</b> to reinstall the files, or <b>Uninstall</b> to remove it."));
    root->addWidget(choose);

    root->addStretch(1);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    m_progress->hide();
    root->addWidget(m_progress);
    m_status = new QLabel(this);
    m_status->setStyleSheet("color:#5a6b80;");
    root->addWidget(m_status);

    auto *btnRow = new QHBoxLayout();
    m_secondaryButton = new QPushButton("Uninstall", this);
    m_secondaryButton->setObjectName("ghost");
    connect(m_secondaryButton, &QPushButton::clicked, this, [this] {
        const QString u = QDir(m_installedDir).filePath("uninstall.exe");
        if (QFile::exists(u)) {
            QProcess::startDetached(u, {"--uninstall"});
        }
        close();
    });
    btnRow->addWidget(m_secondaryButton);
    btnRow->addStretch(1);
    m_primaryButton = new QPushButton(update ? QString("Update to %1").arg(m_config.version) : QString("Repair"), this);
    m_primaryButton->setCursor(Qt::PointingHandCursor);
    m_primaryButton->setDefault(true);
    connect(m_primaryButton, &QPushButton::clicked, this, &SetupWindow::doRepairOrUpdate);
    btnRow->addWidget(m_primaryButton);
    root->addLayout(btnRow);
}

bool SetupWindow::closeRunningInstalledApps(const QString &targetDir)
{
    const QStringList appExes = m_config.appExeNames();
    const QList<RunningAppProcess> running = runningInstalledAppProcesses(targetDir, appExes);
    if (running.isEmpty()) {
        return true;
    }

    QStringList processLines;
    for (const RunningAppProcess &process : running) {
        processLines << QString("%1 (PID %2)").arg(process.exeName).arg(process.pid);
    }

    const int closeChoice = QMessageBox::question(
        this,
        m_config.appName + " Setup",
        QString("%1 is currently running and must be closed before setup can continue.\n\n"
                "Running process%2:\n%3\n\n"
                "Close %1 now and continue?")
            .arg(m_config.appName)
            .arg(running.size() == 1 ? QString() : QStringLiteral("es"))
            .arg(processLines.join('\n')),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);
    if (closeChoice != QMessageBox::Yes) {
        return false;
    }

    if (m_status) {
        m_status->setText("Closing " + m_config.appName + "…");
    }
    QCoreApplication::processEvents();

    requestProcessesClose(running);
    if (waitForInstalledAppsToExit(targetDir, appExes, 10000)) {
        return true;
    }

    const int forceChoice = QMessageBox::warning(
        this,
        m_config.appName + " Setup",
        m_config.appName + " did not close after 10 seconds.\n\n"
        "Force it to close and continue setup?",
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (forceChoice != QMessageBox::Yes) {
        return false;
    }

    terminateInstalledApps(targetDir, appExes);
    if (waitForInstalledAppsToExit(targetDir, appExes, 5000)) {
        return true;
    }

    QMessageBox::critical(
        this,
        m_config.appName + " Setup",
        "Setup could not close the running application. Close " + m_config.appName
            + " manually, then try again.");
    return false;
}

void SetupWindow::doRepairOrUpdate()
{
    const bool update = shouldUpdateToInstaller(m_installedVersion, m_config.version);
    const QString target = m_installedDir;
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    bool desktopExisted = false;
    for (const AppEntry &app : m_config.apps) {
        if (QFile::exists(QDir(desktop).filePath(app.name + ".lnk"))) {
            desktopExisted = true;
            break;
        }
    }

    m_primaryButton->setEnabled(false);
    m_secondaryButton->setEnabled(false);
    m_progress->show();
    m_status->setText(update ? "Updating files…" : "Repairing files…");
    QCoreApplication::processEvents();

    if (!closeRunningInstalledApps(target)) {
        m_primaryButton->setEnabled(true);
        m_secondaryButton->setEnabled(true);
        m_progress->hide();
        m_status->setText(update
            ? "Update cancelled. Close " + m_config.appName + " and try again."
            : "Repair cancelled. Close " + m_config.appName + " and try again.");
        return;
    }

    const int total = countPayloadFiles();
    int done = 0;
    if (!copyPayload(target, done, total)) {
        m_primaryButton->setEnabled(true);
        m_secondaryButton->setEnabled(true);
        m_status->setText(update
            ? "Update failed. Close any running app windows and try again."
            : "Repair failed. Close any running app windows and try again.");
        return;
    }

    const QString uninstPath = QDir(target).filePath("uninstall.exe");
    QString uninstError;
    if (!copyPayloadFile(QCoreApplication::applicationFilePath(), uninstPath, &uninstError)) {
        QMessageBox::critical(this, m_config.appName + " Setup", uninstError);
        m_primaryButton->setEnabled(true);
        m_secondaryButton->setEnabled(true);
        m_status->setText(update
            ? "Update failed. Close any running app windows and try again."
            : "Repair failed. Close any running app windows and try again.");
        return;
    }

    const QString smDir = startMenuDir();
    QDir().mkpath(smDir);
    QString launchPath;
    for (const AppEntry &app : m_config.apps) {
        const QString exePath = QDir(target).filePath(app.exe);
        if (!QFile::exists(exePath)) continue;
        createShortcut(QDir(smDir).filePath(app.name + ".lnk"), exePath, QString(), target, app.name);
        if (desktopExisted) {
            createShortcut(QDir(desktop).filePath(app.name + ".lnk"), exePath, QString(), target, app.name);
        }
        if (launchPath.isEmpty()) {
            launchPath = exePath;
        }
    }
    createShortcut(QDir(smDir).filePath("Uninstall " + m_config.appName + ".lnk"), uninstPath, "--uninstall",
                   target, "Uninstall " + m_config.appName);

    qint64 bytes = 0;
    QDirIterator it(target, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); bytes += it.fileInfo().size(); }
    writeUninstallInfo(target, static_cast<int>(bytes / 1024));

    m_progress->setValue(100);
    m_status->setText(update ? m_config.appName + " has been updated." : m_config.appName + " has been repaired.");
    m_secondaryButton->setText("Close");
    m_secondaryButton->setEnabled(true);
    m_secondaryButton->disconnect();
    connect(m_secondaryButton, &QPushButton::clicked, this, &QWidget::close);
    m_primaryButton->setText("Launch");
    m_primaryButton->setEnabled(!launchPath.isEmpty());
    m_primaryButton->disconnect();
    connect(m_primaryButton, &QPushButton::clicked, this, [this, launchPath] {
        if (!launchPath.isEmpty()) {
            QProcess::startDetached(launchPath, {});
        }
        close();
    });
}

// ---------------------------------------------------------------------------
// Uninstall UI / flow
// ---------------------------------------------------------------------------
void SetupWindow::buildUninstallUi()
{
    setFixedSize(464, 316);
    auto *root = makeFrame(false);

    auto *msg = new QLabel("This will remove " + m_config.appName + " and its shortcuts from this computer.", this);
    msg->setWordWrap(true);
    msg->setStyleSheet("color:#5a6b80;");
    root->addWidget(msg);

    m_progress = new QProgressBar(this);
    m_progress->setRange(0, 0); // busy
    m_progress->hide();
    root->addWidget(m_progress);

    m_status = new QLabel(QString(), this);
    m_status->setStyleSheet("color:#5a6b80;");
    root->addWidget(m_status);
    root->addStretch(1);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    m_secondaryButton = new QPushButton("Cancel", this);
    m_secondaryButton->setObjectName("ghost");
    connect(m_secondaryButton, &QPushButton::clicked, this, &QWidget::close);
    btnRow->addWidget(m_secondaryButton);
    m_primaryButton = new QPushButton("Uninstall", this);
    m_primaryButton->setDefault(true);
    connect(m_primaryButton, &QPushButton::clicked, this, &SetupWindow::startUninstall);
    btnRow->addWidget(m_primaryButton);
    root->addLayout(btnRow);
}

void SetupWindow::startUninstall()
{
    m_primaryButton->setEnabled(false);
    m_secondaryButton->setEnabled(false);
    m_progress->show();
    m_status->setText("Removing…");
    QCoreApplication::processEvents();

    // Remove shortcuts.
    for (const QString &lnk : shortcutPaths()) {
        QFile::remove(lnk);
    }
    QDir(startMenuDir()).removeRecursively();
    removeUninstallInfo();

    // The install folder holds this running uninstaller + its loaded Qt DLLs, so
    // it can't delete itself directly. Write a tiny PowerShell helper that waits
    // for THIS process to exit (by PID), removes the folder, then deletes itself,
    // and launch it hidden.
    const QString dir = QDir::toNativeSeparators(QCoreApplication::applicationDirPath());
    const QString ps1 = QDir::toNativeSeparators(QDir(QDir::tempPath()).filePath("appsetup_cleanup.ps1"));
    QFile sf(ps1);
    if (sf.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream ts(&sf);
        ts << "Wait-Process -Id " << QCoreApplication::applicationPid()
           << " -Timeout 30 -ErrorAction SilentlyContinue\r\n"
           << "Start-Sleep -Milliseconds 400\r\n"
           << "Remove-Item -LiteralPath '" << dir << "' -Recurse -Force -ErrorAction SilentlyContinue\r\n"
           << "Remove-Item -LiteralPath '" << ps1 << "' -Force -ErrorAction SilentlyContinue\r\n";
        sf.close();

        std::wstring cmdLine =
            QString("powershell.exe -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%1\"")
                .arg(ps1).toStdWString();
        std::wstring cwd = QDir::toNativeSeparators(QDir::tempPath()).toStdWString();
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        if (CreateProcessW(nullptr, cmdLine.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, cwd.c_str(), &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
    }

    m_status->setText(m_config.appName + " has been removed.");
    QTimer::singleShot(1000, this, &QWidget::close);
}

// ---------------------------------------------------------------------------
// Win32 helpers
// ---------------------------------------------------------------------------
bool SetupWindow::createShortcut(const QString &linkPath, const QString &target, const QString &args,
                                 const QString &workingDir, const QString &description) const
{
    bool ok = false;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IShellLinkW *psl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_IShellLinkW, reinterpret_cast<void **>(&psl)))) {
        psl->SetPath(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(target).utf16()));
        // Point the shortcut icon explicitly at the target exe's first icon.
        psl->SetIconLocation(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(target).utf16()), 0);
        psl->SetWorkingDirectory(reinterpret_cast<const wchar_t *>(QDir::toNativeSeparators(workingDir).utf16()));
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
}

void SetupWindow::writeUninstallInfo(const QString &targetDir, int sizeKb) const
{
    QSettings reg(m_config.uninstallRegPath(), QSettings::NativeFormat);
    const QString nativeDir = QDir::toNativeSeparators(targetDir);
    reg.setValue("DisplayName", m_config.appName);
    reg.setValue("DisplayVersion", m_config.version);
    reg.setValue("Publisher", m_config.publisher);
    reg.setValue("InstallLocation", nativeDir);
    const QString iconExe = m_config.apps.isEmpty() ? QString() : m_config.apps.first().exe;
    if (!iconExe.isEmpty()) {
        reg.setValue("DisplayIcon", QDir::toNativeSeparators(QDir(targetDir).filePath(iconExe)));
    }
    reg.setValue("UninstallString",
                 QString("\"%1\" --uninstall").arg(QDir::toNativeSeparators(QDir(targetDir).filePath("uninstall.exe"))));
    reg.setValue("EstimatedSize", sizeKb);
    reg.setValue("NoModify", 1);
    reg.setValue("NoRepair", 1);
}

void SetupWindow::removeUninstallInfo() const
{
    QSettings reg(m_config.uninstallRegPath(), QSettings::NativeFormat);
    reg.clear();
}

QString SetupWindow::startMenuDir() const
{
    const QString programs = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    return QDir(programs).filePath(m_config.appName);
}

QStringList SetupWindow::shortcutPaths() const
{
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    QStringList paths;
    for (const AppEntry &app : m_config.apps) {
        paths << QDir(desktop).filePath(app.name + ".lnk");
    }
    paths << QDir(desktop).filePath(m_config.appName + ".lnk"); // legacy single-app shortcut
    return paths;
}
