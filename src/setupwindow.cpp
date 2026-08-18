// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: (c) 2026 SHD Systems Ltd

#include "setupwindow.h"

#include "hostcapability.h"

#include "fetcher.h"

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
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QSvgRenderer>
#include <QTextStream>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include "platform.h"

#ifdef Q_OS_WIN
#include <windows.h>
#include <tlhelp32.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#endif

#include <string>

namespace {

// Which QSettings backend the install record uses.
//
// NativeFormat on Windows means the registry, which is the whole point there.
// NativeFormat on Linux would ALSO work — it would silently put the record in
// ~/.config under an organisation name QSettings derives itself — and that is
// exactly why this is explicit: the record would land somewhere other than the
// path `platform::installRecordPath()` reports, and the two would disagree
// about where the install is recorded without either being obviously wrong.
QSettings::Format installRecordFormat()
{
    return platform::installRecordIsRegistry() ? QSettings::NativeFormat : QSettings::IniFormat;
}

}  // namespace

// ---------------------------------------------------------------------------
// InstallerConfig
// ---------------------------------------------------------------------------
QString InstallerConfig::uninstallRegPath() const
{
    // Named for what it is on Windows, where it is a registry path that
    // Add/Remove Programs reads. On Linux `platform` returns a file path
    // instead and `installRecordFormat()` below picks the matching
    // QSettings::Format — there is no registry, and the thing that puts the
    // application in a menu is the .desktop entry, not this.
    return platform::installRecordPath(publisher, registryKey);
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

// Finding and closing processes running out of the install directory moved to
// `platform`: enumerating them is Toolhelp32 or /proc, and asking one to close
// is a posted WM_CLOSE or a SIGTERM. The name is kept so the call sites below
// read as they did.
using RunningAppProcess = platform::RunningProcess;

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

// Enumerate processes whose image is one of our app exes running from targetDir.
QList<RunningAppProcess> runningInstalledAppProcesses(const QString &targetDir,
                                                      const QStringList &appExes)
{
    return platform::processesRunningFrom(targetDir, appExes);
}

bool waitForInstalledAppsToExit(const QString &targetDir, const QStringList &appExes, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeoutMs) {
        if (runningInstalledAppProcesses(targetDir, appExes).isEmpty()) {
            return true;
        }
        // processEvents already yields for up to 100 ms; QThread::msleep is the
        // portable form of the extra pause that followed it.
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        QThread::msleep(100);
    }
    return runningInstalledAppProcesses(targetDir, appExes).isEmpty();
}

void terminateInstalledApps(const QString &targetDir, const QStringList &appExes)
{
    platform::terminateProcesses(runningInstalledAppProcesses(targetDir, appExes));
}
} // namespace

