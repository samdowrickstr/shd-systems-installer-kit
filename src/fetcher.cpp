// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: (c) 2026 SHD Systems Ltd

#include "fetcher.h"

#include "ed25519.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStorageInfo>
#include <QTimer>
#include <QUrl>

namespace shdkit {
namespace {

// Transport failures are worth retrying; a 404 is a wrong manifest and will
// still be wrong in four seconds. Retrying it just makes the user wait longer
// to be told the same thing.
constexpr int kMaxAttempts = 4;
constexpr int kBackoffMs[kMaxAttempts] = {0, 2000, 5000, 12000};

// Long enough that a slow corporate link is not mistaken for a dead one. This
// is the gap between BYTES, not the total transfer time — a 624 MB download on
// a bad connection is legitimately long and must not be killed for it.
constexpr int kStallTimeoutMs = 60000;

bool isRetryable(QNetworkReply::NetworkError error)
{
    switch (error) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyTimeoutError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::InternalServerError:
    case QNetworkReply::ServiceUnavailableError:
        return true;
    default:
        return false;
    }
}

QString describe(qint64 bytesPerSecond, qint64 remaining)
{
    if (bytesPerSecond <= 0) return QString();
    const qint64 seconds = remaining / bytesPerSecond;
    if (seconds > 90) {
        return QStringLiteral("%1 MB/s · about %2 min left")
            .arg(double(bytesPerSecond) / (1024.0 * 1024.0), 0, 'f', 1)
            .arg(seconds / 60);
    }
    return QStringLiteral("%1 MB/s · about %2 s left")
        .arg(double(bytesPerSecond) / (1024.0 * 1024.0), 0, 'f', 1)
        .arg(seconds);
}

}  // namespace

ComponentFetcher::ComponentFetcher(QString baseUrl, QObject *parent)
    : QObject(parent), m_baseUrl(std::move(baseUrl))
{
    // A managed corporate machine reaches the internet through a proxy
    // configured in the system, and Qt does NOT consult it unless asked. Without
    // this line the download times out on precisely the customers who paid.
    QNetworkProxyFactory::setUseSystemConfiguration(true);

    m_network = new QNetworkAccessManager(this);
    while (m_baseUrl.endsWith(QLatin1Char('/'))) m_baseUrl.chop(1);
}

ComponentFetcher::~ComponentFetcher() = default;

void ComponentFetcher::cancel() { m_cancelled = true; }

QString ComponentFetcher::urlFor(const QString &objectKey) const
{
    return m_baseUrl + QLatin1Char('/') + objectKey;
}

qint64 ComponentFetcher::freeSpaceFor(const QString &path)
{
    // The directory may not exist yet — walk up to one that does, or the check
    // reports -1 for every fresh install, which is every install.
    QDir dir(path);
    while (!dir.exists() && dir.cdUp()) { }
    if (!dir.exists()) return -1;

    const QStorageInfo info(dir.absolutePath());
    if (!info.isValid() || !info.isReady()) return -1;
    return info.bytesAvailable();
}

qint64 ComponentFetcher::spaceNeededFor(const QList<Component> &components)
{
    qint64 total = 0;
    for (const Component &c : components) {
        // The archive, plus what it unpacks to, because the archive is not
        // deleted until extraction succeeds. 2.2x is measured on the OpenFOAM
        // tree; erring high fails an install that would have worked, which is
        // the less damaging mistake of the two.
        total += c.size + qint64(double(c.size) * 2.2);
    }
    return total;
}

