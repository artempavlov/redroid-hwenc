# Tier 8: Android 11/12/13 (HIDL Codec2) — ⚠ work in progress

Everything from Tier 4 onward (the Codec2 component, the daemon) targets Android 15 through the
**AIDL** Codec2 HAL. That HAL does not exist before Android 14 — confirmed by inspecting AOSP
source directly at `android-11.0.0_r48`, `android-12.0.0_r34` and `android-13.0.0_r83`:
`media/codec2/hal/aidl` only appears starting at `android-14.0.0_r75`; before that there is only
`media/codec2/hidl/{1.0,1.1}`. This tier is a HIDL port of the same component code, plus one more
AOSP-level fix those older versions need on any modern host kernel, to get the same hardware
encode working on Android 11/12/13.

## Status: confirmed working on AMD, confirmed broken on Intel

**AMD (Ryzen 5700G/Renoir), 08/11/2026 — confirmed, end to end, through the real module system of
[redroid-forge](https://github.com/fogelmanjg/redroid-forge)** (not a hand-patched container: a
real `POST /instances` with `modules: ["hwenc"]` against a freshly created Android 11 container).
A real VA-API hardware encode through `scrcpy`, confirmed via logcat (`VaapiEncComponent`), no
crashes, h264 output decodes cleanly.

**Intel (Iris Xe, TigerLake-LP), 08/11/2026 — confirmed broken, not just untested.** Same test,
same plumbing (boots, registers, encodes, no crash) — but the output bitstream is malformed:
`ffprobe`/`ffmpeg` report `Overread SPS by 3 bits` on every frame, a warning that does **not**
appear at all on the AMD output. A simple screen (home screen, static icons) still decoded and
looked correct despite the warning; content-heavy screens have not been tested yet, and the
pattern of "simple content looks fine, real content doesn't" has already bitten this project once
before (`redroid-nvidia`'s RTX 4060 texture corruption), so this is not assumed safe.

**Do not assume this is the same root cause as Tier 5.14's Intel P-frame bug** (fixed in 5.16 by
switching to a libavcodec-based encoder, see `tier5-vaapi-daemon/README.md`). That bug was in a
hand-written slice header; this component does not write SPS/PPS bytes by hand at all — it scans
the VA-API driver's own real coded output for where the parameter sets end
(`findLeadingParameterSetsSize` in `VaapiEncComponent.cpp`), so an "overread" is more likely a
boundary-detection bug in that scan than a bitstream construction bug. Worth checking that
function specifically before assuming a full rewrite (5.16's fix) is needed here too.

**Status for Android 12/13 specifically**: not run at all yet (no test hardware exercised beyond
the Android 11 image so far). The AOSP source gap (no AIDL) is confirmed identical across all
three versions, so the same component/patch is expected to apply, but "expected" is not "run" —
treat 12/13 as unverified until someone actually boots one.

## What's here

- `component/` — `VaapiEncComponent.{h,cpp}` / `VaapiDecComponent.{h,cpp}`, the real Codec2
  component logic. Identical to the AIDL version's (`tier4-codec2-skeleton`'s sibling in
  `tier5-vaapi-daemon/codec2-component/`) — the HIDL/AIDL split is purely in the registration
  layer below, not here.