SetupWindow::SetupWindow(const InstallerConfig &config, SetupAction action, bool silent, QWidget *parent)
    : QDialog(parent)
    , m_config(config)
    , m_requestedAction(action)
    , m_silent(silent)
{
    m_sourceDir = QCoreApplication::applicationDirPath();
    const bool installed = readInstalledInfo();
    if (action == SetupAction::Uninstall) {
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

    if (m_silent || m_requestedAction != SetupAction::Auto) {
        QTimer::singleShot(0, this, &SetupWindow::runRequestedAction);
    }
}

QString SetupWindow::defaultInstallDir() const
{
#ifdef Q_OS_WIN
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    return QDir(base).filePath("Programs/" + m_config.appName);
#else
    // ~/.local/share/<appName>, and NOT the Windows shape.
    //
    // AppDataLocation on Linux folds in organisationName and applicationName,
    // which main.cpp sets to the publisher and "<appName> Setup" — so the
    // Windows expression produced
    //   ~/.local/share/SHD Systems Ltd/SHD Sim Setup/Programs/SHD Sim
    // which names the installer twice, calls a Linux directory "Programs", and
    // buries the application four levels down. GenericDataLocation is the
    // XDG data dir itself, with nothing folded in.
    const QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return QDir(base).filePath(m_config.appName);
#endif
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
    QSettings reg(m_config.uninstallRegPath(), installRecordFormat());
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
    logo->setPixmap(brandLogo(42));
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
        QPushButton { background:#15314c; color:#fff; border:0; border-radius:8px; padding:9px 18px; font-weight:600; min-width:84px; }
        QPushButton:hover:enabled { background:#20405c; }
        QPushButton:disabled { background:#dbe2ec; color:#9aa7b8; }
        QPushButton#ghost { background:#fff; color:#15314c; border:1px solid #cdd6e2; }
        QPushButton#ghost:hover:enabled { background:#f1f5f9; }
        QProgressBar { border:1px solid #e1e7ef; border-radius:7px; background:#eef2f6; text-align:center; min-height:12px; color:#52606d; }
        QProgressBar::chunk { background:%1; border-radius:7px; }
    )").arg(accent));
}

void SetupWindow::finishSilent(int code)
{
    if (!m_silent) {
        return;
    }
    done(code);
    QCoreApplication::exit(code);
}

void SetupWindow::failSilent(int code, const QString &message)
{
    if (m_status) {
        m_status->setText(message);
    }
    finishSilent(code);
}

void SetupWindow::runRequestedAction()
{
    if (m_mode == Mode::Install) {
        if (m_requestedAction == SetupAction::Update || m_requestedAction == SetupAction::Repair) {
            failSilent(SetupExitNotInstalled, m_config.appName + " is not installed.");
            return;
        }
        if (m_silent || m_requestedAction == SetupAction::Install) {
            startInstall();
        }
        return;
    }

    if (m_mode == Mode::Maintenance) {
        if (m_requestedAction == SetupAction::Install) {
            failSilent(SetupExitAlreadyCurrent, m_config.appName + " is already installed.");
            return;
        }
        if (m_requestedAction == SetupAction::Uninstall) {
            QStringList args{"--uninstall"};
            if (m_silent) {
                args << "--silent";
            }
            QProcess::startDetached(QDir(m_installedDir).filePath(platform::executableName("uninstall")), args);
            finishSilent(SetupExitSuccess);
            return;
        }
        if (m_silent || m_requestedAction == SetupAction::Update || m_requestedAction == SetupAction::Repair) {
            doRepairOrUpdate();
        }
        return;
    }

    if (m_mode == Mode::Uninstall && (m_silent || m_requestedAction == SetupAction::Uninstall)) {
        startUninstall();
    }
}

// ---------------------------------------------------------------------------
// Install UI — a wizard, one page per decision
//
// Where it goes → what goes in it → the wait → the outcome. The single page
// this replaced put a folder picker, an app list, a network-fetched module list
// with download sizes, a disk-space warning and the Install button in one
// eyeful, and grew taller with every app and every published module until it
// ran off a laptop screen.
//
// Splitting it buys more than room: the manifest is only fetched when the user
// reaches the page that needs it, so the window no longer waits on a website
// before it will appear.
// ---------------------------------------------------------------------------
namespace {

QString stepTitle(int page)
{
    switch (page) {
    case 0:  return QStringLiteral("LOCATION");
    case 1:  return QStringLiteral("SOFTWARE");
    case 2:  return QStringLiteral("INSTALL");
    default: return QStringLiteral("FINISH");
    }
}

}  // namespace

void SetupWindow::buildInstallUi()
{
    // One size for every page. The pages themselves absorb the difference —
    // the software page scrolls — so the window never resizes under the user
    // and never has to guess how many modules a future release will publish.
    setFixedSize(604, 536);
    auto *root = makeFrame(true);

    m_hasSoftwarePage = !m_config.apps.isEmpty() || m_config.download.enabled;

    root->addWidget(buildStepper());

    m_pages = new QStackedWidget(this);
    m_pages->addWidget(buildLocationPage());   // PageLocation
    m_pages->addWidget(buildSoftwarePage());   // PageSoftware
    m_pages->addWidget(buildProgressPage());   // PageProgress
    m_pages->addWidget(buildCompletePage());   // PageComplete
    root->addWidget(m_pages, 1);

    m_sizeLabel = new QLabel(this);
    m_sizeLabel->setWordWrap(true);
    m_sizeLabel->setStyleSheet("color:#5a6b80;");
    root->addWidget(m_sizeLabel);

    auto *btnRow = new QHBoxLayout();
    btnRow->addStretch(1);
    m_backButton = new QPushButton("Back", this);
    m_backButton->setObjectName("ghost");
    connect(m_backButton, &QPushButton::clicked, this, &SetupWindow::goBack);
    btnRow->addWidget(m_backButton);
    m_secondaryButton = new QPushButton("Cancel", this);
    m_secondaryButton->setObjectName("ghost");
    connect(m_secondaryButton, &QPushButton::clicked, this, &QWidget::close);
    btnRow->addWidget(m_secondaryButton);
    m_primaryButton = new QPushButton("Next", this);
    m_primaryButton->setCursor(Qt::PointingHandCursor);
    m_primaryButton->setDefault(true);
    // One connection for the life of the window: the page decides what the
    // button means. The old flow re-wired this button as it went, and a
    // half-rewired button is a bug you only find by clicking it.
    connect(m_primaryButton, &QPushButton::clicked, this, &SetupWindow::onPrimaryClicked);
    btnRow->addWidget(m_primaryButton);
    root->addLayout(btnRow);

#ifndef SHD_WHITELABEL
    // Attribution Notice required under AGPLv3 §7(b); see ATTRIBUTION.md. It must
    // stay visible under the free (AGPL) licence. Removal / white-label is only
    // permitted under a commercial licence — build with -DSHD_WHITELABEL.
    // TODO: hyperlink to SHD Systems' official URL once finalised.
    auto *attribution = new QLabel(
        "Powered by SHD Systems  ·  © 2026 SHD Systems Ltd", this);
    attribution->setStyleSheet("color:#9aa7b8; font-size:11px;");
    attribution->setWordWrap(true);
    root->addWidget(attribution);
#endif

    refreshFooter();
    showPage(PageLocation);
}

QWidget *SetupWindow::buildStepper()
{
    auto *bar = new QWidget(this);
    auto *row = new QHBoxLayout(bar);
    row->setContentsMargins(2, 0, 2, 2);
    row->setSpacing(9);

    m_stepPages.clear();
    m_stepLabels.clear();
    m_stepPages << PageLocation;
    if (m_hasSoftwarePage) {
        m_stepPages << PageSoftware;
    }
    m_stepPages << PageProgress << PageComplete;

    for (int i = 0; i < m_stepPages.size(); ++i) {
        if (i > 0) {
            auto *sep = new QLabel(QString::fromUtf8("\xE2\x80\xBA"), bar); // ›
            sep->setStyleSheet("color:#c3ccd8; font-size:9pt;");
            row->addWidget(sep);
        }
        auto *step = new QLabel(bar);
        m_stepLabels << step;
        row->addWidget(step);
    }
    row->addStretch(1);
    return bar;
}

void SetupWindow::updateStepper()
{
    if (m_stepLabels.isEmpty()) {
        return;
    }
    const QString accent = m_config.accentColor.isEmpty() ? QStringLiteral("#1CA3C2")
                                                          : m_config.accentColor;
    const int current = m_stepPages.indexOf(currentPage());
    for (int i = 0; i < m_stepLabels.size(); ++i) {
        QLabel *label = m_stepLabels.at(i);
        const bool done = current >= 0 && i < current;
        const QString mark = done ? QString::fromUtf8("\xE2\x9C\x93")   // ✓
                                  : QString::number(i + 1);
        label->setText(QStringLiteral("%1  %2").arg(mark, stepTitle(m_stepPages.at(i))));
        QString colour = QStringLiteral("#a9b4c2");  // not reached yet
        if (i == current) {
            colour = accent;
        } else if (done) {
            colour = QStringLiteral("#5a6b80");
        }
        label->setStyleSheet(
            QStringLiteral("color:%1; font-size:8pt; font-weight:700; letter-spacing:1px;")
                .arg(colour));
    }
}

SetupWindow::Page SetupWindow::currentPage() const
{
    return m_pages ? static_cast<Page>(m_pages->currentIndex()) : PageLocation;
}

void SetupWindow::showPage(Page page)
{
    m_pages->setCurrentIndex(static_cast<int>(page));

    // Nothing on the option pages should be reachable once the copy has begun,
    // and nothing about it is cancellable half way — a page that hides its own
    // controls is more honest than one that greys them out and hopes.
    const bool options = page == PageLocation || page == PageSoftware;
    m_sizeLabel->setVisible(options);
    m_backButton->setVisible(page == PageSoftware);
    m_secondaryButton->setEnabled(page != PageProgress);
    m_primaryButton->setEnabled(page != PageProgress);

    switch (page) {
    case PageLocation:
        m_secondaryButton->setText("Cancel");
        m_primaryButton->setText(m_hasSoftwarePage ? "Next" : "Install");
        break;
    case PageSoftware:
        m_secondaryButton->setText("Cancel");
        m_primaryButton->setText("Install");
        break;
    case PageProgress:
        m_secondaryButton->setText("Cancel");
        break;
    case PageComplete:
        m_secondaryButton->setText("Close");
        // The primary button's text belongs to whoever finished the install:
        // "Launch" on success, "Try again" on failure.
        break;
    }

    updateStepper();
}

void SetupWindow::goBack()
{
    if (currentPage() == PageSoftware) {
        showPage(PageLocation);
    }
}

void SetupWindow::onPrimaryClicked()
{
    switch (currentPage()) {
    case PageLocation:
        if (!validateLocation()) {
            return;
        }
        if (m_hasSoftwarePage) {
            showPage(PageSoftware);
            ensureModulesUi();
        } else {
            startInstall();
        }
        break;
    case PageSoftware:
        startInstall();
        break;
    case PageProgress:
        break;  // the button is disabled here
    case PageComplete:
        if (m_installFailed) {
            m_installFailed = false;
            showPage(PageLocation);
            return;
        }
        if (!m_launchPath.isEmpty()) {
            QProcess::startDetached(m_launchPath, {});
        }
        close();
        break;
    }
}

QWidget *SetupWindow::buildLocationPage()
{
    auto *page = new QWidget(this);
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *card = new QFrame(page);
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
    // Show the front of the path, not the tail. A default that opens scrolled to
    // "...\Programs\SHD Sim" hides the one part the user is checking.
    m_pathEdit->setCursorPosition(0);
    // The free-space warning is computed against this path, so it has to follow
    // the path being edited rather than only the checkboxes.
    connect(m_pathEdit, &QLineEdit::textChanged, this, &SetupWindow::refreshFooter);
    locRow->addWidget(m_pathEdit, 1);
    auto *browse = new QPushButton("Browse…", card);
    browse->setObjectName("ghost");
    connect(browse, &QPushButton::clicked, this, &SetupWindow::browseForFolder);
    locRow->addWidget(browse);
    cl->addLayout(locRow);

    auto *note = new QLabel(
        m_config.appName
            + " installs for your user account only — no administrator rights are needed.",
        card);
    note->setWordWrap(true);
    note->setStyleSheet("color:#5a6b80; font-size:11px;");
    cl->addWidget(note);

    cl->addSpacing(8);
    auto *optLabel = new QLabel("SHORTCUTS", card);
    optLabel->setObjectName("section");
    cl->addWidget(optLabel);

    m_desktopCheck = new QCheckBox("Create desktop shortcut(s)", card);
    m_desktopCheck->setChecked(m_config.desktopShortcut);
    cl->addWidget(m_desktopCheck);
    m_startMenuCheck = new QCheckBox("Create Start Menu shortcut(s)", card);
    m_startMenuCheck->setChecked(m_config.startMenuShortcut);
    cl->addWidget(m_startMenuCheck);
    cl->addStretch(1);

    outer->addWidget(card);
    return page;
}

QWidget *SetupWindow::buildSoftwarePage()
{
    auto *page = new QWidget(this);
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);

    // Scrolled, because the module list comes from a published manifest: how
    // many rows it has is not this installer's decision to make.
    auto *scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setStyleSheet("QScrollArea { border:0; background:transparent; }");

    auto *card = new QFrame(scroll);
    card->setObjectName("card");
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(20, 18, 20, 20);
    cl->setSpacing(12);

    // App checkboxes: one per configured app. When there is only a single app we
    // still show it, but a lone always-on app could also be hidden by config.
    if (!m_config.apps.isEmpty()) {
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

    // Physics modules, when this product downloads components. Filled in by
    // ensureModulesUi() from the manifest, not hard-coded — adding structural
    // analysis must be a publishing change, not an installer release.
    m_moduleHost = new QWidget(card);
    m_moduleLayout = new QVBoxLayout(m_moduleHost);
    m_moduleLayout->setContentsMargins(0, 0, 0, 0);
    m_moduleLayout->setSpacing(8);
    cl->addWidget(m_moduleHost);
    cl->addStretch(1);

    scroll->setWidget(card);
    outer->addWidget(scroll);
    return page;
}

QWidget *SetupWindow::buildProgressPage()
{
    auto *page = new QWidget(this);
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *card = new QFrame(page);
    card->setObjectName("card");
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(20, 18, 20, 20);
    cl->setSpacing(12);

    m_progressTitle = new QLabel("Installing " + m_config.appName + "…", card);
    m_progressTitle->setStyleSheet("color:#15314c; font-weight:700; font-size:11pt;");
    m_progressTitle->setWordWrap(true);
    cl->addWidget(m_progressTitle);

    m_progress = new QProgressBar(card);
    m_progress->setRange(0, 100);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    cl->addWidget(m_progress);

    m_status = new QLabel(card);
    m_status->setWordWrap(true);
    m_status->setStyleSheet("color:#5a6b80;");
    cl->addWidget(m_status);
    cl->addStretch(1);

    outer->addWidget(card);
    return page;
}

QWidget *SetupWindow::buildCompletePage()
{
    auto *page = new QWidget(this);
    auto *outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);

    auto *card = new QFrame(page);
    card->setObjectName("card");
    auto *cl = new QVBoxLayout(card);
    cl->setContentsMargins(20, 18, 20, 20);
    cl->setSpacing(10);

    m_completeTitle = new QLabel(card);
    m_completeTitle->setWordWrap(true);
    m_completeTitle->setStyleSheet("color:#15314c; font-weight:700; font-size:13pt;");
    cl->addWidget(m_completeTitle);

    m_completeBody = new QLabel(card);
    m_completeBody->setWordWrap(true);
    m_completeBody->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_completeBody->setStyleSheet("color:#5a6b80;");
    cl->addWidget(m_completeBody);

    // Where a partial outcome gets explained: the components that did not
    // arrive, and the fact that nothing needs reinstalling because of it.
    m_completeDetail = new QLabel(card);
    m_completeDetail->setWordWrap(true);
    m_completeDetail->hide();
    cl->addWidget(m_completeDetail);
    cl->addStretch(1);

    outer->addWidget(card);
    return page;
}

void SetupWindow::browseForFolder()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, "Choose install location", m_pathEdit->text());
    if (!dir.isEmpty()) {
        m_pathEdit->setText(QDir::toNativeSeparators(QDir(dir).filePath(m_config.appName)));
    }
}

// ---------------------------------------------------------------------------
// Component selection and fetching
//
// The application is embedded in this installer; solver backends are not. The
// user picks which physics they want, the sizes are shown before they are
// asked, and the fetch happens DURING the install while they are already
// waiting — rather than later, in the middle of their first simulation.
// (SHD-Sim-CFD ADR-0013.)
// ---------------------------------------------------------------------------

namespace {

// "fluids" -> "Fluids". Only a fallback: a manifest may carry a displayName,
// and that always wins. This exists so a newly published module is legible
// immediately rather than needing an installer change to get a capital letter.
QString prettyModuleName(const QString &id)
{
    if (id.isEmpty()) return id;
    QString out = id;
    out[0] = out[0].toUpper();
    return out.replace(QLatin1Char('-'), QLatin1Char(' '));
}

}  // namespace

bool SetupWindow::loadModules(QString *whyNot)
{
    if (!m_config.download.enabled) return false;

    // An offline build embeds every backend in the payload. It still has a
    // manifest — so the components are known and recorded — but there is
    // nothing to fetch, and no page to show.
    const QString embeddedDir = QDir(m_sourceDir).filePath(QStringLiteral("components"));

    shdkit::ComponentFetcher fetcher(m_config.download.baseUrl, this);

    if (m_config.download.publicKey.isEmpty()) {
        // Not fatal — a product may use the kit without a signing key — but it
        // must not be quiet. An installer that writes fetched executables and
        // cannot say who signed them is a supply-chain surface with no lid.
        qWarning("No download.publicKey configured: the release manifest will NOT be "
                 "verified. Set one before shipping.");
    }

    QString error;
    if (!fetcher.fetchManifest(m_config.download.manifestKey, m_config.download.publicKey,
                               &m_manifest, &error)) {
        if (whyNot) *whyNot = error;
        return false;
    }

    // Asked once per install rather than per module: it runs wsl.exe, and the
    // answer is a property of the machine rather than of any one backend.
    m_hostFacts = shdkit::VirtualisationFacts::gather();

    for (const QString &id : m_manifest.modules()) {
        ModuleEntry entry;
        entry.id = id;
        entry.label = prettyModuleName(id);

        for (const shdkit::Component &c : m_manifest.components) {
            if (!c.modules.contains(id)) continue;
            if (!c.fetched) continue;

            // Already in the payload? Then this is the offline variant and the
            // component is a fact, not a choice.
            if (QFileInfo::exists(QDir(embeddedDir).filePath(
                    c.objectKey.section(QLatin1Char('/'), -1)))) {
                entry.embedded = true;
            }
            entry.components.append(c);
            if (entry.description.isEmpty()) entry.description = c.description;
        }

        if (entry.components.isEmpty()) continue;
        // The first module in the manifest is on by default: somebody
        // installing a CFD product almost certainly wants the CFD solver, and
        // an installer whose every box is unticked invites a user to sail past
        // the page and end up with nothing.
        entry.defaultOn = m_modules.isEmpty();
        m_modules.append(entry);
    }

    return !m_modules.isEmpty();
}

void SetupWindow::ensureModulesUi()
{
    if (m_modulesLoaded || !m_config.download.enabled) {
        return;
    }
    m_modulesLoaded = true;

    // Silent installs never see this page but still need the module defaults,
    // so they come through here too — just without the reassurance.
    QLabel *busy = nullptr;
    if (!m_silent) {
        busy = new QLabel(QStringLiteral("Checking for optional components…"), m_moduleHost);
        busy->setStyleSheet(QStringLiteral("color:#5a6b80;"));
        m_moduleLayout->addWidget(busy);
        // Paint the page before the blocking fetch, or the user clicks Next and
        // watches a frozen window for as long as their proxy feels like taking.
        QCoreApplication::processEvents();
    }

    buildModuleUi(m_moduleHost, m_moduleLayout);
    delete busy;  // removes itself from the layout
    refreshFooter();
}

void SetupWindow::buildModuleUi(QWidget *card, QVBoxLayout *layout)
{
    QString whyNot;
    if (!loadModules(&whyNot)) {
        if (!m_config.download.enabled) return;

        // ── The manifest could not be had ──────────────────────────────────
        // Not an error. The application is embedded and installs perfectly
        // well without a solver; what the user loses is the CHOICE, not the
        // product. Refusing to install because a website is down would be the
        // worse failure by a wide margin.
        auto *note = new QLabel(
            QStringLiteral(
                "Optional physics backends could not be listed — the download service "
                "is unreachable.\n\n%1 will install, and the backends can be added "
                "later from Settings.")
                .arg(m_config.appName),
            card);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:#8a6d3b;"));
        layout->addSpacing(6);
        layout->addWidget(note);
        if (!whyNot.isEmpty()) qWarning("%s", qPrintable(whyNot));
        return;
    }

    layout->addSpacing(6);
    auto *label = new QLabel(m_config.download.promptTitle, card);
    label->setObjectName(QStringLiteral("section"));
    layout->addWidget(label);

    if (!m_config.download.promptHint.isEmpty()) {
        auto *hint = new QLabel(m_config.download.promptHint, card);
        hint->setWordWrap(true);
        hint->setStyleSheet(QStringLiteral("color:#5a6b80; font-size:11px;"));
        layout->addWidget(hint);
    }

    for (ModuleEntry &entry : m_modules) {
        // The size is on the checkbox, not in a tooltip. Asking somebody to
        // choose a 624 MB download without telling them it is 624 MB is not
        // asking a fair question.
        const QString sizeText = entry.embedded
                                     ? QStringLiteral("included")
                                     : shdkit::formatSize(entry.downloadBytes());
        QString text = QStringLiteral("%1  —  %2").arg(entry.label, sizeText);
        if (!entry.description.isEmpty()) {
            text = QStringLiteral("%1  —  %2  (%3)")
                       .arg(entry.label, sizeText, entry.description);
        }

        entry.check = new QCheckBox(text, card);
        entry.check->setChecked(entry.defaultOn || entry.embedded);
        if (entry.embedded) {
            // Nothing to download and nothing to decide: it is already here.
            entry.check->setEnabled(false);
        }
        layout->addWidget(entry.check);
        connect(entry.check, &QCheckBox::toggled, this, &SetupWindow::refreshFooter);

        // ── Can this machine actually run it? ──────────────────────────────
        // Only asked of modules that need a container. Offering a gigabyte to a
        // machine whose firmware has virtualisation switched off spends
        // somebody's bandwidth and tells them at the end; the selection page is
        // the last moment it is cheap to know.
        //
        // A negative answer UNTICKS the box and says why. It never hides it:
        // selection is a bandwidth choice, and somebody about to walk into
        // their BIOS must still be able to take the bytes now.
        if (!entry.needsVirtualisation() || entry.embedded) continue;

        const shdkit::VirtualisationAssessment capability =
            shdkit::VirtualisationAssessment::of(m_hostFacts);

        if (capability.verdict == shdkit::VirtualisationVerdict::Ready) continue;

        if (!capability.offerByDefault) entry.check->setChecked(false);

        auto *note = new QLabel(capability.summary + QStringLiteral("  ") + capability.remedy,
                                card);
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:#8a5a00; font-size:11px; "
                                           "margin-left:26px; margin-bottom:2px;"));
        layout->addWidget(note);

        if (capability.fixableWithElevation) {
            auto *fix = new QPushButton(tr("Enable it now"), card);
            fix->setCursor(Qt::PointingHandCursor);
            layout->addWidget(fix, 0, Qt::AlignLeft);
            connect(fix, &QPushButton::clicked, this, [this, fix, note]() {
                bool restart = false;
                QString error;
                fix->setEnabled(false);
                if (shdkit::enableVirtualisationFeatures(
                        shdkit::VirtualisationAssessment::of(m_hostFacts), &restart, &error)) {
                    note->setText(tr("Windows virtualisation features are enabled. "
                                     "Restart when the install has finished."));
                    note->setStyleSheet(QStringLiteral("color:#1f7a3d; font-size:11px; "
                                                       "margin-left:26px;"));
                    fix->hide();
                    // Re-read: the person may have been told to restart, but
                    // the facts on disk have changed either way and the next
                    // page should not still be describing the old ones.
                    m_hostFacts = shdkit::VirtualisationFacts::gather();
                } else {
                    note->setText(error);
                    fix->setEnabled(true);
                }
            });
        }
    }

    m_downloadSummary = new QLabel(card);
    m_downloadSummary->setWordWrap(true);
    m_downloadSummary->setStyleSheet(QStringLiteral("color:#5a6b80; font-size:11px;"));
    layout->addWidget(m_downloadSummary);
}

