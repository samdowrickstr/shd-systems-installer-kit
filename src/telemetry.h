// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: (c) 2026 SHD Systems Ltd

#pragma once

// One anonymous request when an install or an uninstall completes.
//
// ── Why this is worth having when the application can report for itself ────
// Because the application's reporting is behind a first-run choice, and this is
// not. An install counter that only counts the people who agreed to be counted
// is a biased sample of exactly the population a product most needs to
// understand — the ones who installed it, opened it once, and never came back.
//
// The difference in kind is what makes that acceptable rather than a loophole:
//
//   the application     stores a persistent identifier on the machine, so it
//                       can say "this install ran again today". Storing that
//                       identifier is what puts it inside PECR regulation 6 and
//                       behind an explicit choice.
//
//   this                sends **no identifier of any kind** and stores nothing
//                       on the machine in order to send it. There is no
//                       persistent state, so regulation 6 is not engaged. The
//                       only personal data in the transaction is the source
//                       address, which the server turns into a two-letter
//                       country code and does not store.
//
// The consequence is that it cannot be deduplicated. A reinstall is a second
// row, and a repair is a third. So whatever consumes this must call the number
// "installer runs" and never "machines" — the server side of this contract
// spells that out in the same words (SHD-Sim-Website docs/21).
//
// ── Off unless a product asks for it ──────────────────────────────────────
// Absent from a config.json, nothing is sent and no socket is opened. Several
// products use this kit and none of them asked for a counter; the same posture
// the component downloader takes, and for the same reason.
//
// ── It must never affect the install ──────────────────────────────────────
// Fire-and-forget with a short timeout, on a local event loop that is given a
// hard deadline. A statistics endpoint that is slow, unreachable, or answering
// nonsense must not delay somebody's installation by so much as a spinner, and
// must never be able to fail one.

#include <QString>

namespace shdkit {

// Where to report an install, if anywhere. Read from config.json's `telemetry`
// block; disabled and inert when absent.
struct TelemetryConfig {
    bool enabled = false;
    // Full URL, e.g. https://api.shd-sim.com/v1/telemetry/install. A whole URL
    // rather than a host plus an assumed path: this kit is used by products on
    // different services, and guessing a path for them is how one of them ends
    // up silently posting into a 404.
    QString endpoint;
    QString productCode;   // e.g. SHDSIM
    QString channel;       // e.g. stable
};

// `kind` is "install" or "uninstall". Blocks for at most a couple of seconds
// and swallows every failure; there is no return value because there is no
// caller that could sensibly do anything with one.
void reportInstallRun(const TelemetryConfig &config,
                      const QString &kind,
                      const QString &version);

}  // namespace shdkit
