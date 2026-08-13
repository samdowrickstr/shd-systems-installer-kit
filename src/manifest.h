// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: (c) 2026 SHD Systems Ltd

#pragma once

// The release manifest: what components exist, which modules they serve, how
// big they are, and whether the whole document is genuinely ours.
//
// ── Why an installer reads a manifest at all ───────────────────────────────
// The kit used to embed everything: one exe, whole bundle inside, no network.
// That does not survive a solver backend — OpenFOAM alone is 624 MB against
// ~130 MB of application, and there are eight more physics modules coming. So
// the app stays embedded and the backends are selected on a page and fetched
// during the install (SHD-Sim-CFD ADR-0013).
//
// The embedded path is UNCHANGED and still the default. A config with no
// `components` block behaves exactly as it always did, because other SHD
// products use this kit and none of them asked for a downloader.
//
// ── The schema is not ours to invent ───────────────────────────────────────
// It is written by SHD-Sim-Website's `scripts/publish-release.mjs` and defined
// in its `src/lib/releases/manifest.ts`. Three readers must agree: this
// installer, the application's updater, and the admin page that records a
// release. Two fetchers with two notions of what is current is a bug that only
// appears in the field, months later — so when this disagrees with that file,
// that file is right.

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace shdkit {

// One downloadable or embedded piece of the product.
struct Component {
    QString name;         // stable id: "app", "openfoam". Not a filename.
    QString kind;         // app | portable | backend | examples | manifest
    QString version;      // the COMPONENT's version — "2606", not the app's
    QString platform;
    QStringList modules;  // physics this serves: ["fluids"]. Empty for the app.
    qint64 size = 0;      // bytes. Mandatory: shown before asking, and summed
                          // for the disk-space check.
    QString sha256;       // lower-case hex, checked after download
    QString objectKey;    // key under the download base, not a full URL
    QString displayName;
    QString description;
    QString licenceUrl;
    bool fetched = false; // true when it comes over the network

    // How the payload has to be handled once it is unpacked. Declared by the
    // publisher, never inferred from the file — the rule `kind` learned when it
    // was sniffed from an extension and classified a backend `.zip` as
    // "portable". A directory of Windows executables and a container image are
    // both an archive on the wire and need different treatment on the machine.
    //
    //   "native"         unpack into <install>/<name>/ and run directly
    //   "container"      unpack; the application loads the OCI archive inside
    //   "machine-image"  unpack; it is the VM a container backend runs in
    //
    // Unpacking is the same for all three. What differs is what has to happen
    // afterwards, which is the application's job rather than the installer's:
    // provisioning is deferred and retryable, and must never fail an install.
    QString runtime;      // empty means "native"

    // Components this one cannot work without, by name. A container backend
    // requires its machine image; fetching one without the other leaves a
    // customer holding a gigabyte and no way to run it.
    QStringList requiresComponents;   // JSON key is "requires"; the C++ name avoids the C++20 keyword

    bool isBackend() const { return kind == QLatin1String("backend"); }
    bool isContainer() const { return runtime == QLatin1String("container"); }
    bool isMachineImage() const { return runtime == QLatin1String("machine-image"); }
    // Whether this payload needs a container runtime provisioned before it can
    // do anything. Both halves do: the image needs somewhere to load, and the
    // machine image is that somewhere.
    bool needsProvisioning() const { return isContainer() || isMachineImage(); }

    QString label() const { return displayName.isEmpty() ? name : displayName; }
};

struct Manifest {
    int schemaVersion = 0;
    QString product;
    QString channel;
    QString version;
    QString minimumVersion;
    QList<Component> components;

    bool isEmpty() const { return components.isEmpty(); }

    // Every distinct module named by any component, in first-seen order — which
    // is the order the publisher chose, so the selection page does not reorder
    // somebody's deliberate arrangement into alphabetical.
    QStringList modules() const;

    // Components serving `module` and fetched over the network.
    QList<Component> backendsFor(const QString &module) const;
};

// Parses and, when a key is supplied, VERIFIES.
//
// `signature` is the detached base64 signature served beside the manifest.
// `publicKeyBase64` is compiled into this binary — never read from the same
// place as the manifest, because a signature checked against a key the
// attacker also supplied proves nothing.
//
// Returns false and sets `error` on a bad signature, a bad parse, or a
// component missing objectKey/sha256/size. **Fails closed**: if a public key
// is configured, an unsigned manifest is a failure, not a warning.
bool parseManifest(const QByteArray &json,
                   const QByteArray &signature,
                   const QString &publicKeyBase64,
                   Manifest *out,
                   QString *error);

// Bytes as something a person reads: "624 MB", "1.2 GB".
QString formatSize(qint64 bytes);

}  // namespace shdkit