QList<shdkit::Component> SetupWindow::selectedComponents() const
{
    QList<shdkit::Component> out;
    QStringList seen;
    for (const ModuleEntry &entry : m_modules) {
        if (entry.check && !entry.check->isChecked()) continue;
        if (entry.embedded) continue;  // already in the payload
        for (const shdkit::Component &c : entry.components) {
            // One backend can serve several modules — CalculiX covers
            // structural AND thermal — so ticking both must not download it
            // twice.
            const QString key = c.objectKey;
            if (seen.contains(key)) continue;
            seen.append(key);
            out.append(c);
        }
    }
    return out;
}

void SetupWindow::refreshFooter()
{
    if (!m_sizeLabel) {
        return;  // maintenance and uninstall have no options to summarise
    }

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
    const QList<shdkit::Component> selected = selectedComponents();
    qint64 downloadBytes = 0;
    for (const shdkit::Component &c : selected) downloadBytes += c.size;

    const double mb = bytes / (1024.0 * 1024.0);
    m_sizeLabel->setText(QString("Requires about %1 MB of disk space   ·   Version %2")
                             .arg(QString::number(mb + downloadBytes / (1024.0 * 1024.0), 'f', 0),
                                  m_config.version));

    if (!m_downloadSummary) return;

    if (selected.isEmpty()) {
        m_downloadSummary->setText(
            QStringLiteral("Nothing to download. Backends can be added later from Settings."));
        return;
    }

    // ── Disk space, checked BEFORE the user commits ────────────────────────
    // Running out at 90% of a 624 MB download leaves a mess they have to clean
    // up by hand, and it is entirely predictable beforehand.
    const QString target =
        QDir::cleanPath(QDir::fromNativeSeparators(m_pathEdit->text().trimmed()));
    const qint64 needed = bytes + shdkit::ComponentFetcher::spaceNeededFor(selected);
    const qint64 free = shdkit::ComponentFetcher::freeSpaceFor(target);

    QString text = QStringLiteral("%1 to download across %2 component%3.")
                       .arg(shdkit::formatSize(downloadBytes))
                       .arg(selected.size())
                       .arg(selected.size() == 1 ? QString() : QStringLiteral("s"));

    if (free >= 0 && free < needed) {
        text += QStringLiteral("\n⚠ Needs about %1 free including unpacking; this drive has %2.")
                    .arg(shdkit::formatSize(needed), shdkit::formatSize(free));
        m_downloadSummary->setStyleSheet(QStringLiteral("color:#a5442f; font-size:11px;"));
    } else {
        m_downloadSummary->setStyleSheet(QStringLiteral("color:#5a6b80; font-size:11px;"));
    }
    m_downloadSummary->setText(text);
}

