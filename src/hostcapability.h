// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: (c) 2026 SHD Systems Ltd

#pragma once

// Whether this machine can run a container backend, and what the person should
// do when it cannot.
//
// ── Why an installer asks this at all ──────────────────────────────────────
// A container backend is large — code_aster is about a gigabyte, and the
// virtual-machine image it runs in is a few hundred megabytes more. Offering
// that download to a machine whose firmware has virtualisation switched off, or
// whose IT policy blocks WSL, spends somebody's bandwidth and their patience
// and tells them at the end. The selection page is the last moment it is cheap
// to know.
//
// ── The rule about refusing ────────────────────────────────────────────────
// A negative answer **unticks the box and states why. It never hides it.**
// Selection is a bandwidth choice, not a licence check, and somebody who is
// about to walk into their BIOS — or who knows their IT department better than
// we do — must still be able to tick it. `offerByDefault` is the only thing
// this decides.
//
// ── Facts in, verdict out ──────────────────────────────────────────────────
// Gathering needs CPUID, the registry and wsl.exe. Deciding needs none of that,
// so the decision is a pure function over a struct and every interesting case —
// a locked-down corporate laptop, a desktop with VT-x off — is reachable in a
// test without owning a machine in that state.
//
// ── A note on the second copy ──────────────────────────────────────────────
// SHD Sim carries equivalent logic in its own `simcfd::HostVirtualisation`, and
// the duplication is deliberate rather than an oversight: this kit is
// AGPL-3.0-or-later (or commercial) and is shared across products, and it must
// not depend on any one product's proprietary library to build. The question
// "can this Windows machine run a container" belongs to an installer as much as
// to an application. If the two ever disagree, the app's answer governs what
// runs and this one governs only what is offered.

#include <QString>

namespace shdkit {

// What the machine reports about itself. Every field defaults to the
// pessimistic answer, and `known` says whether anything was gathered at all —
// an all-false struct must never read as "this machine cannot virtualise".
struct VirtualisationFacts {
    bool known = false;

    bool hypervisorPresent = false;             // something is virtualising already
    bool firmwareVirtualisationEnabled = false; // VT-x / AMD-V, switched on
    bool virtualMachinePlatformEnabled = false; // the optional Windows feature
    bool hyperVAvailable = false;               // Pro/Enterprise/Education only
    bool policyBlocksWsl = false;
    bool userCanElevate = false;                // in the local Administrators group
    int  windowsBuild = 0;

    // Reads this machine. Windows-only; elsewhere it returns `known = false`,
    // which the assessment treats as "offer it and say so".
    static VirtualisationFacts gather();
};

enum class VirtualisationVerdict {
    Unknown,
    Ready,
    EnableVirtualMachinePlatform,
    EnableInFirmware,
    UseHyperV,
    WindowsTooOld,
    NotSupportedByThisMachine
};

struct VirtualisationAssessment {
    VirtualisationVerdict verdict = VirtualisationVerdict::Unknown;

    QString summary;   // one line, for a heading
    QString remedy;    // what to do, written for the person

    bool offerByDefault = false;   // tick the box? never "hide the box"
    bool needsReboot = false;

    // Whether one elevation prompt would clear it. True for exactly one
    // verdict — the Virtual Machine Platform feature. Firmware is below the OS
    // and Group Policy belongs to somebody's IT department, so a button for
    // either would be a lie with a UAC prompt attached.
    bool fixableWithElevation = false;
    QString elevatedRemedyCommand;

    static VirtualisationAssessment of(const VirtualisationFacts &facts);
};

// Runs the elevated remedy behind a single UAC prompt. Returns false when the
// person declines, which is not a failure: nothing changed.
//
// `restartRequired` is set when the command succeeded and the machine needs a
// restart before the features take effect — DISM's 3010, which is the expected
// outcome rather than an error.
bool enableVirtualisationFeatures(const VirtualisationAssessment &assessment,
                                  bool *restartRequired,
                                  QString *error);

// Windows 10 21H1. Below this the provisioning path is untestable.
int minimumWindowsBuild();

}  // namespace shdkit