- `service/service.cpp` — the HIDL registration layer: `registerAsService()`
  (hwservicemanager) instead of AIDL's `AServiceManager_addService()` (servicemanager). Adapted
  from AOSP's own empty-service template at
  `frameworks/av/media/codec2/hidl/services/vendor.cpp` (same starting point Tier 4 used for the
  AIDL version, per that tier's own README).
- `service/android.hardware.media.c2@1.1-vaapi-service.rc`, `manifest_media_c2_vaapi.xml`,
  `seccomp_policy/*` — init/VINTF/seccomp artifacts for the HIDL variant.
- `libion-dma_heap-patch/ion.c` — see below. **This one is a patch to AOSP itself**, not new code
  added by this project.

## The other half: `libion.so`'s missing `dma_heap` fallback

Even with the component above correctly registered, a real device hits a second, separate bug:
`C2AllocatorIon::getId()`/`setUsageMapper()` crash, because `ion_open()` in AOSP's own
`system/memory/libion/ion.c` hardcodes `open("/dev/ion", ...)` with **no fallback** — and
`/dev/ion` (legacy ION) does not exist on any host kernel using the modern `dma_heap` subsystem
(anything 5.10+). This is not an Android-11-specific oversight: the exact same code, byte-for-byte
unfixed, is present at `android-11.0.0_r48`, `android-12.0.0_r34` and `android-13.0.0_r83` — a
general AOSP-before-14 gap on any current host kernel, not something specific to one version.

`ion.c` here is that file, patched with a fallback: `ion_open()` tries `/dev/ion` first (unchanged
behaviour on an older host kernel that still has it), and on failure falls back to
`/dev/dma_heap/system`, tracking which mode it ended up in (`g_using_dma_heap`, a module-level
flag — this process only ever opens one ion/dma_heap fd in practice) so `ion_is_legacy()` and
`ion_alloc_fd()` both do the right thing afterward. The `dma-heap.h` uapi struct/ioctl definitions
are inlined directly in the patch (vendored, not `#include`d) because AOSP 11's bundled kernel
headers predate that header entirely.

**Build**: apply this file in place of `system/memory/libion/ion.c` in a redroid AOSP-11/12/13
checkout, then `m libion`. It needs to be installed to **three separate locations** inside the
running container (confirmed by hitting each one as a separate crash, in order):
`/vendor/lib64/libion.so` (the vendor/HIDL service's own copy, pulled in by its dependency
closure), `/system/lib64/libion.so`, and `/apex/com.android.media.swcodec/lib64/libion.so`
(framework/client-side code paths — `/system` and the apex are both byte-identical copies of the
same `android_x86_64_shared` build variant, so one build covers both destinations). `/apex/*` is a
live overlay mount created by Android's own init during boot — `docker cp` cannot reach it
directly (it only sees the build-time rootfs snapshot); stage the file at a normal writable path
first (e.g. `/data/local/tmp/`) and `docker exec ... cp` it the rest of the way once the container
is actually running.

## Build & deploy (the component/service, inside a redroid AOSP checkout)

```sh
mkdir -p external/vaapi_codec2/component external/vaapi_codec2/service
cp component/* external/vaapi_codec2/component/
cp service/* external/vaapi_codec2/service/
# (seccomp_policy/ keeps its own subfolder under service/)
```

Same product-makefile wiring as Tier 4 (`PRODUCT_PACKAGES += android.hardware.media.c2@1.1-vaapi-service`,
and the VINTF manifest fragment), adjusted for the `@1.1` HIDL interface version instead of the
AIDL one. See `tier4-codec2-skeleton/README.md`'s own "Build & deploy" section for the full detail
that still applies unchanged (product makefile location, SELinux, etc.).

## Runtime enablement differs from the AIDL path

AIDL (Android 15, Tier 4+) is switched on with `setprop media.c2.hal.selection aidl`. HIDL
(Android 11-13) instead needs `setprop debug.stagefright.ccodec 4` — the official Android 11
image ships this at `0`, which wholesale disables Codec2 for `MediaCodecList` regardless of HAL
registration (confirmed by reading `Codec2InfoBuilder::buildMediaCodecList()` directly). Both
need a `mediaserver` restart afterward (`GetServiceNames()`/`MediaCodecList` are each cached once
per process).

**A real boot-ordering race was found here** (08/11/2026, redroid-forge's integration): applying
this setprop based on a weak readiness proxy (e.g. "does `/data/local/tmp` exist yet") can
succeed with no error and then get silently reset moments later by a later boot step — wait for
the real `sys.boot_completed=1` signal first, not a proxy for it.

## What's next

1. The Intel SPS bug — check `findLeadingParameterSetsSize` first, before assuming a libavcodec
   rewrite (Tier 5.16's fix for an unrelated bug) is the answer here too.
2. Actually run this on Android 12 and 13, not just 11.
3. `VaapiDecComponent` (hwdec, the HIDL side) has not been exercised at all yet — only encode has
   a real validation run so far.