QList<shdkit::Component> SetupWindow::fetchSelectedComponents(const QString &targetDir)
{
    return fetchComponents(targetDir, selectedComponents());
}

QList<shdkit::Component> SetupWindow::fetchComponents(
    const QString &targetDir, const QList<shdkit::Component> &selected,
    const QList<shdkit::Component> &alreadyInstalled)
{
    QList<shdkit::Component> failed;
    m_componentFailures.clear();
    if (selected.isEmpty()) {
        writeBackendState(targetDir, alreadyInstalled, {});
        return failed;
    }

    // Downloads land in the install tree, not %TEMP%. Two reasons: a resumed
    // download survives a reboot that clears TEMP, and the archive is on the
    // same volume as its destination, so unpacking cannot fail for space
    // reasons that the pre-check said were fine.
    const QString cacheDir = QDir(targetDir).filePath(QStringLiteral("downloads"));

    shdkit::ComponentFetcher fetcher(m_config.download.baseUrl, this);
    connect(&fetcher, &shdkit::ComponentFetcher::progress, this,
            [this](const QString &, int percent, const QString &detail) {
                m_progress->setValue(percent);
                m_status->setText(detail);
                QCoreApplication::processEvents();
            });
    connect(&fetcher, &shdkit::ComponentFetcher::message, this, [this](const QString &text) {
        m_status->setText(text);
        QCoreApplication::processEvents();
    });

    QList<shdkit::Component> installed = alreadyInstalled;

    for (const shdkit::Component &component : selected) {
        m_progress->setValue(0);
        m_status->setText(QStringLiteral("Downloading %1…").arg(component.label()));
        QCoreApplication::processEvents();

        // Not `result`: QDialog already has a result() and the shadowing is
        // silent until it isn't.
        const shdkit::FetchResult fetched = fetcher.fetch(component, cacheDir);
        if (!fetched.ok) {
            qWarning("%s", qPrintable(fetched.error));
            m_componentFailures.append(component.label() + QStringLiteral(" — ") + fetched.error);
            failed.append(component);
            continue;
        }

        m_status->setText(QStringLiteral("Unpacking %1…").arg(component.label()));
        QCoreApplication::processEvents();

        QString error;
        if (!unpackComponent(component, fetched.path, targetDir, &error)) {
            qWarning("%s", qPrintable(error));
            m_componentFailures.append(component.label() + QStringLiteral(" — ") + error);
            failed.append(component);
            continue;
        }

        // The archive is only removed once it has unpacked. Deleting it earlier
        // would turn a failed extraction into a second full download.
        QFile::remove(fetched.path);
        installed.append(component);
    }

    QDir(cacheDir).removeRecursively();
    writeBackendState(targetDir, installed, failed);
    return failed;
}