bool ComponentFetcher::fetchManifest(const QString &manifestKey,
                                     const QString &publicKeyBase64,
                                     Manifest *out,
                                     QString *error)
{
    QByteArray manifestBytes;
    QByteArray signatureBytes;

    const auto get = [&](const QString &key, QByteArray *into, QString *why) -> bool {
        for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
            if (m_cancelled) { *why = QStringLiteral("Cancelled."); return false; }
            if (kBackoffMs[attempt] > 0) {
                QEventLoop wait;
                QTimer::singleShot(kBackoffMs[attempt], &wait, &QEventLoop::quit);
                wait.exec();
            }

            QNetworkRequest request{QUrl(urlFor(key))};
            request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                                 QNetworkRequest::NoLessSafeRedirectPolicy);
            QNetworkReply *reply = m_network->get(request);

            QEventLoop loop;
            connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            loop.exec();

            const QNetworkReply::NetworkError err = reply->error();
            if (err == QNetworkReply::NoError) {
                *into = reply->readAll();
                reply->deleteLater();
                return true;
            }
            *why = reply->errorString();
            const bool retry = isRetryable(err);
            reply->deleteLater();
            if (!retry) return false;
        }
        return false;
    };

    QString why;
    if (!get(manifestKey, &manifestBytes, &why)) {
        if (error) {
            *error = QStringLiteral(
                         "Could not reach the download service to find out which solver "
                         "backends are available.\n\n%1")
                         .arg(why);
        }
        return false;
    }

    // A missing signature is NOT an error here — parseManifest decides, because
    // it is the thing that knows whether a key was configured. Fetching it
    // separately keeps that decision in one place.
    QString ignored;
    get(manifestKey + QStringLiteral(".sig"), &signatureBytes, &ignored);

    return parseManifest(manifestBytes, signatureBytes, publicKeyBase64, out, error);
}

FetchResult ComponentFetcher::fetch(const Component &component, const QString &destDir)
{
    FetchResult result;
    result.component = component;

    QDir().mkpath(destDir);

    // The object key's last segment is the filename. Taken from the key rather
    // than the component name so two versions of the same backend cannot
    // collide in the cache.
    const QString fileName = component.objectKey.section(QLatin1Char('/'), -1);
    const QString destPath = QDir(destDir).filePath(fileName);

    QString error;
    if (!download(component, destPath, &error)) {
        result.error = error;
        return result;
    }

    // ── Digest AFTER writing, before anything is unpacked ──────────────────
    // Streamed in 1 MB blocks: the payload is 624 MB and an installer that
    // holds one in memory to hash it fails on exactly the machines that can
    // least afford it.
    emit progress(component.name, 100, QStringLiteral("Checking %1…").arg(component.label()));

    QFile file(destPath);
    if (!file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("Downloaded %1 but could not read it back.").arg(fileName);
        return result;
    }

    Sha256 hash;
    QByteArray block;
    while (!(block = file.read(1024 * 1024)).isEmpty()) {
        hash.update(reinterpret_cast<const unsigned char *>(block.constData()),
                    static_cast<size_t>(block.size()));
        QCoreApplication::processEvents();
    }
    file.close();

    const QString digest = QString::fromStdString(hash.hexDigest());
    if (digest != component.sha256) {
        // Removed, not kept: a file that failed its digest must not be resumed
        // from on the next attempt, or the corruption becomes permanent and
        // every retry reproduces it.
        QFile::remove(destPath);
        result.error = QStringLiteral(
                           "%1 did not download correctly and has been discarded.\n\n"
                           "Expected %2…\nGot      %3…")
                           .arg(component.label(), component.sha256.left(16), digest.left(16));
        return result;
    }

    result.ok = true;
    result.path = destPath;
    return result;
}

