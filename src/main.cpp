// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: (c) 2026 SHD Systems Ltd

#include "setupwindow.h"

#include <QApplication>
#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QIcon>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>

namespace {

// Load the embedded per-project configuration. The whole point of this kit is
// that everything product-specific lives here, not in the code.
bool loadConfig(InstallerConfig &config, QString *error)
{
    QFile file(QStringLiteral(":/setup/config.json"));
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("Bundled config.json is missing.");
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (doc.isNull() || !doc.isObject()) {
        if (error) *error = QStringLiteral("config.json is invalid: ") + parseError.errorString();
        return false;
    }

    const QJsonObject obj = doc.object();
    config.appName = obj.value("appName").toString();
    config.displayName = obj.value("displayName").toString(config.appName);
    config.subtitle = obj.value("subtitle").toString(QStringLiteral("SETUP"));
    config.publisher = obj.value("publisher").toString();
    config.version = obj.value("version").toString();
    config.registryKey = obj.value("registryKey").toString();
    config.accentColor = obj.value("accentColor").toString(QStringLiteral("#1CA3C2"));

    const QJsonObject options = obj.value("options").toObject();
    config.desktopShortcut = options.value("desktopShortcut").toBool(true);
    config.startMenuShortcut = options.value("startMenuShortcut").toBool(true);

    for (const QJsonValue &value : obj.value("apps").toArray()) {
        const QJsonObject appObj = value.toObject();
        AppEntry app;
        app.exe = appObj.value("exe").toString();
        app.name = appObj.value("name").toString(app.exe);
        app.description = appObj.value("description").toString();
        app.defaultOn = appObj.value("default").toBool(true);
        if (!app.exe.isEmpty()) {
            config.apps.append(app);
        }
    }

    // Reporting that an install happened. Absent -> nothing is sent and no
    // socket is opened, the same posture as the downloader below and for the
    // same reason: several products use this kit and none of them asked for a
    // counter. What it carries, and why it can carry it without asking, is in
    // src/telemetry.h.
    const QJsonObject telemetry = obj.value("telemetry").toObject();
    if (!telemetry.isEmpty()) {
        config.telemetry.enabled = telemetry.value("enabled").toBool(false);
        config.telemetry.endpoint = telemetry.value("endpoint").toString();
        config.telemetry.productCode = telemetry.value("productCode").toString();
        config.telemetry.channel = telemetry.value("channel").toString(QStringLiteral("stable"));

        // Enabled with nowhere to send is a configuration mistake, and a silent
        // one — the install would succeed and the number would stay at zero
        // forever. Refused at load, like the downloader's missing baseUrl.
        if (config.telemetry.enabled && config.telemetry.endpoint.isEmpty()) {
            if (error) {
                *error = QStringLiteral(
                    "config.json enables install reporting but sets no endpoint.");
            }
            return false;
        }
    }

    // Component downloading. Absent -> the kit behaves exactly as it always
    // has: everything embedded, no network, no selection page. Other SHD
    // products use this kit and none of them asked for a downloader, so this
    // stays opt-in rather than turning the embedded path into a special case.
    const QJsonObject download = obj.value("download").toObject();
    if (!download.isEmpty()) {
        config.download.enabled = download.value("enabled").toBool(false);
        config.download.baseUrl = download.value("baseUrl").toString();
        config.download.manifestKey = download.value("manifestKey").toString();
        config.download.publicKey = download.value("publicKey").toString();
        config.download.promptTitle =
            download.value("promptTitle").toString(QStringLiteral("OPTIONAL COMPONENTS"));
        config.download.promptHint = download.value("promptHint").toString();

        if (config.download.enabled &&
            (config.download.baseUrl.isEmpty() || config.download.manifestKey.isEmpty())) {
            if (error) {
                *error = QStringLiteral(
                    "config.json enables downloading but sets no baseUrl or manifestKey.");
            }
            return false;
        }
    }

    if (config.appName.isEmpty() || config.registryKey.isEmpty()) {
        if (error) *error = QStringLiteral("config.json must set appName and registryKey.");
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    InstallerConfig config;
    QString error;
    if (!loadConfig(config, &error)) {
        QMessageBox::critical(nullptr, QStringLiteral("Setup"), error);
        return 1;
    }

    QApplication::setApplicationName(config.appName + " Setup");
    QApplication::setOrganizationName(config.publisher);
    app.setWindowIcon(QIcon(QStringLiteral(":/setup/app.ico")));

    SetupAction action = SetupAction::Auto;
    bool silent = false;
    for (int i = 1; i < argc; ++i) {
        const QByteArray arg(argv[i]);
        if (arg == "--uninstall") {
            action = SetupAction::Uninstall;
        } else if (arg == "--install") {
            action = SetupAction::Install;
        } else if (arg == "--update") {
            action = SetupAction::Update;
        } else if (arg == "--repair") {
            action = SetupAction::Repair;
        } else if (arg == "--silent" || arg == "--quiet") {
            silent = true;
        } else if (arg == "--help" || arg == "/?") {
            QMessageBox::information(
                nullptr,
                QStringLiteral("Setup"),
                QStringLiteral("Supported arguments:\n"
                               "  --install\n"
                               "  --update\n"
                               "  --repair\n"
                               "  --uninstall\n"
                               "  --silent\n\n"
                               "Exit codes:\n"
                               "  0 success\n"
                               "  1 failed\n"
                               "  2 cancelled/running app did not close\n"
                               "  3 already current/already installed\n"
                               "  4 not installed\n"
                               "  5 invalid arguments"));
            return SetupExitSuccess;
        } else {
            return SetupExitInvalidArguments;
        }
    }
    // The installed copy is named uninstall.exe; double-clicking it (no args)
    // should uninstall, not run the installer.
    if (QFileInfo(QCoreApplication::applicationFilePath()).fileName()
            .compare("uninstall.exe", Qt::CaseInsensitive) == 0) {
        action = SetupAction::Uninstall;
    }

    SetupWindow window(config, action, silent);
    if (!silent) {
        window.show();
    }
    return app.exec();
}