bool SetupWindow::unpackComponent(const shdkit::Component &component,
                                  const QString &archivePath,
                                  const QString &targetDir,
                                  QString *error)
{
    // Into a sibling, then renamed. The old shape — delete the destination,
    // then spend two minutes writing 949 files into it — leaves a half-built
    // backend behind if anything interrupts it, and a half-built backend is
    // indistinguishable from a deliberate UI-only install: the app falls back
    // to preview mode and says nothing. The rename is the only moment the
    // destination changes.
    const QString finalDir = QDir(targetDir).filePath(component.name);
    const QString incoming = finalDir + QStringLiteral(".incoming");

    QDir(incoming).removeRecursively();
    if (!QDir().mkpath(incoming)) {
        *error = QStringLiteral("Could not create %1.").arg(QDir::toNativeSeparators(incoming));
        return false;
    }

    // Windows 10+ ships bsdtar as tar.exe and it reads zip as well as tar.gz —
    // the same tool the bootstrap stub already relies on, so this adds no
    // dependency. Every Linux has GNU tar on PATH. bsdtar returns non-zero on
    // harmless root-entry warnings, so success is judged by the result rather
    // than the exit code.
    //
    // Resolved rather than named: hardcoding "tar.exe" made every backend
    // download fail on Linux with "Could not run tar.exe", after the install
    // had otherwise succeeded — so the app was installed and permanently unable
    // to fetch a solver.
    const QString tarProgram = platform::executableName(QStringLiteral("tar"));
    QProcess tar;
    tar.setWorkingDirectory(incoming);
    tar.start(tarProgram,
              {QStringLiteral("-xf"), QDir::toNativeSeparators(archivePath),
               QStringLiteral("-C"), QDir::toNativeSeparators(incoming)});
    if (!tar.waitForStarted(15000)) {
        *error = QStringLiteral("Could not run %1 to unpack %2.")
                     .arg(tarProgram, component.label());
        QDir(incoming).removeRecursively();
        return false;
    }
    // 624 MB across 949 files takes a while on a slow disk; the progress bar is
    // deliberately indeterminate here rather than lying about a percentage.
    while (!tar.waitForFinished(200)) {
        QCoreApplication::processEvents();
    }

    // ── The probe MUST be destroyed before the rename below ────────────────
    // QDirIterator opens a Win32 search handle on the first hasNext() and holds
    // it until it is destroyed. Windows refuses to rename a directory while a
    // search handle inside it is live, so leaving the iterator at function scope
    // made the rename fail *every time* with access denied - after a perfectly
    // good 183 MB download and a perfectly good extraction.
    //
    // That is what shipped in 0.1.1: both backends downloaded, both unpacked,
    // and both were then reported as "could not be downloaded", which is the one
    // thing that had not gone wrong. Measured directly - renaming the same
    // directory succeeds with no handle open, fails with ERROR_ACCESS_DENIED
    // while one is, and succeeds again once it is closed.
    bool unpackedNothing = true;
    {
        QDirIterator probe(incoming, QDir::Files, QDirIterator::Subdirectories);
        unpackedNothing = !probe.hasNext();
    }
    if (unpackedNothing) {
        *error = QStringLiteral("%1 unpacked to nothing.").arg(component.label());
        QDir(incoming).removeRecursively();
        return false;
    }

    QDir(finalDir).removeRecursively();
    if (!QDir().rename(incoming, finalDir)) {
        *error = QStringLiteral("Could not move %1 into place.").arg(component.label());
        QDir(incoming).removeRecursively();
        return false;
    }
    return true;
}

void SetupWindow::writeBackendState(const QString &targetDir,
                                    const QList<shdkit::Component> &installed,
                                    const QList<shdkit::Component> &failed) const
{
    // ── The file that keeps a failed download from being a dead end ────────
    // The application reads this to know what it has, by VERSION rather than by
    // path, and to offer a retry for what is missing. A missing backend never
    // hides an analysis type — it offers the download.
    QJsonArray installedArray;
    for (const shdkit::Component &c : installed) {
        installedArray.append(QJsonObject{
            {QStringLiteral("name"), c.name},
            {QStringLiteral("version"), c.version},
            {QStringLiteral("sha256"), c.sha256},
            {QStringLiteral("modules"), QJsonArray::fromStringList(c.modules)},
        });
    }

    QJsonArray missingArray;
    for (const shdkit::Component &c : failed) {
        missingArray.append(QJsonObject{
            {QStringLiteral("name"), c.name},
            {QStringLiteral("version"), c.version},
            {QStringLiteral("objectKey"), c.objectKey},
            {QStringLiteral("size"), double(c.size)},
            {QStringLiteral("modules"), QJsonArray::fromStringList(c.modules)},
        });
    }

    const QJsonObject state{
        {QStringLiteral("product"), m_manifest.product},
        {QStringLiteral("version"), m_config.version},
        {QStringLiteral("channel"), m_manifest.channel},
        {QStringLiteral("baseUrl"), m_config.download.baseUrl},
        {QStringLiteral("manifestKey"), m_config.download.manifestKey},
        {QStringLiteral("installed"), installedArray},
        {QStringLiteral("missing"), missingArray},
    };

    QFile file(QDir(targetDir).filePath(QStringLiteral("components.json")));
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        file.write(QJsonDocument(state).toJson(QJsonDocument::Indented));
    }
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
            if (!m_silent) {
                QMessageBox::critical(this, m_config.appName + " Setup", errorMessage);
            }
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

void SetupWindow::optionError(const QString &message, int silentCode, Page page)
{
    if (m_silent) {
        failSilent(silentCode, message);
        return;
    }
    showPage(page);
    QMessageBox::warning(this, m_config.appName + " Setup", message);
}

bool SetupWindow::validateLocation(QString *targetOut)
{
    const QString targetDir =
        QDir::cleanPath(QDir::fromNativeSeparators(m_pathEdit->text().trimmed()));
    if (targetDir.isEmpty()) {
        optionError("Please choose an install location.", SetupExitInvalidArguments, PageLocation);
        return false;
    }
    if (QDir(targetDir) == QDir(m_sourceDir)) {
        optionError("Please choose a different folder from the installer's own location.",
                    SetupExitInvalidArguments, PageLocation);
        return false;
    }
    if (targetOut) {
        *targetOut = targetDir;
    }
    return true;
}

void SetupWindow::showInstallFailure(const QString &message)
{
    m_installFailed = true;
    m_launchPath.clear();
    m_completeTitle->setText("Setup did not finish");
    m_completeBody->setText(message);
    m_completeDetail->hide();
    showPage(PageComplete);
    m_primaryButton->setText("Try again");
}

void SetupWindow::startInstall()
{
    // A silent install never visits the software page, and an install with no
    // software page never builds one — either way the module defaults have to
    // exist before anything is selected from them.
    ensureModulesUi();

    QString targetDir;
    if (!validateLocation(&targetDir)) {
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
        optionError("Select at least one app to install.", SetupExitInvalidArguments, PageSoftware);
        return;
    }

    m_installFailed = false;
    m_progress->setValue(0);
    m_progressTitle->setText("Installing " + m_config.appName + "…");
    m_status->setText("Preparing…");
    showPage(PageProgress);
    QCoreApplication::processEvents();

    if (!QDir().mkpath(targetDir)) {
        if (m_silent) { failSilent(SetupExitFailed, "Could not create install folder."); return; }
        showInstallFailure("Could not create:\n" + QDir::toNativeSeparators(targetDir));
        return;
    }

    if (!closeRunningInstalledApps(targetDir)) {
        if (m_silent) { failSilent(SetupExitCancelled, "Install cancelled because running apps did not close."); return; }
        showInstallFailure("Install cancelled — " + m_config.appName
                           + " is still running. Close it and try again.");
        return;
    }

    const int total = countPayloadFiles();
    int done = 0;
    if (!copyPayload(targetDir, done, total)) {
        if (m_silent) { failSilent(SetupExitFailed, "Install failed while copying payload files."); return; }
        showInstallFailure("The files could not be copied. Close any running app windows and "
                           "try again.");
        return;
    }

    // Drop a copy of ourselves as the uninstaller.
    const QString uninstPath = QDir(targetDir).filePath(platform::executableName("uninstall"));
    QString uninstError;
    if (!copyPayloadFile(QCoreApplication::applicationFilePath(), uninstPath, &uninstError)) {
        if (m_silent) { failSilent(SetupExitFailed, uninstError); return; }
        showInstallFailure(uninstError);
        return;
    }

    // ── Components, AFTER the application is on disk and working ───────────
    // Order matters and is the whole safety property: by this point the install
    // is complete and launchable. Anything below can fail without taking the
    // product with it.
    //
    // **A failed download must still leave a working application.** The app is
    // embedded; the backends are not. Network dies here and the user gets a
    // working install that says "Fluids backend not installed — retry" in
    // Settings. Never a rollback — the one thing worse than an install that
    // fetched nothing is an install that undid itself over a solver.
    QList<shdkit::Component> failedComponents;
    if (m_config.download.enabled) {
        failedComponents = fetchSelectedComponents(targetDir);
    }

    // What could not be downloaded is reported on the finish page, not in a
    // modal over it: it is part of the outcome, and the outcome is a page now.
    finishInstall(targetDir, failedComponents);
}

