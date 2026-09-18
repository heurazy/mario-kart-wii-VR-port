# SteamVR intermittent black frames

The Pimax Dream Air report describes periodic stutter and a momentary black image
with a native SteamVR driver. SteamVR is already selected by this port at startup;
the report alone does not establish a Pimax-specific compatibility failure.
No log or reproduction from that headset was available for this investigation.

Two application-side blanking paths were found in the asynchronous display loop:

- It required a valid *current* located view to resubmit a completed image, even
  though the projection layer uses that image's original render poses. A transient
  invalid orientation therefore submitted zero layers unnecessarily.
- It compared the completed image with the live render-content tag. Incomplete
  scene/camera observations can temporarily change that tag without a real scene
  transition, suppressing an otherwise valid cached image.

The corrected loop requires valid current views and coherent observations for
new rendering, but reuses completed content with its original poses during those
gaps. A separate display tag retains the stable presentation generation. Real
settings, scene and session changes still reject obsolete images. SteamVR's
`shouldRender = false` request still submits no layers.

Every five seconds, the existing `async timing` log now also reports:

| Counter | Meaning |
| --- | --- |
| `runtime-hidden` | The OpenXR runtime requested no application rendering. |
| `invalid-views` | Rendering was requested but a fresh orientation was unavailable. |
| `policy-gaps` | Guest scene/camera observations were temporarily incoherent. |
| `cache-misses` | Rendering was requested but no compatible completed image was available. |
| `canceled` | A pending image was canceled after its submission deadline. |

The regression test covers incomplete observations, coherent recovery, settings
transitions and session isolation. The standalone VR suite passes all 12 tests;
both changed OpenXR translation units pass the Windows Clang syntax check.
This is not physical Pimax validation. To confirm the user's cause, obtain their
runtime log around a visible flicker, SteamVR version, headset refresh rate, GPU,
resolution, and whether it also happens in menus. Compare the counters before and
after the correction. Driver/runtime-hidden events or GPU stalls need separate
investigation if they remain.
