# Supporter Edition launch

OrbitLan Supporter Edition is a one-time **US$5** digital product. It is not a recurring
membership and does not use DRM or an in-app account.

## Product files

Build with:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-native-release.ps1 -Edition Supporter
```

Keep these private build artifacts out of GitHub Releases:

- `dist\OrbitLan-Supporter-Installer.exe`
- `dist\OrbitLan-Supporter-Installer.exe.sha256`
- `dist\OrbitLan-Supporter-Portable.zip`
- `dist\OrbitLan-Supporter-Portable.zip.sha256`

The installer is the recommended download. The portable archive is an alternative for people
who prefer an extracted app; it still needs one administrator approval for the virtual adapter
and background network service on first run.

Patreon accepts ZIP attachments but not standalone Windows executables. Package the four
files above into one ZIP named `OrbitLan-Supporter-v1.0.0.zip` and upload that ZIP.

## Patreon product settings

Published product:
https://www.patreon.com/orbitlan/posts/orbitlan-edition-170621986

- Open **Creator studio → Create → Product**.
- Title: **OrbitLan Supporter Edition v1.0.0**
- Price: **$5**
- Access: **For purchase only**
- Attachment: `OrbitLan-Supporter-v1.0.0.zip`
- Preview before publishing: **On**

Suggested short description:

> The official ready-to-run Supporter build of OrbitLan. Host larger virtual-LAN rooms,
> use Force Relay for troubleshooting, and get the darker Supporter theme with its moon
> animation. Core networking, encryption, self-hosting, and security updates remain available
> in the free Community Edition. One-time payment; no subscription or DRM.

State clearly on the product page that OrbitLan is MIT licensed, Community Edition is free,
and the complete source can build either edition. The purchase is for convenient official
Supporter builds and helps fund the shared infrastructure; it is not a source-code paywall.

## Payout settings

In **Settings → Billing and payouts**, select Pakistan and add **Bank transfer**, not
**Payoneer Wallet**. Patreon uses Payoneer to process the direct bank transfer, but this route
does not require a Payoneer Wallet account. Use PKR as the receiving currency if offered and
complete the tax information truthfully.

Pakistan direct-bank payouts have a $10 minimum. USD-to-PKR transfers currently cost 1.55%
of the payout plus $0.25; the exact fee and converted amount are shown before confirming.

## Website checkout

Patreon does not provide a supported embeddable checkout for one-time products. The website
links directly to the published product above; purchase and account handling stay on Patreon,
and buyers can retrieve updates from their Patreon Purchases tab. Complete one real purchase
before publicly announcing the Supporter Edition so the delivery flow is tested end to end.

For each later app release, create a new bundle from the four private build artifacts and
replace the product attachment so existing customers can retrieve the current official build
from their Patreon Purchases tab.