void SetupWindow::finishInstall(const QString &targetDir,
                                const QList<shdkit::Component> &failed)
{
    QString launchPath; // first installed app
    for (const AppEntry &app : m_config.apps) {
        if (!wantExe(app.exe)) continue;
        const QString exePath = QDir(targetDir).filePath(app.exe);
        if (m_desktopCheck->isChecked()) {
            const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
            createShortcut(QDir(desktop).filePath(platform::shortcutFileName(app.name)), exePath, QString(), targetDir, app.name);
        }
        if (m_startMenuCheck->isChecked()) {
            const QString smDir = startMenuDir();
            QDir().mkpath(smDir);
            createShortcut(QDir(smDir).filePath(platform::shortcutFileName(app.name)), exePath, QString(), targetDir, app.name);
        }
        if (launchPath.isEmpty()) {
            launchPath = exePath;
        }
    }

    if (m_startMenuCheck->isChecked()) {
        const QString smDir = startMenuDir();
        QDir().mkpath(smDir);
        createShortcut(QDir(smDir).filePath(platform::shortcutFileName("Uninstall " + m_config.appName)),
                       QDir(targetDir).filePath(platform::executableName("uninstall")), "--uninstall", targetDir,
                       "Uninstall " + m_config.appName);
    }

    // Estimated size (KB) for Add/Remove Programs.
    qint64 bytes = 0;
    QDirIterator it(targetDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); bytes += it.fileInfo().size(); }
    writeUninstallInfo(targetDir, static_cast<int>(bytes / 1024));

    // Ask the shell to refresh icons so the new app/shortcut icon shows without
    // waiting for the Windows icon cache to expire.
    platform::notifyApplicationsChanged(startMenuDir());

    m_progress->setValue(100);
    m_status->setText(m_config.appName + " has been installed.");

    // Anonymous, identifier-free, and inert unless a product configured it.
    //
    // Here rather than at the start of the install, because "how many people
    // installed this" should not count the ones where it failed halfway and
    // left them with nothing. Blocking for at most three seconds, on a page
    // that has already finished — see src/telemetry.h for why it cannot be
    // fired and forgotten from here.
    shdkit::reportInstallRun(m_config.telemetry, QStringLiteral("install"),
                             m_config.version);

    m_installFailed = false;
    m_launchPath = launchPath;
    m_completeTitle->setText(m_config.appName + " is installed");

    QStringList lines;
    lines << "Installed to " + QDir::toNativeSeparators(targetDir);
    if (m_desktopCheck->isChecked() || m_startMenuCheck->isChecked()) {
        QStringList where;
        if (m_desktopCheck->isChecked())   where << "the desktop";
        if (m_startMenuCheck->isChecked()) where << "the Start Menu";
        lines << "Shortcuts added to " + where.join(" and ") + ".";
    }
    m_completeBody->setText(lines.join('\n'));

    if (failed.isEmpty()) {
        m_completeDetail->hide();
    } else {
        // The reason, not just the name. "Could not be downloaded" was also
        // simply wrong for anything that failed while unpacking, which is the
        // half of this step most likely to break on a particular machine.
        QStringList detail = m_componentFailures;
        if (detail.isEmpty()) {
            for (const shdkit::Component &c : failed) detail.append(c.label());
        }
        m_completeDetail->setStyleSheet(QStringLiteral("color:#8a6d3b; font-size:11px;"));
        m_completeDetail->setText(
            QStringLiteral(
                "These optional components could not be installed:\n  %1\n\n"
                "Everything else works. You can retry from Settings inside the "
                "application whenever you are ready — nothing needs reinstalling.")
                .arg(detail.join(QStringLiteral("\n  "))));
        m_completeDetail->show();
    }

    showPage(PageComplete);
    m_primaryButton->setText("Launch");
    m_primaryButton->setEnabled(!m_launchPath.isEmpty());
    finishSilent(SetupExitSuccess);
}

// ---------------------------------------------------------------------------
// Maintenance UI / flow (shown when an install already exists)
// ---------------------------------------------------------------------------
void SetupWindow::buildMaintenanceUi()
{
    // Taller than the choice page needs, because the change page shares the
    // window and the window must not resize under the user — the same reasoning
    // as buildInstallUi, which sizes for its longest page and lets that page
    // scroll.
    setFixedSize(520, 470);
    auto *root = makeFrame(false);

    m_maintPages = new QStackedWidget(this);
    m_maintPages->addWidget(buildMaintChoicePage());   // MaintChoice
    m_maintPages->addWidget(buildMaintChangePage());   // MaintChange
    root->addWidget(m_maintPages, 1);

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
    connect(m_secondaryButton, &QPushButton::clicked, this, &SetupWindow::onMaintSecondary);
    btnRow->addWidget(m_secondaryButton);
    btnRow->addStretch(1);

    // Only offered when there is a download source to change anything from. An
    // all-embedded build of this kit has no components to add and no manifest
    // to list them, and a button that opens an empty page is worse than none.
    if (m_config.download.enabled) {
        m_changeButton = new QPushButton("Change", this);
        m_changeButton->setObjectName("ghost");
        m_changeButton->setCursor(Qt::PointingHandCursor);
        connect(m_changeButton, &QPushButton::clicked, this,
                [this] { showMaintPage(MaintChange); });
        btnRow->addWidget(m_changeButton);
    }

    m_primaryButton = new QPushButton(this);
    m_primaryButton->setCursor(Qt::PointingHandCursor);
    m_primaryButton->setDefault(true);
    connect(m_primaryButton, &QPushButton::clicked, this, &SetupWindow::onMaintPrimary);
    btnRow->addWidget(m_primaryButton);
    root->addLayout(btnRow);

    showMaintPage(MaintChoice);

#ifndef SHD_WHITELABEL
    // Attribution Notice required under AGPLv3 §7(b); see ATTRIBUTION.md. It must
    // stay visible under the free (AGPL) licence. Removal / white-label is only
    // permitted under a commercial licence — build with -DSHD_WHITELABEL.
    // TODO: hyperlink to SHD Systems' official URL once finalised.
    auto *attribution = new QLabel(
        "Powered by SHD Systems  ·  © 2026 SHD Systems Ltd", this);
    attribution->setStyleSheet("color:#9aa7b8; font-size:11px;");
    attribution->setWordWrap(true);
    root->addWidget(attribution);
#endif
}

