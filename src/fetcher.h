// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: (c) 2026 SHD Systems Ltd

#pragma once

// Downloads components during an installation.
//
// ── What this has to survive ───────────────────────────────────────────────
// The audience is a CFD engineer on a managed corporate machine, and the
// payload is hundreds of megabytes. That combination is where naive downloaders
// fail, so each of these is deliberate rather than defensive:
//
//   Resume       A 624 MB download that dies at 610 MB and starts again is
//                worse than one that refuses. HTTP Range picks it up.
//   Retry        With backoff, on transport errors only — never on a 404, which
//                is a wrong manifest and will still be wrong in four seconds.
//   Proxy        Qt is told to use the system proxy explicitly. A corporate
//                machine's proxy is in the registry, and without this the
//                download simply times out on exactly the customers who pay.
//   Disk space   Checked BEFORE asking, against the sum of what was selected
//                plus the unpacked size. Running out at 90% leaves a mess the
//                user has to clean up by hand.
//   Digest       Streamed while writing, so a 624 MB file is never held in
//                memory and never lands as an executable without being checked.
//
// ── The rule that outranks all of them ─────────────────────────────────────
// **A failed download must still leave a working application.** The app is
// embedded in the installer; the backends are not. If the network dies the user
// gets a working install whose Settings screen says "Fluids backend not
// installed — retry". Never a rollback. The one thing worse than an install
// that fetched nothing is an install that undid itself over a solver.

#include "manifest.h"

#include <QList>
#include <QObject>
#include <QString>

QT_BEGIN_NAMESPACE
class QNetworkAccessManager;
class QNetworkReply;
QT_END_NAMESPACE

namespace shdkit {

struct FetchResult {
    Component component;
    bool ok = false;
    QString path;   // where the payload landed, when ok
    QString error;  // why not, when not
};

class ComponentFetcher : public QObject {
    Q_OBJECT
public:
    explicit ComponentFetcher(QString baseUrl, QObject *parent = nullptr);
    ~ComponentFetcher() override;

    // Fetches the manifest and its detached signature, and verifies.
    // `publicKeyBase64` empty disables verification — which exists ONLY so a
    // product using this kit without a signing key still builds, and is logged
    // loudly by the caller.
    bool fetchManifest(const QString &manifestKey,
                       const QString &publicKeyBase64,
                       Manifest *out,
                       QString *error);

    // Downloads one component into `destDir`, resuming a partial file if one is
    // there, and verifies its SHA-256 before returning ok.
    FetchResult fetch(const Component &component, const QString &destDir);

    // Free bytes on the volume holding `path`, or -1 if it cannot be told.
    static qint64 freeSpaceFor(const QString &path);

    // Unpacked payloads are roughly 2.2x their compressed size for the OpenFOAM
    // tree, and the archive is kept until it is extracted — so the peak is
    // both. Deliberately generous: a pre-check that is too tight fails an
    // install that would have worked, which is the more annoying error.
    static qint64 spaceNeededFor(const QList<Component> &components);

    void cancel();

signals:
    // 0..100 for this component, plus a line for the status label.
    void progress(const QString &componentName, int percent, const QString &detail);
    void message(const QString &text);

private:
    QString urlFor(const QString &objectKey) const;
    bool download(const Component &component, const QString &destPath, QString *error);

    QString m_baseUrl;
    QNetworkAccessManager *m_network = nullptr;
    bool m_cancelled = false;
};

}  // namespace shdkit
