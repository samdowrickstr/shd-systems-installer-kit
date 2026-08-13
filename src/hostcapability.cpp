// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: (c) 2026 SHD Systems Ltd

#include "hostcapability.h"

#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>

#ifdef Q_OS_WIN
#include <windows.h>
#include <cpuid.h>
#endif

namespace shdkit {

namespace {
const int kMinimumBuild = 19043;   // Windows 10 21H1
}

int minimumWindowsBuild() { return kMinimumBuild; }

VirtualisationFacts VirtualisationFacts::gather()
{
    VirtualisationFacts facts;

#ifdef Q_OS_WIN
    facts.known = true;

    // Not "can this CPU virtualise" but "is it switched on", which is the
    // question with a remedy attached.
    facts.firmwareVirtualisationEnabled =
        IsProcessorFeaturePresent(PF_VIRT_FIRMWARE_ENABLED) != 0;

    // CPUID leaf 1, ECX bit 31 is the architectural hypervisor-present bit:
    // set by every hypervisor, reserved-zero on bare metal. There is no
    // processor-feature constant for it, and a WMI query would need COM
    // initialised in an installer that otherwise does not use it.
    {
        unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
        if (__get_cpuid(1, &eax, &ebx, &ecx, &edx))
            facts.hypervisorPresent = (ecx & (1u << 31)) != 0;
    }

    // Membership of the local Administrators group — not whether we ARE
    // elevated. This installer is per-user and unelevated on purpose; this
    // only decides whether offering to fix it is honest.
    {
        SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
        PSID admins = nullptr;
        if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                     DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins)) {
            BOOL isMember = FALSE;
            if (CheckTokenMembership(nullptr, admins, &isMember))
                facts.userCanElevate = isMember != FALSE;
            FreeSid(admins);
        }
    }

    const QSettings current(
        QStringLiteral(R"(HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion)"),
        QSettings::NativeFormat);
    facts.windowsBuild = current.value(QStringLiteral("CurrentBuildNumber")).toString().toInt();

    // "Core" is Home, which has no Hyper-V. That makes Hyper-V narrower than
    // WSL2 rather than a universal fallback.
    const QString edition = current.value(QStringLiteral("EditionID")).toString();
    facts.hyperVAvailable = !edition.isEmpty() &&
                            !edition.startsWith(QLatin1String("Core"), Qt::CaseInsensitive);

    const QString system32 =
        QProcessEnvironment::systemEnvironment().value(QStringLiteral("SystemRoot"),
                                                       QStringLiteral("C:/Windows"))
        + QStringLiteral("/System32/");

    // wslapi.dll ships with the Virtual Machine Platform / WSL optional
    // features and is absent without them — cheaper and more reliable than
    // parsing DISM, and it is what WSL's own launcher looks for.
    facts.virtualMachinePlatformEnabled =
        QFileInfo::exists(system32 + QStringLiteral("wslapi.dll"));

    if (QFileInfo::exists(system32 + QStringLiteral("wsl.exe"))) {
        QProcess wsl;
        wsl.setProgram(system32 + QStringLiteral("wsl.exe"));
        wsl.setArguments({QStringLiteral("--status")});
        wsl.setProcessChannelMode(QProcess::MergedChannels);
        wsl.start();
        // Short timeout: on a machine where WSL is broken this can hang, and an
        // installer page that freezes is worse than one that says "could not
        // tell" and offers the download anyway.
        if (wsl.waitForFinished(4000)) {
            const QByteArray raw = wsl.readAll();
            const QString text =
                QString::fromUtf16(reinterpret_cast<const char16_t *>(raw.constData()));
            facts.policyBlocksWsl =
                text.contains(QStringLiteral("policy"), Qt::CaseInsensitive) ||
                text.contains(QStringLiteral("disabled by your administrator"),
                              Qt::CaseInsensitive) ||
                text.contains(QStringLiteral("blocked"), Qt::CaseInsensitive);
        }
    }
#endif

    return facts;
}