bool SetupWindow::closeRunningInstalledApps(const QString &targetDir)
{
    const QStringList appExes = m_config.appExeNames();
    const QList<RunningAppProcess> running = runningInstalledAppProcesses(targetDir, appExes);
    if (running.isEmpty()) {
        return true;
    }

    if (m_silent) {
        if (m_status) {
            m_status->setText("Closing " + m_config.appName + "...");
        }
        QCoreApplication::processEvents();
        platform::requestProcessesClose(running);
        return waitForInstalledAppsToExit(targetDir, appExes, 30000);
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

    platform::requestProcessesClose(running);
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
    if (m_silent && m_requestedAction == SetupAction::Update && !update) {
        failSilent(SetupExitAlreadyCurrent, m_config.appName + " is already current.");
        return;
    }
    const QString target = m_installedDir;
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    bool desktopExisted = false;
    for (const AppEntry &app : m_config.apps) {
        if (QFile::exists(QDir(desktop).filePath(platform::shortcutFileName(app.name)))) {
            desktopExisted = true;
            break;
        }
    }

    m_primaryButton->setEnabled(false);
    m_secondaryButton->setEnabled(false);
    if (m_changeButton) m_changeButton->setEnabled(false);
    m_progress->show();
    m_status->setText(update ? "Updating files…" : "Repairing files…");
    QCoreApplication::processEvents();

    if (!closeRunningInstalledApps(target)) {
        if (m_silent) { failSilent(SetupExitCancelled, "Update/repair cancelled because running apps did not close."); return; }
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
        if (m_silent) { failSilent(SetupExitFailed, "Update/repair failed while copying payload files."); return; }
        m_primaryButton->setEnabled(true);
        m_secondaryButton->setEnabled(true);
        m_status->setText(update
            ? "Update failed. Close any running app windows and try again."
            : "Repair failed. Close any running app windows and try again.");
        return;
    }

    const QString uninstPath = QDir(target).filePath(platform::executableName("uninstall"));
    QString uninstError;
    if (!copyPayloadFile(QCoreApplication::applicationFilePath(), uninstPath, &uninstError)) {
        if (m_silent) { failSilent(SetupExitFailed, uninstError); return; }
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
        createShortcut(QDir(smDir).filePath(platform::shortcutFileName(app.name)), exePath, QString(), target, app.name);
        if (desktopExisted) {
            createShortcut(QDir(desktop).filePath(platform::shortcutFileName(app.name)), exePath, QString(), target, app.name);
        }
        if (launchPath.isEmpty()) {
            launchPath = exePath;
        }
    }
    createShortcut(QDir(smDir).filePath(platform::shortcutFileName("Uninstall " + m_config.appName)), uninstPath, "--uninstall",
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
    finishSilent(SetupExitSuccess);
}

// ---------------------------------------------------------------------------
// Change: add or remove solver backends on an install that already exists
// ---------------------------------------------------------------------------
QStringList SetupWindow::installedComponentNames(const QString &dir) const
{
    QFile file(QDir(dir).filePath(QStringLiteral("components.json")));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();

    QStringList out;
    for (const QJsonValue &value : root.value(QStringLiteral("installed")).toArray()) {
        const QString name = value.toObject().value(QStringLiteral("name")).toString();
        if (!name.isEmpty()) out.append(name);
    }
    return out;
}

bool SetupWindow::removeComponent(const QString &targetDir, const QString &name,
                                  QString *error) const
{
    const QString dir = QDir(targetDir).filePath(name);
    if (!QFileInfo::exists(dir)) return true;   // already gone; nothing to undo

    if (!QDir(dir).removeRecursively()) {
        *error = QStringLiteral("Could not remove %1. It may be in use.")
                     .arg(QDir::toNativeSeparators(dir));
        return false;
    }
    return true;
}

QWidget *SetupWindow::buildMaintChoicePage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(12);

    const bool update = shouldUpdateToInstaller(m_installedVersion, m_config.version);

    auto *msg = new QLabel(m_config.appName + " is already installed on this computer.", page);
    msg->setWordWrap(true);
    msg->setStyleSheet("color:#15314c; font-weight:600; font-size:11pt;");
    layout->addWidget(msg);

    auto *info = new QLabel(page);
    info->setWordWrap(true);
    info->setStyleSheet("color:#5a6b80;");
    const QString ver = m_installedVersion.isEmpty() ? QString()
                                                     : QString("Installed version %1   ·   ").arg(m_installedVersion);
    info->setText(ver + QDir::toNativeSeparators(m_installedDir));
    layout->addWidget(info);

    auto *choose = new QLabel(page);
    choose->setWordWrap(true);
    choose->setStyleSheet("color:#5a6b80;");
    const QString changeClause = m_config.download.enabled
        ? QStringLiteral(", <b>Change</b> to add or remove analysis types")
        : QString();
    choose->setText(update
        ? QString("This installer is version %1. Choose <b>Update</b> to upgrade%2, or "
                  "<b>Uninstall</b> to remove it.").arg(m_config.version, changeClause)
        : QString("Choose <b>Repair</b> to reinstall the files%1, or <b>Uninstall</b> to "
                  "remove it.").arg(changeClause));
    layout->addWidget(choose);

    layout->addStretch(1);
    return page;
}

QWidget *SetupWindow::buildMaintChangePage()
{
    auto *page = new QWidget(this);
    auto *layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto *heading = new QLabel(QStringLiteral("Analysis types"), page);
    heading->setStyleSheet("color:#15314c; font-weight:600; font-size:11pt;");
    layout->addWidget(heading);

    m_changeHint = new QLabel(
        QStringLiteral("Tick to add, untick to remove. Removing one frees the disk space and "
                       "does not touch anything you have already solved."),
        page);
    m_changeHint->setWordWrap(true);
    m_changeHint->setStyleSheet("color:#5a6b80; font-size:11px;");
    layout->addWidget(m_changeHint);

    // Scrolls, for the same reason the software page does: the number of
    // analysis types is a publishing decision, and a fifth family must not push
    // the buttons off the bottom of a fixed window.
    auto *scroll = new QScrollArea(page);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *inner = new QWidget(scroll);
    m_changeList = new QVBoxLayout(inner);
    m_changeList->setContentsMargins(0, 0, 0, 0);
    m_changeList->setSpacing(6);
    m_changeList->addStretch(1);
    scroll->setWidget(inner);
    layout->addWidget(scroll, 1);

    return page;
}

bool SetupWindow::populateChangePage()
{
    if (m_changeLoaded) return true;

    // The manifest is what says which analysis types exist and what they cost.
    // The maintenance page never needed it, so this is usually the first fetch
    // of the run and it can fail — offline, or a signature that does not verify.
    m_status->setText(QStringLiteral("Checking what is available…"));
    QCoreApplication::processEvents();

    QString whyNot;
    if (m_manifest.isEmpty() && !loadModules(&whyNot)) {
        m_status->setText(whyNot.isEmpty()
                              ? QStringLiteral("Could not read the list of available backends.")
                              : whyNot);
        return false;
    }
    m_status->clear();

    const QStringList have = installedComponentNames(m_installedDir);

    m_changeChecks.clear();
    for (const ModuleEntry &entry : m_modules) {
        // Installed means every component behind it is here. Any one of them
        // absent and the analysis type cannot be run, so offering it as
        // "installed" would be a promise the install cannot keep.
        bool complete = !entry.components.isEmpty();
        for (const shdkit::Component &c : entry.components) {
            if (!have.contains(c.name)) { complete = false; break; }
        }

        const QString text =
            QStringLiteral("%1  —  %2")
                .arg(entry.label, complete ? QStringLiteral("installed")
                                           : shdkit::formatSize(entry.downloadBytes()));

        auto *check = new QCheckBox(text, m_changeList->parentWidget());
        check->setChecked(complete);
        check->setEnabled(!entry.embedded);
        // Before the trailing stretch, so the rows stay at the top.
        m_changeList->insertWidget(m_changeList->count() - 1, check);
        m_changeChecks.append({entry.id, check});
    }

    if (m_changeChecks.isEmpty()) {
        m_status->setText(QStringLiteral("This build has no downloadable backends."));
        return false;
    }

    m_changeLoaded = true;
    return true;
}

void SetupWindow::showMaintPage(MaintPage page)
{
    if (page == MaintChange && !populateChangePage()) {
        return;   // stay put; the status line says why
    }

    m_maintPages->setCurrentIndex(static_cast<int>(page));

    const bool update = shouldUpdateToInstaller(m_installedVersion, m_config.version);
    if (page == MaintChange) {
        m_primaryButton->setText(QStringLiteral("Apply"));
        m_secondaryButton->setText(QStringLiteral("Back"));
    } else {
        m_primaryButton->setText(update
            ? QString("Update to %1").arg(m_config.version.split('+').first())
            : QStringLiteral("Repair"));
        m_secondaryButton->setText(QStringLiteral("Uninstall"));
    }
    if (m_changeButton) m_changeButton->setVisible(page == MaintChoice);
}

void SetupWindow::onMaintPrimary()
{
    if (m_maintPages->currentIndex() == static_cast<int>(MaintChange)) {
        // Ticked types -> the components behind them, deduplicated: the material
        // library serves all four, and asking for two of them must not queue it
        // twice or, worse, list it as removable because one of the four is off.
        QList<shdkit::Component> wanted;
        QStringList seen;
        for (const auto &pair : m_changeChecks) {
            if (!pair.second->isChecked()) continue;
            for (const ModuleEntry &entry : m_modules) {
                if (entry.id != pair.first) continue;
                for (const shdkit::Component &c : entry.components) {
                    if (seen.contains(c.name)) continue;
                    seen.append(c.name);
                    wanted.append(c);
                }
            }
        }
        doChange(wanted);
        return;
    }
    doRepairOrUpdate();
}

void SetupWindow::onMaintSecondary()
{
    if (m_maintPages->currentIndex() == static_cast<int>(MaintChange)) {
        showMaintPage(MaintChoice);
        return;
    }

    const QString u = QDir(m_installedDir).filePath(platform::executableName("uninstall"));
    if (QFile::exists(u)) {
        QProcess::startDetached(u, {"--uninstall"});
    }
    close();
}

void SetupWindow::doChange(const QList<shdkit::Component> &wanted)
{
    const QStringList have = installedComponentNames(m_installedDir);

    QStringList wantedNames;
    for (const shdkit::Component &c : wanted) wantedNames.append(c.name);

    QList<shdkit::Component> toAdd;
    QList<shdkit::Component> keep;
    for (const shdkit::Component &c : wanted) {
        if (have.contains(c.name)) keep.append(c);
        else                       toAdd.append(c);
    }

    QStringList toRemove;
    for (const QString &name : have) {
        if (!wantedNames.contains(name)) toRemove.append(name);
    }

    if (toAdd.isEmpty() && toRemove.isEmpty()) {
        m_status->setText(QStringLiteral("Nothing to change."));
        return;
    }

    // A backend cannot be replaced or deleted while the application has its
    // executables open, and the same check the update path uses says so once
    // rather than failing file by file.
    if (!closeRunningInstalledApps(m_installedDir)) {
        m_status->setText(QStringLiteral("Cancelled. Close ") + m_config.appName
                          + QStringLiteral(" and try again."));
        return;
    }

    m_primaryButton->setEnabled(false);
    m_secondaryButton->setEnabled(false);
    if (m_changeButton) m_changeButton->setEnabled(false);
    m_progress->show();
    QCoreApplication::processEvents();

    // Removals first, and they are cheap: doing them before a download means a
    // machine swapping one backend for a larger one needs the difference in
    // free space rather than the sum.
    QStringList removeErrors;
    for (const QString &name : toRemove) {
        m_status->setText(QStringLiteral("Removing %1…").arg(name));
        QCoreApplication::processEvents();
        QString error;
        if (!removeComponent(m_installedDir, name, &error)) removeErrors.append(error);
    }

    // What survives, as components rather than names, so components.json can be
    // written whole. Anything removed is simply absent from it.
    QList<shdkit::Component> stillInstalled = keep;
    for (const QString &name : have) {
        if (toRemove.contains(name)) continue;
        if (wantedNames.contains(name)) continue;   // already in `keep`
        // Installed, still wanted, but not in the manifest any more. Keep the
        // directory and say nothing: a component withdrawn from publication is
        // not a reason to delete a working backend from someone's machine.
        shdkit::Component orphan;
        orphan.name = name;
        stillInstalled.append(orphan);
    }

    QList<shdkit::Component> failed;
    if (!toAdd.isEmpty()) {
        failed = fetchComponents(m_installedDir, toAdd, stillInstalled);
    } else {
        writeBackendState(m_installedDir, stillInstalled, {});
    }

    qint64 bytes = 0;
    QDirIterator it(m_installedDir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) { it.next(); bytes += it.fileInfo().size(); }
    writeUninstallInfo(m_installedDir, static_cast<int>(bytes / 1024));

    m_progress->setValue(100);

    QStringList said;
    if (!toAdd.isEmpty()) {
        said.append(QStringLiteral("%1 added").arg(toAdd.size() - failed.size()));
    }
    if (!toRemove.isEmpty()) {
        said.append(QStringLiteral("%1 removed").arg(toRemove.size() - removeErrors.size()));
    }
    m_status->setText(said.isEmpty() ? QStringLiteral("Nothing changed.")
                                     : said.join(QStringLiteral(", ")) + QStringLiteral("."));

    if (!failed.isEmpty() || !removeErrors.isEmpty()) {
        QMessageBox::warning(this, m_config.appName + " Setup",
                             (m_componentFailures + removeErrors).join(QStringLiteral("\n")));
    }

    m_primaryButton->setEnabled(true);
    m_secondaryButton->setEnabled(true);
    if (m_changeButton) m_changeButton->setEnabled(true);

    // Back to the choice page, with the outcome still on the status line. The
    // ticked boxes are now stale — every one of them says "installed" or a
    // download size that has just changed — so the page is rebuilt from
    // components.json the next time it is asked for rather than re-shown.
    const QString outcome = m_status->text();
    m_changeLoaded = false;
    m_changeChecks.clear();
    while (m_changeList->count() > 1) {
        QLayoutItem *item = m_changeList->takeAt(0);
        delete item->widget();
        delete item;
    }
    showMaintPage(MaintChoice);
    m_status->setText(outcome);
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

#ifndef SHD_WHITELABEL
    // Attribution Notice required under AGPLv3 §7(b); see ATTRIBUTION.md. It must
    // stay visible under the free (AGPL) licence. Removal / white-label is only
    // permitted under a commercial licence — build with -DSHD_WHITELABEL.
    // TODO: hyperlink to SHD Systems' official URL once finalised.
    auto *attribution = new QLabel(
        "Powered by SHD Systems  ·  © 2026 SHD Systems Ltd", this);
    attribution->setStyleSheet("color:#9aa7b8; font-size:11px;");
    attribution->setWordWrap(true);
    root->addWidget(attribution);
#endif
}

void SetupWindow::startUninstall()
{
    m_primaryButton->setEnabled(false);
    m_secondaryButton->setEnabled(false);
    m_progress->show();
    m_status->setText("Removing…");
    QCoreApplication::processEvents();

    // Reported BEFORE anything is removed, and before the self-deleting script
    // below is spawned. Once that script is running this process is on a clock
    // it does not control, and a request begun there would be killed with it.
    //
    // Uninstalls are the churn signal and the one number that cannot be
    // inferred from anything else: an install that stops reporting has either
    // been removed or is simply not being opened, and those are very different
    // problems.
    shdkit::reportInstallRun(m_config.telemetry, QStringLiteral("uninstall"),
                             m_config.version);

    // Remove shortcuts. `shortcutPaths()` now names the menu entries as well as
    // the desktop ones, individually.
    for (const QString &lnk : shortcutPaths()) {
        platform::removeShortcut(lnk);
    }

#ifdef Q_OS_WIN
    // The Start Menu folder is ours — created as Programs\<appName> — so
    // removing what is left of it is correct and tidies any stray entry.
    //
    // Deliberately NOT done on Linux, where the same call returns
    // ~/.local/share/applications: the directory the desktop environment keeps
    // every application's launcher in. Recursively deleting it would uninstall
    // this product and take the user's entire applications menu with it.
    QDir(startMenuDir()).removeRecursively();
#endif

    removeUninstallInfo();

    // The install folder holds this running uninstaller and the Qt libraries it
    // has mapped, so it cannot delete itself directly. Both platforms spawn a
    // helper that waits for THIS process to exit by pid and then removes the
    // folder; the helper is PowerShell or /bin/sh and the waiting is
    // Wait-Process or a kill(2) poll. See platform::scheduleSelfDelete.
    platform::scheduleSelfDelete(QCoreApplication::applicationDirPath());

    m_status->setText(m_config.appName + " has been removed.");
    finishSilent(SetupExitSuccess);
    QTimer::singleShot(1000, this, &QWidget::close);
}

// ---------------------------------------------------------------------------
// Win32 helpers
// ---------------------------------------------------------------------------
bool SetupWindow::createShortcut(const QString &linkPath, const QString &target, const QString &args,
                                 const QString &workingDir, const QString &description) const
{
    // Kept as a member so every call site reads the same as it did; the
    // mechanism moved to `platform` because a .lnk through COM and a .desktop
    // through QTextStream have nothing in common but their purpose.
    return platform::createShortcut(linkPath, target, args, workingDir, description);
}

void SetupWindow::writeUninstallInfo(const QString &targetDir, int sizeKb) const
{
    QSettings reg(m_config.uninstallRegPath(), installRecordFormat());
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
                 QString("\"%1\" --uninstall").arg(QDir::toNativeSeparators(QDir(targetDir).filePath(platform::executableName("uninstall")))));
    reg.setValue("EstimatedSize", sizeKb);
    reg.setValue("NoModify", 1);
    reg.setValue("NoRepair", 1);
}

