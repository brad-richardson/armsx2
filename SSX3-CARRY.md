# SSX 3 carry on ARMSX2 master

Branch `armsx2-ssx3-four` carries GS and microVU changes needed by the
source-built PS2Recomp SSX 3 adapter. It follows `armsx2/master` through
ordinary merge commits. The twelve SG groups describe the in-tree changes;
additive adapters and platform build files are in the PS2Recomp fork at
`ps2xRuntime/third_party/armsx2/`.

Classes: **A** additive or suitable outside this tree; **B** a hook or API
needed by the adapter; **C** a bug fix; **D** a local policy or driver
workaround. This classification describes the current carry, not an upstream
submission plan.

| Group | Change | Class |
| --- | --- | --- |
| SG01 | iOS, Android and Mac embed build portability | A/C |
| SG02 | Surfaceless Metal host and asynchronous IOSurface export | B/C |
| SG03 | Android AHB export seam, export drains, bundled shaderc and external Vulkan HAL loader | B/D |
| SG04 | GS export filters (CAS and sharp bilinear) | B |
| SG05 | Split-front state refresh under draw buffering | C |
| SG06 | PACKED resume at a chunk boundary keeps its tag open | C |
| SG07 | Adreno destination-read paths | D |
| SG08 | Adreno/Turnip blend workarounds | D |
| SG09 | TFX pipeline prewarm and pipeline-cache flush | B |
| SG10 | GS diagnostics for back-thread, pipeline and stall counters | B |
| SG11 | microVU embed hooks | B |
| SG12 | arm64 microVU capture recorder hook | B/A |

`GE1_ADRENO_AD_ACCU=rta` remains in SG08/SG10 as a default-off diagnostic
option. The root `CMakeLists.txt` has an optional OM1 capture hook guarded by
`if(DEFINED OM1_CAPTURE_INCLUDE)`; a plain configure needs no recording inputs.
The GE1 adapter builds outside this tree and therefore needs no root build
hook. No game assets, generated guest code or offline recordings belong in
this branch.

For source preparation and the supported public Android build route, see
[`tools/ssx3/README.md`](https://github.com/brad-richardson/PS2Recomp/blob/ssx3/tools/ssx3/README.md)
in the PS2Recomp fork. The pinned vendor commit for that route is recorded in
its `tools/ssx3/build.py`.
