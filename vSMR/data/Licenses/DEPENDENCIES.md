# vSMR dependency manifest

This file is an inventory, not a replacement for the license texts shipped in
this directory. It describes the dependencies used by vSMR 2.0.0-beta.2 so a
release can be reviewed without inspecting the Visual Studio project.

| Component | Version | Use | Distribution in the vSMR package | License material |
| --- | --- | --- | --- | --- |
| vSMR | 2.0.0-beta.2 | EuroScope plugin and bundled UI/data | `vSMR.dll`, `vSMR_Data` | `vSMR.txt` (GPL-3.0) |
| Microsoft WebView2 SDK/Loader | 1.0.4078.44 | Hosts the local Control Center UI; loader is linked statically | Code included in `vSMR.dll` | `Microsoft.WebView2-LICENSE.txt`, `Microsoft.WebView2-NOTICE.txt` |
| RapidJSON | bundled source snapshot | JSON parsing and writing | Code included in `vSMR.dll` | `RapidJSON.txt` (MIT) |
| Microsoft Visual C++ and MFC runtimes | MSVC v145 | Native runtime | Not bundled; x86 redistributable required | Microsoft redistributable terms |
| EuroScope Plug-in SDK | repository-provided header/import library | Plug-in ABI | Not separately bundled | Consult the EuroScope SDK distribution terms |
| Windows system libraries | Windows 10 SDK | GDI/GDI+, WinHTTP, multimedia, COM and windowing | Provided by Windows | Microsoft Windows terms |

The Microsoft WebView2 Evergreen Runtime itself is not included. Users install
the x86 runtime separately. The package also contains data and media assets;
their provenance review is tracked in `ASSET_PROVENANCE.md`.
