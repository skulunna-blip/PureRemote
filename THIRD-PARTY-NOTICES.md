# Third-party notices

PureRemote's own code (the shell, `pmrelay`, the scripts) is MIT — see [LICENSE](LICENSE).
A **packaged build** additionally bundles the components below. They are **not** covered by
that MIT licence.

## RustDesk core — AGPL-3.0

The portable build bundles the **unmodified** RustDesk 1.5.0 runtime:

| File / folder | What it is |
|---|---|
| `bin/rustdesk.exe`, `bin/librustdesk.dll` | the RustDesk core |
| `bin/*.dll`, `bin/data/`, `bin/drivers/` | RustDesk's own runtime, Flutter engine and plugins |
| `bin/RuntimeBroker_rustdesk.exe`, `bin/WindowInjection.dll` | shipped as part of that runtime |

* Copyright © the RustDesk authors.
* Licensed under the **GNU Affero General Public License v3.0** — the full text ships with
  RustDesk or is available at <https://www.gnu.org/licenses/agpl-3.0.html>.
* Corresponding source: <https://github.com/rustdesk/rustdesk> (tag `1.5.0`). The binaries
  here are byte-for-byte as released; no modifications were made to them.
* PureRemote is not affiliated with, endorsed by, or supported by the RustDesk project.

If you redistribute a packaged build, you are redistributing RustDesk under the AGPL and must
comply with its terms.

## usbmmidd — virtual display driver

`bin/usbmmidd_v2/` is a third-party virtual display driver bundled with RustDesk. Its own
licence is included at `bin/usbmmidd_v2/License.txt`.

## Qt 6

The shell is built against Qt 6.8 (The Qt Company), licensed under **LGPL-3.0**. Qt is used as
shared libraries (`Qt6Core.dll`, `Qt6Gui.dll`, `Qt6Widgets.dll`, `Qt6Network.dll`,
`Qt6Svg.dll` and `plugins/`), which are shipped unmodified and can be replaced with your own
builds of the same version. Source: <https://download.qt.io/official_releases/qt/>.

Also shipped by the Qt deployment tool: `D3Dcompiler_47.dll` (Microsoft), `opengl32sw.dll`
(Mesa, MIT), and the MinGW runtime (`libgcc_s_seh-1.dll`, `libstdc++-6.dll`,
`libwinpthread-1.dll` — GPL-with-GCC-exception / MIT).

## Tailscale (inside `pmrelay.exe`)

`pmrelay.exe` is PureRemote's own Go helper (MIT), but Go statically links its dependencies
into the binary. It uses the `tailscale.com` client library, © Tailscale Inc., licensed under
the **BSD 3-Clause** licence — <https://github.com/tailscale/tailscale>.