VirtualisationAssessment VirtualisationAssessment::of(const VirtualisationFacts &f)
{
    VirtualisationAssessment out;

    // ── Nothing gathered ───────────────────────────────────────────────────
    // Offered, not refused. The two mistakes are not symmetric: offering a
    // download to a machine that cannot use it costs bandwidth and leaves a
    // working application; refusing one that could have run it costs the solver
    // outright, with no way to change our mind from inside the installer.
    if (!f.known) {
        out.verdict = VirtualisationVerdict::Unknown;
        out.summary = QObject::tr("Could not check whether this machine can run the solver.");
        out.remedy = QObject::tr(
            "The download is offered anyway. If it turns out this machine cannot run "
            "it, everything else still works and the backend can be removed.");
        out.offerByDefault = true;
        return out;
    }

    // A build of 0 means "not read", never "ancient" — treating it as a version
    // would refuse every machine whose registry we failed to read.
    if (f.windowsBuild > 0 && f.windowsBuild < kMinimumBuild) {
        out.verdict = VirtualisationVerdict::WindowsTooOld;
        out.summary = QObject::tr("This version of Windows is too old to run the solver.");
        out.remedy = QObject::tr("Windows 10 version 21H1 (build %1) or newer is needed. "
                                 "Everything else installs and works as it is.")
                         .arg(kMinimumBuild);
        return out;
    }

    if (f.hypervisorPresent && f.virtualMachinePlatformEnabled) {
        out.verdict = VirtualisationVerdict::Ready;
        out.summary = QObject::tr("This machine can run the solver.");
        out.offerByDefault = true;
        return out;
    }

    // Firmware before policy: a machine with virtualisation switched off cannot
    // use Hyper-V either, so "try Hyper-V instead" would send somebody
    // somewhere that also fails.
    if (!f.firmwareVirtualisationEnabled && !f.hypervisorPresent) {
        out.verdict = VirtualisationVerdict::EnableInFirmware;
        out.summary = QObject::tr("Virtualisation is switched off for this machine.");
        out.remedy = QObject::tr(
            "It is usually a setting in the BIOS or UEFI firmware, named something like "
            "Intel VT-x, AMD-V or SVM. Turn it on and restart. You can still download "
            "the solver now so it is ready when you do.");
        out.needsReboot = true;
        return out;
    }

    if (f.policyBlocksWsl) {
        if (f.hyperVAvailable) {
            out.verdict = VirtualisationVerdict::UseHyperV;
            out.summary = QObject::tr("WSL is blocked here, but Hyper-V is available.");
            out.remedy = QObject::tr("The solver will use Hyper-V instead. Nothing else "
                                     "changes; it is the same virtual machine created a "
                                     "different way.");
            out.offerByDefault = true;
            return out;
        }
        out.verdict = VirtualisationVerdict::NotSupportedByThisMachine;
        out.summary = QObject::tr("WSL is blocked here and Hyper-V is not available.");
        out.remedy = QObject::tr(
            "This is an administrative policy rather than a limit of the hardware, so "
            "your IT department can change it. Until then the native solvers work "
            "normally, and a remote Linux host can be used instead.");
        return out;
    }

    if (!f.virtualMachinePlatformEnabled) {
        out.verdict = VirtualisationVerdict::EnableVirtualMachinePlatform;
        out.summary = QObject::tr("A Windows feature needs to be switched on first.");
        out.needsReboot = true;
        // Offered either way: the bytes are useful the moment the reboot
        // happens, and making somebody reinstall to get a download they could
        // have had is a worse trade than a gigabyte.
        out.offerByDefault = true;

        if (f.userCanElevate) {
            out.fixableWithElevation = true;
            // Both features or neither. Enabling the platform without the WSL
            // feature produces a machine that fails to start with an error
            // naming neither of them.
            out.elevatedRemedyCommand = QStringLiteral(
                "/Online /Enable-Feature /All /NoRestart "
                "/FeatureName:VirtualMachinePlatform "
                "/FeatureName:Microsoft-Windows-Subsystem-Linux");
            out.remedy = QObject::tr(
                "Virtual Machine Platform is not enabled. It can be switched on from "
                "here — one administrator prompt and a restart, and nothing else "
                "changes.");
        } else {
            out.remedy = QObject::tr(
                "Virtual Machine Platform is not enabled, and switching it on needs "
                "administrator rights this account does not have. Your IT department "
                "can enable it; the download is offered now so it is ready when they do.");
        }
        return out;
    }

    out.verdict = VirtualisationVerdict::Ready;
    out.summary = QObject::tr("This machine can run the solver.");
    out.offerByDefault = true;
    return out;
}

bool enableVirtualisationFeatures(const VirtualisationAssessment &assessment,
                                  bool *restartRequired, QString *error)
{
    if (restartRequired) *restartRequired = false;
    if (!assessment.fixableWithElevation) {
        if (error) *error = QObject::tr("There is nothing here that elevation can fix.");
        return false;
    }

#ifdef Q_OS_WIN
    // ShellExecute with the `runas` verb rather than QProcess: there is no way
    // to raise a child process's integrity level from an unelevated parent.
    // Exactly one prompt, and declining it must leave everything as it was.
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"runas";
    info.lpFile = L"dism.exe";
    info.lpParameters = reinterpret_cast<LPCWSTR>(assessment.elevatedRemedyCommand.utf16());
    info.nShow = SW_HIDE;

    if (!ShellExecuteExW(&info) || !info.hProcess) {
        if (error)
            *error = QObject::tr("The features were not enabled. Nothing has changed.");
        return false;
    }

    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);

    // 3010 is DISM for "succeeded, restart required" — the expected outcome
    // here. Reporting it as an error would tell somebody their machine is
    // broken at the moment it was fixed.
    if (code == 0 || code == 3010) {
        if (restartRequired) *restartRequired = true;
        return true;
    }

    if (error)
        *error = QObject::tr("Enabling the Windows features failed (code %1). An IT "
                             "policy may prevent it.")
                     .arg(code);
    return false;
#else
    if (error) *error = QObject::tr("Only available on Windows.");
    return false;
#endif
}

}  // namespace shdkit
