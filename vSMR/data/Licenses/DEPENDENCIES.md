# vSMR dependency manifest

This file is an inventory, not a replacement for the license texts shipped in
this directory. It describes the dependencies used by vSMR 2.0.0-beta.5 so a
release can be reviewed without inspecting the Visual Studio project.

| Component | Version | Use | Distribution in the vSMR package | License material |
| --- | --- | --- | --- | --- |
| vSMR | 2.0.0-beta.5 | EuroScope plugin and bundled UI/data | `vSMR.dll`, `vSMR_Data` | `vSMR.txt` (GPL-3.0) |
| Microsoft WebView2 SDK/Loader | 1.0.4078.44 | Hosts the local Control Center UI; loader is linked statically | Code included in `vSMR_Data\Runtime\vSMR.Runtime.dll` | `Microsoft.WebView2-LICENSE.txt`, `Microsoft.WebView2-NOTICE.txt` |
| RapidJSON | 1.1.0 API, upstream commit `24b5e7a8b27f42fa16b96fc70aade9106cf7102f` | JSON parsing and writing | Code included in `vSMR_Data\Runtime\vSMR.Runtime.dll` | `RapidJSON.txt` (MIT) |
| Microsoft Visual C++ and MFC runtimes | MSVC v145 by default; v143 in compatibility CI | Native runtime | Not bundled; matching x86 redistributable required | Microsoft redistributable terms |
| EuroScope Plug-in SDK | repository-provided header/import library | Plug-in ABI | Not separately bundled | Consult the EuroScope SDK distribution terms |
| France-Ground-Layouts | commit `da539a889b213e66eb254f893fa087c263f65332` | Geometry and ground labels for 53 airports, converted to GeoJSON with vSMR palettes | `vSMR_Data/AVISO/*.geojson`; affected files carry source metadata | `France-Ground-Layouts.txt` (GPL-3.0); source: https://github.com/vaccfr/France-Ground-Layouts |
| Windows system libraries | Windows 10 SDK | GDI/GDI+, WinHTTP, multimedia, COM and windowing | Provided by Windows | Microsoft Windows terms |

The Microsoft WebView2 Evergreen Runtime itself is not included. Users install
the x86 runtime separately. The package also contains data and media assets;
their provenance review is tracked in `ASSET_PROVENANCE.md`.

RapidJSON headers are an unmodified full copy of `include/rapidjson` from
https://github.com/Tencent/rapidjson/tree/24b5e7a8b27f42fa16b96fc70aade9106cf7102f.
The downloaded source ZIP SHA-256 is
`df07f5ddfebbc2940181039f6c939ec2764a7303ef79b17958d9792a364306bb`.
All application DOM parsing goes through `shared/JsonDocument.hpp`, with
iterative parsing, UTF-8 validation, resource limits and embedded-NUL rejection.
