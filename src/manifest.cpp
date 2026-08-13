// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: (c) 2026 SHD Systems Ltd

#include "manifest.h"

#include "ed25519.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace shdkit {

QStringList Manifest::modules() const
{
    QStringList out;
    for (const Component &c : components) {
        for (const QString &m : c.modules) {
            if (!out.contains(m)) out.append(m);
        }
    }
    return out;
}

QList<Component> Manifest::backendsFor(const QString &module) const
{
    QList<Component> out;
    QStringList taken;

    for (const Component &c : components) {
        if (c.fetched && c.modules.contains(module)) {
            out.append(c);
            taken.append(c.name);
        }
    }

    // Pull in what those components cannot work without.
    //
    // A container solver needs the machine image it runs in, and the two are
    // separate components on purpose: the image is ~1 GB and changes with the
    // solver, the VM is a few hundred MB and changes almost never, so binding
    // them into one payload would re-download the VM on every solver update.
    // Keeping them separate is only safe if selecting one selects the other,
    // which is what this does — otherwise the install succeeds, the customer
    // has a gigabyte of solver, and the first run reports no virtual machine.
    //
    // Transitive, and cycle-safe: a requirement that names something already
    // taken is simply skipped, so a mutual `requires` pair terminates instead
    // of appending forever.
    for (int i = 0; i < out.size(); ++i) {
        for (const QString &need : out.at(i).requiresComponents) {
            if (taken.contains(need)) continue;
            for (const Component &c : components) {
                if (c.name != need || !c.fetched) continue;
                out.append(c);
                taken.append(c.name);
                break;
            }
        }
    }

    return out;
}

bool parseManifest(const QByteArray &json,
                   const QByteArray &signature,
                   const QString &publicKeyBase64,
                   Manifest *out,
                   QString *error)
{
    const auto fail = [error](const QString &message) {
        if (error) *error = message;
        return false;
    };

    // ── Signature first, before anything in the document is believed ───────
    //
    // Over the EXACT bytes received. Not over a re-serialisation of the parsed
    // document: a JSON round trip does not preserve key order or number
    // formatting, so verifying re-encoded bytes verifies something the server
    // never signed and the check silently becomes decorative.
    if (!publicKeyBase64.isEmpty()) {
        if (signature.isEmpty()) {
            return fail(QStringLiteral(
                "The release manifest arrived without a signature. It will not be used.\n\n"
                "This installer fetches solver backends and writes them into the install "
                "folder, so an unsigned manifest is refused rather than trusted."));
        }

        const std::vector<unsigned char> key =
            base64Decode(publicKeyBase64.toStdString());
        const std::vector<unsigned char> sig =
            base64Decode(QString::fromUtf8(signature).trimmed().toStdString());

        if (!ed25519Verify(reinterpret_cast<const unsigned char *>(json.constData()),
                           static_cast<size_t>(json.size()),
                           sig.data(), sig.size(), key.data(), key.size())) {
            return fail(QStringLiteral(
                "The release manifest's signature is not valid.\n\n"
                "Either it was altered in transit or it was not published by us. "
                "Nothing will be downloaded."));
        }
    }

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (doc.isNull() || !doc.isObject()) {
        return fail(QStringLiteral("The release manifest is not valid JSON (%1).")
                        .arg(parseError.errorString()));
    }

    const QJsonObject root = doc.object();
    Manifest manifest;
    manifest.schemaVersion = root.value(QStringLiteral("schemaVersion")).toInt(1);
    manifest.product = root.value(QStringLiteral("product")).toString();
    manifest.channel = root.value(QStringLiteral("channel")).toString(QStringLiteral("stable"));
    manifest.version = root.value(QStringLiteral("version")).toString();
    manifest.minimumVersion = root.value(QStringLiteral("minimumVersion")).toString();

    const QJsonArray components = root.value(QStringLiteral("components")).toArray();
    if (components.isEmpty()) {
        return fail(QStringLiteral("The release manifest lists no components."));
    }

    for (const QJsonValue &value : components) {
        const QJsonObject o = value.toObject();
        Component c;
        c.name = o.value(QStringLiteral("name")).toString();
        c.kind = o.value(QStringLiteral("kind")).toString();
        c.version = o.value(QStringLiteral("version")).toString(manifest.version);
        c.platform = o.value(QStringLiteral("platform")).toString(QStringLiteral("windows-x64"));
        c.sha256 = o.value(QStringLiteral("sha256")).toString().toLower();
        c.objectKey = o.value(QStringLiteral("objectKey")).toString();
        c.displayName = o.value(QStringLiteral("displayName")).toString();
        c.description = o.value(QStringLiteral("description")).toString();
        c.licenceUrl = o.value(QStringLiteral("licenceUrl")).toString();
        c.fetched = o.value(QStringLiteral("fetched")).toBool(false);

        // Absent means "native", which is what every manifest written before
        // container backends existed means. Reading an unknown value as native
        // would be worse than refusing: a container image unpacked as a plain
        // backend is a gigabyte in the install directory and no solver, with
        // nothing to show that anything went wrong.
        c.runtime = o.value(QStringLiteral("runtime")).toString(QStringLiteral("native"));
        if (c.runtime != QLatin1String("native") &&
            c.runtime != QLatin1String("container") &&
            c.runtime != QLatin1String("machine-image")) {
            return fail(QStringLiteral(
                "Component '%1' declares runtime '%2', which this installer does not "
                "understand. A newer installer is needed for this release.")
                            .arg(c.name, c.runtime));
        }

        for (const QJsonValue &r : o.value(QStringLiteral("requires")).toArray()) {
            c.requiresComponents.append(r.toString());
        }

        // toDouble, then cast: QJsonValue has no integer type and toInt()
        // silently truncates past 2^31. A 624 MB backend fits; a future 3 GB
        // container image does not, and it would arrive as a negative size that
        // passes every "is it big enough" check.
        c.size = static_cast<qint64>(o.value(QStringLiteral("size")).toDouble(0));

        for (const QJsonValue &m : o.value(QStringLiteral("modules")).toArray()) {
            c.modules.append(m.toString());
        }

        if (c.objectKey.isEmpty() || c.sha256.isEmpty()) {
            return fail(QStringLiteral("Component '%1' has no objectKey or no sha256.")
                            .arg(c.name.isEmpty() ? QStringLiteral("(unnamed)") : c.name));
        }
        // Size is mandatory for anything fetched: it is shown before the user
        // is asked to choose and summed for the disk-space check. A zero here
        // would make the installer promise a free download of 624 MB.
        if (c.fetched && c.size <= 0) {
            return fail(QStringLiteral("Component '%1' is fetched but declares no size.")
                            .arg(c.name));
        }

        manifest.components.append(c);
    }

    if (out) *out = manifest;
    return true;
}

QString formatSize(qint64 bytes)
{
    if (bytes <= 0) return QStringLiteral("—");
    const double mb = double(bytes) / (1024.0 * 1024.0);
    if (mb >= 1024.0) return QStringLiteral("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
    if (mb >= 10.0) return QStringLiteral("%1 MB").arg(qRound(mb));
    return QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
}

}  // namespace shdkit
