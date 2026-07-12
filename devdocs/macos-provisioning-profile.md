# macOS Provisioning Profile Setup

Goal: silent keychain access across UFB.app and ufb-agent.app on macOS,
no per-app ACL prompts on cdhash mismatch. Achieved via the
`keychain-access-groups` entitlement, which Apple gates behind a
provisioning profile.

This is a one-time setup per Apple developer account. The profile
expires every 12 months and you renew + drop the new file in the same
location.

## What you do in Apple Developer portal

### 1. Register the bundle IDs (if not already)

https://developer.apple.com/account/resources/identifiers/list

Register all three with App ID type. Names and identifiers below.
For each, scroll the Capabilities list and check **Keychain Sharing**;
leave the others alone.

| Bundle ID         | Description           |
| ----------------- | --------------------- |
| `dev.ufb.app`     | UFB main GUI          |
| `dev.ufb.agent`   | UFB mount agent       |
| `dev.ufb.tray`    | UFB menu-bar tray     |

(If they already exist from notarization runs, just edit each to enable
Keychain Sharing.)

### 2. Create the provisioning profile

https://developer.apple.com/account/resources/profiles/list

- Click `+` (Create new profile).
- Distribution → **Developer ID** → Continue.
- App ID: pick `dev.ufb.app` (you can only pick one — we'll register
  the other two in a moment).
- Certificates: select your Developer ID Application certificate
  (the one in Keychain we already use for signing).
- Name: `UFB Developer ID` (or anything, just remember it).
- Generate → Download the `.provisionprofile` file.

Repeat for `dev.ufb.agent` and `dev.ufb.tray` so each gets its own
profile (Developer ID profiles are per-bundle-ID; you can't list
multiple App IDs in one Developer ID profile, unlike Development /
App Store profiles).

You'll end up with three files. Rename them to keep things clean:

- `UFB.provisionprofile`        (for `dev.ufb.app`)
- `ufb-agent.provisionprofile`  (for `dev.ufb.agent`)
- `UFBTray.provisionprofile`    (for `dev.ufb.tray`)

### 3. Drop the files into the repo

Place them at:

```
signing/UFB.provisionprofile
signing/ufb-agent.provisionprofile
signing/UFBTray.provisionprofile
```

`signing/` is git-ignored — profiles are tied to your developer
account, they shouldn't ship in the repo. Each developer who needs to
build a signed UFB wires up their own copies.

## What the build does after that

Re-run `scripts/sign-mac-dev.sh` (or any cmake build that triggers it).
The script detects the profile files, copies each into the matching
bundle's `Contents/embedded.provisionprofile`, and signs each app
using the `-with-keychain.entitlements` variant that declares
`keychain-access-groups: 5Z4S9VHV56.dev.ufb.shared`.

After this the agent's `SecItemCopyMatching` calls (and UFB.app's
`SecItemAdd` calls) include `kSecAttrAccessGroup =
5Z4S9VHV56.dev.ufb.shared`, the OS routes both apps to the same
shared keychain group, and there are no per-app ACL prompts. Ever.

## Renewal (yearly)

Apple Developer profiles expire 12 months after creation. When that
happens UFB will fail to launch with `amfid: Error -413 "No matching
profile found"` (the same error we see when no profile is present).
Regenerate via the same portal steps and replace the files in
`signing/`.

## Troubleshooting

**`taskgated-helper: Disallowing dev.ufb.app because no eligible
provisioning profiles found`** — the profile file isn't being embedded
or doesn't match the bundle ID. Check `codesign -d --extract-certs`
on the bundle; the profile should appear under
`Contents/embedded.provisionprofile`.

**`amfid: Error -413 "No matching profile found"`** — the profile is
embedded but doesn't list this bundle ID in its `App ID` field. Open
the profile's plist with `security cms -D -i UFB.provisionprofile`
and confirm the `Entitlements > application-identifier` matches the
bundle.

**`The signature does not include a secure timestamp.`** — sign
script ran without `--timestamp`. Should not happen; the script always
passes it.
