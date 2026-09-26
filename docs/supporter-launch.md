# Supporter Edition launch

OrbitLan Supporter Edition is a one-time, pay-what-you-want digital download. The minimum
price is **US$3**. It is not a subscription and does not use DRM or an in-app account.

## Product files

Build with:

```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\build-native-release.ps1 -Edition Supporter
```

Upload these files to the product:

- `dist\OrbitLan-Supporter-Installer.exe`
- `dist\OrbitLan-Supporter-Installer.exe.sha256`
- `dist\OrbitLan-Supporter-Portable.zip`
- `dist\OrbitLan-Supporter-Portable.zip.sha256`

The installer is the recommended download. The portable archive is an alternative for people
who prefer an extracted app; it still needs one administrator approval for the virtual adapter
and background network service on first run.

## Lemon Squeezy product settings

- Product: **OrbitLan Supporter Edition**
- Payment: **Single payment**
- Pricing: **Pay what you want**
- Minimum: **$3**
- Suggested: **$5**
- License keys: **Off**
- Product status before testing: **Draft**

Suggested short description:

> The official ready-to-run Supporter build of OrbitLan. Host larger virtual-LAN rooms,
> use Force Relay for troubleshooting, and get the darker Supporter theme with its moon
> animation. Core networking, encryption, self-hosting, and security updates remain available
> in the free Community Edition. One-time payment; no subscription or DRM.

After Lemon Squeezy approves the store, make a test purchase, replace
`SUPPORTER_CHECKOUT_URL` in `website/index.html` with the live checkout URL, and change the
disabled checkout label into a link. Do not upload the Supporter files to public GitHub Releases.

For each later app release, replace all four product files so existing customers can retrieve
the current official build from their Lemon Squeezy order library.
