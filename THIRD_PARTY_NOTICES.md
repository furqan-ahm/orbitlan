# Third-party notices

OrbitLan's own source code is licensed under the repository's MIT license. The Windows
release also bundles the following separately licensed components.

## TAP-Windows6 9.27.0

The files `OemVista.inf`, `tap0901.cat`, and `tap0901.sys` under
`client/ui/payload/driver/` come from the OpenVPN TAP-Windows6 9.27.0 release.

- Copyright (C) 2002-2024 OpenVPN Technologies, Inc. and contributors.
- License: GNU General Public License version 2, with the WDK system-library exception
  described by the upstream project.
- Source: https://github.com/OpenVPN/tap-windows6/tree/9.27.0
- Upstream license: https://github.com/OpenVPN/tap-windows6/blob/9.27.0/COPYING

A copy of GPL-2.0 is included with the bundled driver as
`client/ui/payload/driver/LICENSE-GPL-2.0.txt`.

## Microsoft Device Console (DevCon)

`client/ui/payload/driver/devcon.exe` is built from Microsoft's Device Console sample and
is used only to install the TAP device.

- Copyright (c) Microsoft Corporation.
- License: Microsoft Public License (MS-PL).
- Source: https://github.com/microsoft/Windows-driver-samples/tree/main/setup/devcon

A copy of the MS-PL is included as `client/ui/payload/driver/LICENSE-MS-PL.txt`.
