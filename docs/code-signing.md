# Code signing for releases

Release packages build and publish without OS code signatures until signing is configured (every tag build is
still attested; see [Linux and checksums](#linux-and-checksums-build-provenance)). Each platform's signing steps turn on
by themselves once that platform's secrets exist (repo **Settings → Secrets and variables → Actions**). Only tag
builds (`v*`) are signed; main and branch builds stay unsigned and never touch the signing services.

## Windows: Azure Artifact Signing

Artifact Signing (formerly *Trusted Signing*) issues short-lived Microsoft-rooted code-signing certificates
and signs with them; the key never leaves Azure. The Basic SKU is about $9.99/month. Individual developers
qualify in the USA and Canada; organizations in more countries.

`release.yml` signs `VideoSpeedTracker.exe`, `ProcessHiLites.exe` and `VSTStudio.exe` in the `windows` job. It logs
in to Azure with GitHub OIDC (no client secret is stored anywhere), then runs `azure/artifact-signing-action`,
which hashes each exe, has Azure sign the digest, and timestamps it at `http://timestamp.acs.microsoft.com`. On
tag builds the job runs in the GitHub environment `release`, and that environment is what Azure trusts.

### 1. Signing account (Azure portal)

1. In the subscription, open **Resource providers** and register `Microsoft.CodeSigning`.
2. Create an **Artifact Signing account** (SKU Basic). Note its **region**: the signing endpoint must match it.
3. In the account, **Identity validations → New → Public**, type *Individual* (or *Organization*). The legal name
   must match your ID exactly. Approval usually takes from a few hours to a few business days; nothing can be
   signed until its status is **Completed**.
4. **Certificate profiles → Create → Public Trust**, pointing at that validation. Its status must be **Active**.
   (Private Trust profiles are for internal apps and don't satisfy SmartScreen.)

### 2. Identity GitHub Actions signs as (Microsoft Entra ID)

1. **App registrations → New registration** (single tenant, no redirect URI). Note its **Application (client) ID**
   and **Directory (tenant) ID**.
2. In the app, **Certificates & secrets → Federated credentials → Add credential**, scenario *GitHub Actions
   deploying Azure resources*:
   - Organization `joekiller`, Repository `VideoSpeedTracker` (the portal also asks for their numeric IDs and
     builds the ID-based subject this repo's tokens use, `repo:joekiller@<org id>/VideoSpeedTracker@<repo id>:...`)
   - Entity type **Environment**, environment **`release`**
   - Resulting subject: `repo:joekiller@1022919/VideoSpeedTracker@1430362908:environment:release`

   A *Tag* entity type matches a single exact tag name, so it can't cover every `v*` release; the environment can.
3. On the **Artifact Signing account** (not the subscription), **Access control (IAM) → Add role assignment →
   Artifact Signing Certificate Profile Signer** (older docs: *Trusted Signing Certificate Profile Signer*) →
   *User, group, or service principal* → select the app registration. Assigning it needs Owner or User Access
   Administrator. A new assignment can take several minutes to take effect.

   CLI equivalent:

   ```sh
   az role assignment create --assignee <application client id> \
     --role "Artifact Signing Certificate Profile Signer" \
     --scope /subscriptions/<sub>/resourceGroups/<rg>/providers/Microsoft.CodeSigning/codeSigningAccounts/<account>
   ```

The app needs no role on the subscription; the workflow logs in with `allow-no-subscriptions`.

### 3. GitHub settings

- **Settings → Environments → `release`** (created by the first tag build, or create it yourself): under
  *Deployment branches and tags* choose *Selected branches and tags* and add the tag rule `v*`.
- **Secrets**: `AZURE_CLIENT_ID`, `AZURE_TENANT_ID`.
- **Variables**:
  - `AZURE_SIGNING_ENDPOINT`: the account's region endpoint, which is the **Account URI** on its Overview page,
    for example `https://eus.codesigning.azure.net/` (East US), `https://wus2.codesigning.azure.net/` (West US 2),
    `https://weu.codesigning.azure.net/` (West Europe). Full list: [Signing integrations](https://learn.microsoft.com/azure/artifact-signing/how-to-signing-integrations).
  - `AZURE_SIGNING_ACCOUNT`: the account name, exactly as in the portal.
  - `AZURE_CERT_PROFILE`: the certificate profile name.

Push a `v*` tag and check the `windows` job's *Authenticode-sign the executables* step; the *Zip* step then prints
`Get-AuthenticodeSignature` for each exe (`Valid` when signed). A re-run of a tag build uses the workflow as it was
at that tag, so workflow fixes need a new tag; Azure-side fixes only need the failed job re-run.

### Troubleshooting

| Failing step and message | Cause | Fix |
|---|---|---|
| Azure login: `AADSTS700213: No matching federated identity record found` | The token's subject doesn't match the federated credential (it prints the subject it presented) | Credential entity type *Environment* / `release`; only tag builds use that environment |
| Azure login: `No subscriptions found` | Expected: the app has no subscription role | Keep `allow-no-subscriptions: true` on `azure/login` |
| Signing: `403 (Forbidden)` | The app isn't allowed to sign with this account/profile | Check in order: the Signer role on the **account** for the **app registration** (wait a few minutes after adding it); `AZURE_SIGNING_ENDPOINT` matches the account's region (a mismatch is the most common 403); account and profile names are exact; the profile is **Active** and the identity validation **Completed** |
| Signing: `No files matched` | Path or filter | The step signs `*.exe` under `pkg\<package>` |

The exes are signed first. The installer (`packaging/windows/vst-setup.iss`) is then built around them and
signed with the same profile. Its uninstaller is generated at install time and is not signed.

SmartScreen can still warn on early downloads until the signing identity builds reputation.

## macOS: Developer ID and notarization

Apple Developer Program, $99/year.

1. Enroll as an individual, create a *Developer ID Application* certificate and export it with its key as a .p12.
2. Create an app-specific password for your Apple ID (account.apple.com → Sign-In and Security).
3. Secrets: `MACOS_CERT_P12_BASE64` (`base64 -i cert.p12`), `MACOS_CERT_PASSWORD`, `APPLE_ID`, `APPLE_TEAM_ID`,
   `APPLE_APP_PASSWORD`. Either repo secrets or secrets of the `release` environment work: the macos job enters
   `release` on tag builds, like windows.

The binaries are then signed with the hardened runtime and notarized with `notarytool`. VST Studio ships as
`VSTStudio.app` with the ticket stapled, so it opens offline: Gatekeeper refuses a double-clicked bare executable
("Apple could not verify…") even when it's notarized. The command-line tools can't be stapled, so Gatekeeper checks
their notarization online the first time they run. The `.dmg` is built from those stapled files, then signed,
notarized and stapled itself (a second, short notary submission). The job ends with `spctl --assess` on the app, so a release whose
app Gatekeeper would refuse fails instead of publishing; the README shows the same checks for a downloaded copy.

## Linux and checksums: build provenance

Nothing to set up, no secrets, no cost beyond a few seconds of the tag build. Every package (the `.deb`s, but
also the Windows and macOS files) gets a GitHub artifact attestation: the job's short-lived OIDC identity is
exchanged with Sigstore for a ten-minute certificate naming this repo, `release.yml` and the tag, which signs an
SLSA provenance statement over the file's sha256, and the signature is logged in Sigstore's public transparency
log. Nobody holds a key, so there is no key to steal or rotate. The README shows how to verify.

The release is created as a draft. Once every platform is uploaded, the last job (`checksums`) writes `SHA256SUMS`
from the digests GitHub recorded for the stored assets, attests it, uploads it with `SHA256SUMS.sigstore.json` (the
signature bundle) and only then publishes. If any platform fails, the release stays a draft: fix it and
"Re-run failed jobs".

**Turn on immutable releases** (repo **Settings → General → Releases → Enable release immutability**). A published
release's assets and tag can then never be replaced, even with a write token, and GitHub adds its own release
attestation (`gh release verify v1.8.0`, `gh release verify-asset v1.8.0 <file>`). Because the workflow publishes
only at the end, it works with this as is. Create releases by pushing the tag, or as a draft in the UI; a release
published from the UI before the build would refuse the uploads.

Not done, on purpose:

- **GPG-signed `.deb`s** (`dpkg-sig`, `debsigs`): dpkg and apt don't check them by default, so they add a key to
  guard and protect almost no one.
- **A signed apt repository** (`apt install videospeedtracker` with automatic updates): the Linux equivalent of
  Authenticode for users who want it. It needs a long-lived GPG key in the `release` environment and a host for
  the repo (GitHub Pages works). Worth doing if people start installing VST on many Pis.

Sources: [Artifact Signing signing integrations](https://learn.microsoft.com/azure/artifact-signing/how-to-signing-integrations),
[azure/artifact-signing-action](https://github.com/Azure/artifact-signing-action),
[Scott Hanselman's walkthrough](https://www.hanselman.com/blog/automatically-signing-a-windows-exe-with-azure-trusted-signing-dotnet-sign-and-github-actions).
