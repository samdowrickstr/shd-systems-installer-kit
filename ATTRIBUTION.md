# Attribution Requirement (AGPLv3 §7(b) additional term)

**This is an additional term applied under section 7(b) of the GNU Affero General
Public License, version 3.** It is a permitted additional term
("*Requiring preservation of specified reasonable legal notices or author
attributions … in the Appropriate Legal Notices displayed by works containing
it*") and travels with the software to all recipients. It is modelled on the
Attribution ("Exhibit B") mechanism of the Common Public Attribution License.

## The required notice

Any covered work — including the SHD Systems Installer Kit itself, any modified
version, and **any installer or application produced using it** — that has an
interactive user interface must display the following **Attribution Notice**:

- **Text:** `Powered by SHD Systems`
- **Link:** the notice must hyperlink to `https://shdsystems.example` (replace
  with SHD Systems Ltd's official URL before release).
- **Logo (optional):** where a logo is shown, it must be the SHD Systems logo at
  no less than legible size.

## How it must be displayed

1. The Attribution Notice must appear in each interactive user interface, in a
   **reasonably prominent and persistent** position (e.g. a footer or corner of
   the setup window), visible during normal use.
2. It must be **legible** and must **not be removed, hidden, obscured, resized to
   illegibility, or altered** except as permitted below.
3. Where the display is non-graphical (console/log output), an equivalent text
   line `Powered by SHD Systems` satisfies this requirement.

## Removal / white-labelling

The Attribution Notice may only be removed, replaced, or white-labelled under a
**commercial licence** from SHD Systems Ltd (see
[COMMERCIAL-LICENSE.md](COMMERCIAL-LICENSE.md)). Under the AGPLv3 free licence the
notice must remain.

In this codebase, removal is gated behind a build flag (`SHD_WHITELABEL` /
`COMMERCIAL` build) that is only licensed for use by commercial licensees. Using
that flag without a commercial licence is a breach of both this attribution term
and the commercial licence terms.

## Modified versions

Under AGPLv3 §7(b)/(c) you must also not misrepresent the origin of the software,
and modified versions must remain marked as changed from the original.

---

> **Note:** This addendum is a draft modelled on CPAL Exhibit B and should be
> reviewed by a solicitor before release. Replace the placeholder URL and confirm
> the exact notice wording/logo with SHD Systems Ltd.