bool ComponentFetcher::download(const Component &component, const QString &destPath, QString *error)
{
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (m_cancelled) {
            *error = QStringLiteral("Cancelled.");
            return false;
        }

        if (kBackoffMs[attempt] > 0) {
            emit message(QStringLiteral("Retrying %1 (attempt %2 of %3)…")
                             .arg(component.label())
                             .arg(attempt + 1)
                             .arg(kMaxAttempts));
            QEventLoop wait;
            QTimer::singleShot(kBackoffMs[attempt], &wait, &QEventLoop::quit);
            wait.exec();
        }

        // ── Resume ─────────────────────────────────────────────────────────
        // Whatever is already on disk is bytes we do not have to pay for
        // again. A 624 MB transfer that dies at 610 MB and restarts is the
        // difference between an install that finishes and one the user gives
        // up on. The digest at the end is what makes this safe: a resumed file
        // that is wrong fails the same check a fresh one would.
        qint64 have = QFileInfo::exists(destPath) ? QFileInfo(destPath).size() : 0;
        if (have > component.size) {
            // Longer than it should be: a previous run against a different
            // version, or a truncated write. Start again rather than reason
            // about it.
            QFile::remove(destPath);
            have = 0;
        }
        if (have == component.size) return true;  // already complete

        QNetworkRequest request{QUrl(urlFor(component.objectKey))};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        if (have > 0) {
            request.setRawHeader("Range", QByteArray("bytes=") + QByteArray::number(have) + "-");
        }

        QFile file(destPath);
        if (!file.open(have > 0 ? (QIODevice::WriteOnly | QIODevice::Append)
                                : QIODevice::WriteOnly)) {
            *error = QStringLiteral("Cannot write to %1.").arg(destPath);
            return false;  // not retryable: the disk is not going to change
        }

        QNetworkReply *reply = m_network->get(request);

        // A server that ignores Range answers 200 with the WHOLE file. Appending
        // that to what we already had would produce a file that is too long and
        // fails its digest — with a confusing message. Detected on the first
        // readyRead and handled by truncating back to zero.
        bool rangeHonoured = (have == 0);
        qint64 written = have;

        QElapsedTimer clock;
        clock.start();
        qint64 lastProgressBytes = have;
        qint64 lastDataMs = 0;

        QEventLoop loop;
        QTimer stallTimer;
        stallTimer.setInterval(5000);

        connect(reply, &QNetworkReply::readyRead, [&]() {
            if (!rangeHonoured) {
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                if (status == 200) {
                    file.seek(0);
                    file.resize(0);
                    written = 0;
                }
                rangeHonoured = true;
            }
            const QByteArray chunk = reply->readAll();
            file.write(chunk);
            written += chunk.size();
            lastDataMs = clock.elapsed();

            if (component.size > 0) {
                const int percent = int((written * 100) / component.size);
                const qint64 elapsed = clock.elapsed();
                const qint64 rate =
                    elapsed > 0 ? ((written - lastProgressBytes) * 1000) / elapsed : 0;
                emit progress(component.name, percent,
                              QStringLiteral("%1 — %2 of %3%4")
                                  .arg(component.label(),
                                       formatSize(written),
                                       formatSize(component.size),
                                       rate > 0 ? QStringLiteral("  ·  ") +
                                                      describe(rate, component.size - written)
                                                : QString()));
            }
        });

        // Stall detection rather than a total timeout. A big download over a bad
        // link is legitimately slow; what is actually broken is a connection
        // that has stopped delivering bytes entirely.
        connect(&stallTimer, &QTimer::timeout, [&]() {
            if (clock.elapsed() - lastDataMs > kStallTimeoutMs) reply->abort();
            if (m_cancelled) reply->abort();
        });

        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        stallTimer.start();
        loop.exec();
        stallTimer.stop();

        file.close();

        const QNetworkReply::NetworkError err = reply->error();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString errorString = reply->errorString();
        reply->deleteLater();

        if (err == QNetworkReply::NoError) {
            if (QFileInfo(destPath).size() == component.size) return true;
            // Short read with no error: the connection closed cleanly part way.
            // The partial file is KEPT — the next attempt resumes from it.
            continue;
        }

        if (m_cancelled) {
            *error = QStringLiteral("Cancelled.");
            return false;
        }

        // 404/403 mean the manifest points somewhere that is not there. Waiting
        // will not fix it, and four rounds of backoff before saying so just
        // wastes the user's time.
        if (status == 404 || status == 403) {
            *error = QStringLiteral("%1 is not available on the download service (%2).")
                         .arg(component.label())
                         .arg(status);
            return false;
        }

        if (!isRetryable(err) || attempt == kMaxAttempts - 1) {
            *error = QStringLiteral("Could not download %1.\n\n%2")
                         .arg(component.label(), errorString);
            return false;
        }
    }

    *error = QStringLiteral("Could not download %1 after %2 attempts.")
                 .arg(component.label())
                 .arg(kMaxAttempts);
    return false;
}

}  // namespace shdkit