void SetupWindow::removeUninstallInfo() const
{
    const QString path = m_config.uninstallRegPath();
    {
        QSettings reg(path, installRecordFormat());
        reg.clear();
        // Scoped so the destructor flushes before the file is removed below.
        // Without the scope, QSettings writes the (empty) file back out on
        // destruction and undoes the removal.
    }

    if (platform::installRecordIsRegistry()) return;

    // clear() empties the file but leaves it, and its parent directories, in
    // the user's data directory. An empty record is harmless — it reads back as
    // "not installed", which is true — but leaving litter behind after an
    // uninstall is exactly the thing a per-user installer promises not to do.
    //
    // Only the directories WE made, and only while they are empty: rmdir fails
    // on a non-empty directory, which is the desired outcome if another product
    // from the same publisher is still installed beside this one.
    QFile::remove(path);
    QDir dir = QFileInfo(path).absoluteDir();
    const QString appDir = dir.absolutePath();
    dir.cdUp();
    const QString publisherDir = dir.absolutePath();
    QDir().rmdir(appDir);
    QDir().rmdir(publisherDir);
}

QString SetupWindow::startMenuDir() const
{
    // A per-product folder under the Start Menu on Windows; on Linux the same
    // call returns the flat ~/.local/share/applications, because the
    // freedesktop menu groups by the Categories= line inside each entry and a
    // directory per vendor would be a folder nothing reads.
    //
    // That difference matters at ONE call site: the uninstaller does
    // `QDir(startMenuDir()).removeRecursively()`, which is right when the
    // directory belongs to us and catastrophic when it is the user's whole
    // applications directory. See uninstall(), which no longer does that.
    return platform::menuDir(m_config.appName);
}

QStringList SetupWindow::shortcutPaths() const
{
    const QString desktop = QStandardPaths::writableLocation(QStandardPaths::DesktopLocation);
    const QString menu = startMenuDir();
    QStringList paths;
    for (const AppEntry &app : m_config.apps) {
        paths << QDir(desktop).filePath(platform::shortcutFileName(app.name));
        // The menu entries are listed INDIVIDUALLY rather than left to a
        // directory removal. On Windows the Start Menu folder is ours and
        // deleting it wholesale was correct; on Linux `startMenuDir()` is the
        // user's entire ~/.local/share/applications, shared with every other
        // application they have installed. Naming each file is the only form of
        // this that is safe on both.
        paths << QDir(menu).filePath(platform::shortcutFileName(app.name));
    }
    paths << QDir(desktop).filePath(platform::shortcutFileName(m_config.appName)); // legacy single-app shortcut
    paths << QDir(menu).filePath(platform::shortcutFileName(m_config.appName));
    paths << QDir(menu).filePath(
        platform::shortcutFileName(QStringLiteral("Uninstall ") + m_config.appName));
    return paths;
}
