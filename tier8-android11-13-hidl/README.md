# Tier 8: Android 11 (HIDL Codec2)

Everything from Tier 4 onward (the Codec2 component, the daemon) targets Android 15 through the
**AIDL** Codec2 HAL. That HAL does not exist before Android 14 — confirmed by inspecting AOSP
source directly at `android-11.0.0_r48`, `android-12.0.0_r34` and `android-13.0.0_r83`:
`media/codec2/hal/aidl` only appears starting at `android-14.0.0_r75`; before that there is only
`media/codec2/hidl/{1.0,1.1}`. This tier is a HIDL port of the same component code, plus one more
AOSP-level fix those older versions need on any modern host kernel, to get the same hardware
encode working.

**Android 12 and 13: assumed to work the same way, not separately tested.** This tier exists
because of one single fact — no AIDL Codec2 before Android 14 — and that fact is confirmed
identical at the 12 and 13 AOSP tags too, same as 11. Same gap, same fix, same component: there's
no version-specific logic in here for 11 vs. 12 vs. 13, so the working assumption is that it just
works there too. It's only validated on real hardware for 11, though, so if you run this on 12 or
13 and hit something that doesn't work, that's exactly the kind of thing this assumption could be
wrong about — [open an issue](https://github.com/fogelmanjg/redroid-hwenc/issues) with what you
saw and it'll get looked at specifically, not treated as out of scope.

**Just want an Android 11 instance with this working, not the component source?**
[redroid-forge](https://github.com/fogelmanjg/redroid-forge)'s `hwenc` module is this exact
component, wired into a real instance manager — `android-11-official` is one of its two catalog
images.

## Status: confirmed working on both AMD and Intel (Intel needs the ffmpeg backend)

**AMD (Ryzen 5700G/Renoir), 08/11/2026 — confirmed, end to end, through the real module system of
[redroid-forge](https://github.com/fogelmanjg/redroid-forge)** (not a hand-patched container: a
real `POST /instances` with `modules: ["hwenc"]` against a freshly created Android 11 container).
A real VA-API hardware encode through `scrcpy`, confirmed via logcat (`VaapiEncComponent`), no
crashes, h264 output decodes cleanly.

**Intel (Iris Xe, TigerLake-LP), 08/11/2026 — confirmed broken on the default (`legacy`) backend,
confirmed fixed on the `ffmpeg` backend (09/11/2026).** Same test, same plumbing (boots, registers,
encodes, no crash) — but on `legacy` the output bitstream is malformed: `ffprobe`/`ffmpeg` report
`Overread SPS by 3 bits` on every frame, a warning that does **not** appear at all on the AMD
output. Switching the instance's encode backend to `ffmpeg` (the same libavcodec/`h264_vaapi`
backend 5.16 added to fix Intel's P-frame bug) **clears the SPS warning entirely** — checked
directly on the running instance, not inferred. So this *was* the same class of bug as 5.14's
P-frame issue after all (see the correction below), and the fix is the same one-line config
change, not a new patch to this component.

**Correction, 09/11/2026 (the paragraph below is kept for the record, but its conclusion was
wrong):** this file originally argued the SPS bug was *probably not* the same root cause as
5.14's P-frame bug, because this component scans the driver's own coded output
(`findLeadingParameterSetsSize` in `VaapiEncComponent.cpp`) instead of writing SPS/PPS bytes by
hand — unlike `daemon.c`'s legacy path. That reasoning was never actually tested against the
`ffmpeg` backend; once it was, the bug disappeared. Read as: Intel's VA-API driver produces coded
output that both the hand-written legacy path *and* this component's own boundary scan handle
incorrectly, and libavcodec's encoder sidesteps whatever that Intel-specific quirk is in both
cases — a property of the driver's output, not of which piece of this project's own code consumes
it.

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

1. ~~The Intel SPS bug~~ **Resolved 09/11/2026**: use the `ffmpeg` encode backend on Intel, same
   as 5.14/5.16. `findLeadingParameterSetsSize` itself was never actually the bug.
2. Android 12/13 — assumed to work (see the note at the top), not separately validated on
   hardware. `VaapiDecComponent` (hwdec, the HIDL side) also hasn't been exercised at all yet.
   Neither is being chased proactively; a real issue report on either is what triggers picking it
   up.
