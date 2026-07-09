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
