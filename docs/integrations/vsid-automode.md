# vSID automatic-mode indicator

vSMR reads the optional global String field `vsid/automode` from the EuroScope Plugin Bridge. Each record is exactly seven ASCII bytes: `LFPG=1;` (On) or `LFPO=0;` (Off). Records have unique uppercase four-character airport identifiers, no whitespace or NUL terminator, and a maximum total length of 4096 bytes. An absent airport, unsupported field, disconnected provider, or invalid snapshot displays **Unknown**. vSMR never infers success from a toggle command being consumed.

The bridge-enabled vSID currently publishes only SID, runway, and CFL. The companion [patch](vsid-automode.patch) adds the global field, bumps the provider schema minor version to 1, and publishes actual airport settings every timer tick. This includes state changes from manual commands, reloads, and controller coordination.

The patch targets [AlexisBalzano/vSID at fce87a0](https://github.com/AlexisBalzano/vSID/tree/fce87a08db147d25ff92b041456cc93cf83a32d1). From a checkout of that revision, apply it with `git apply <path-to-vsid-automode.patch>`, then build and load vSID using that project's build instructions. It has been checked for clean application; the companion vSID DLL has not been built or installed as part of the vSMR build.

The existing **Auto status** action remains available on older vSID builds. It displays the provider's status in EuroScope, while the vSMR indicator stays Unknown. The vSMR renderer reads a timer snapshot and makes no bridge calls during drawing.
