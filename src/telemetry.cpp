// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: (c) 2026 SHD Systems Ltd

#include "telemetry.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSysInfo>
#include <QTimer>
#include <QUrl>

namespace shdkit {

namespace {

// Two seconds, and the event loop below quits at three whatever the socket is
// doing. The uninstaller in particular calls this immediately before it starts
// deleting its own directory, so "wait a little longer just in case" is not a
// harmless default here.
constexpr int kTimeoutMs = 2000;
constexpr int kHardDeadlineMs = 3000;

QString osName()
{
    const QString kernel = QSysInfo::kernelType().toLower();
    if (kernel == QLatin1String("winnt")) return QStringLiteral("windows");
    if (kernel == QLatin1String("darwin")) return QStringLiteral("macos");
    if (kernel == QLatin1String("linux")) return QStringLiteral("linux");
    return QStringLiteral("other");
}

}  // namespace

void reportInstallRun(const TelemetryConfig &config,
                      const QString &kind,
                      const QString &version)
{
    if (!config.enabled || config.endpoint.isEmpty()) return;

    const QUrl url(config.endpoint);
    // https only. This posts from an elevated installer on somebody else's
    // network; a plaintext endpoint in a config file would be an invitation to
    // redirect it, and there is no deployment where it is the right answer.
    if (!url.isValid() || url.scheme().toLower() != QLatin1String("https")) return;

    QNetworkProxyFactory::setUseSystemConfiguration(true);

    QJsonObject body;
    body.insert(QStringLiteral("product"), config.productCode);
    body.insert(QStringLiteral("app_version"), version);
    body.insert(QStringLiteral("channel"), config.channel);
    body.insert(QStringLiteral("os"), osName());
    body.insert(QStringLiteral("kind"), kind);
    // Nothing else. Not a machine id, not a user name, not the install path -
    // see the header. Every field here is a property of the RELEASE, not of the
    // person or the computer.

    QNetworkAccessManager manager;
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("SHD Setup/%1").arg(version));
    request.setTransferTimeout(kTimeoutMs);
    // No redirects: a fixed endpoint we control, so following one could only
    // ever mean somebody in the middle has moved it.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QVariant::fromValue(QNetworkRequest::ManualRedirectPolicy));

    QNetworkReply *reply =
        manager.post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));

    /**
     * A local event loop with its own deadline.
     *
     * Blocking is deliberate rather than lazy: both call sites are about to
     * finish and tear down — one shows the completion page, the other spawns
     * the script that deletes this executable's directory — so a request left
     * in flight would be cancelled by the process ending, every time, and the
     * counter would read zero while looking perfectly implemented.
     *
     * The belt to that brace is the timer. `setTransferTimeout` does not cover
     * a connection that is accepted and then simply never answers, which is
     * exactly what a captive portal or a filtering proxy does.
     */
    QEventLoop loop;
    QTimer deadline;
    deadline.setSingleShot(true);
    QObject::connect(&deadline, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    deadline.start(kHardDeadlineMs);
    loop.exec();

    // The status is not examined and no error is shown. There is nothing the
    // person installing the software could do about it and nothing they would
    // want to know.
    reply->abort();
    reply->deleteLater();
}

}  // namespace shdkit
