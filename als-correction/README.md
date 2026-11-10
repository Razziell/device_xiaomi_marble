# Content-aware ALS correction for marble

Poco F5 (`marble`) uses an ambient-light sensor below the OLED panel. The proprietary
SSC calibration compensates panel leakage using global backlight brightness (DBV), but
it does not know which pixels are lit above the sensor. Consequently, light content in
the sensor area can increase reported lux and automatic brightness.

`MarbleAlsCorrection` samples a small framebuffer region above the sensor, converts each
RGB primary to approximate linear panel emission, applies the measured TCS3701 spectral
response and subtracts the estimated residual panel leakage before the value is consumed
by `AutomaticBrightnessController`. While auto-brightness keeps the correction running,
`DisplayModeDirector` (the low-brightness refresh-rate zones) receives the corrected lux too.

## Integration

The implementation is built as `marble-als-correction.jar`, installed in
`/system_ext/framework`, and added to `SYSTEMSERVERCLASSPATH` by `device.mk`.

The framework side contains only a generic optional plugin hook,
`com.android.server.display.AmbientLightCorrection`, and its use in
`AutomaticBrightnessController` and `DisplayModeDirector`. The exact patch is stored in
[`frameworks-base.patch`](frameworks-base.patch). Apply it from the Android source root with:

```sh
git -C frameworks/base apply \
    "$(pwd)/device/xiaomi/marble/als-correction/frameworks-base.patch"
```

`AmbientLightCorrection` loads the class named by `ro.vendor.display.als_correction_class`
once per `system_server` and shares that one instance between all users of the light sensor.
The contract of the class is:

| Contract | Required | Meaning |
|---|---|---|
| `DoubleUnaryOperator` | yes | Raw lux in, corrected lux out. Called on any thread. |
| `Consumer<Boolean>` | no | `accept(true)` before the first client that needs the correction registers its light-sensor listener, `accept(false)` once the last one has unregistered. |
| Public constructor taking a `Runnable` | no | The correction runs it when its answer for the latest raw reading has changed. Every listening client then passes its latest raw reading through the operator again. |

Finite, non-negative light-sensor events are passed through the operator. The framework
validates its result and falls back to the original lux if correction returns an invalid
value or throws a runtime/linkage exception.

`AutomaticBrightnessController` is the client that needs the correction: its listener starts
and stops the captures. This follows the listener itself, so one-shot auto-brightness, which
unregisters the listener while the sensor stays logically enabled, stops them as well.
`DisplayModeDirector` only uses the correction while it is running. With manual brightness
its zones still see raw lux, so that no screen is captured while auto-brightness is off.

Each transition invalidates the content cache and in-flight generation. Enable schedules an
immediate background prewarm; disable cancels queued refreshes. No final sensor event with
DBV=0 is needed to discard pre-sleep content. Install the updated framework hook together
with the JAR: this implementation needs the enable callback, and without the `Runnable` it
cannot correct a reading again (see "Re-evaluation" below).

If the property, JAR or implementation is missing, readings pass through unchanged and stock
automatic brightness behavior is preserved for valid sensor events. Implementations that
only implement `DoubleUnaryOperator` remain supported without lifecycle notifications.

## Runtime parameters

All calibration parameters are persistent system properties and are read at runtime.
They do not require a reboot or framework restart. A changed value normally takes effect
on the next ALS event or periodic framebuffer refresh. The exception is re-enabling: a
disabled corrector does not poll, so it resumes only when `system_server` is told that a
property changed (XiaomiParts does this, see below) or when the next ALS event arrives.

| Property | Default | Clamped to | Meaning |
|---|---:|---:|---|
| `persist.sys.als_correction.enabled` | `true` | — | Enables or bypasses correction. |
| `persist.sys.als_correction.k` | `550` | `0..1500` | Leakage in lux for a fully lit weighted RGB sample at maximum DBV. Larger values apply stronger correction. |
| `persist.sys.als_correction.ref_luma` | `0.14` | `0..1` | Weighted content level below which no leakage is subtracted. |
| `persist.sys.als_correction.gamma` | `2.2` | `1..4` | Gamma used to convert each captured RGB primary to approximate panel emission. |
| `persist.sys.als_correction.cx` | `745` | `0..1079` | Sensor-area center X in natural panel coordinates. |
| `persist.sys.als_correction.cy` | `51` | `0..2399` | Sensor-area center Y in natural panel coordinates. |
| `persist.sys.als_correction.r` | `47` | `16..96` | Capture radius in natural panel pixels. |
| `persist.sys.als_correction.interval_ms` | `500` | `100..5000` | Delay after a completed successful capture before refreshing again, in milliseconds. |

Out-of-range values are clamped inside the corrector itself, so unvalidated writes via
adb or a settings UI degrade gracefully instead of breaking auto-brightness. Malformed
values (non-numeric strings, `NaN` or infinities) silently fall back to the compiled defaults.
The settings UI also rejects non-finite values when reading overrides. Note that the
float properties must use a dot as the decimal separator (`0.14`, not `0,14`).

Recommended user-facing UI ranges are narrower than the hard clamps:
`k` 400–900, `ref_luma` 0.05–0.30, `gamma` 1.8–2.6, `interval_ms` one of 250/500 (Fast/Balanced; default 500).

The corrector also reads two properties that it does not own, to know SurfaceFlinger's global
saturation: `sys.parts.sf_saturation`, which the XiaomiParts saturation control writes each
time it applies a value, and the framework's `persist.sys.sf.color_saturation` when that one
is unset.

### State property

The corrector publishes its current operating state (written only on change) in a
read-only property intended for settings UIs:

```properties
sys.als_correction.state
```

| Value | Meaning |
|---|---|
| `active` | A valid, fresh content capture is ready for correction. |
| `capture_failed` | The last capture attempt actually failed (for example, secure content or a SurfaceFlinger error); lux is passed through unmodified. |
| `idle` | The ALS listener is inactive or panel DBV is zero; no new capture is requested. |
| `off` | Disabled via `persist.sys.als_correction.enabled`. |

An empty value means fresh correction data is not available yet: before the first capture,
after cache expiry or a very slow capture, or when DBV cannot be read. It does not indicate
a capture failure. Lifecycle and refresh callbacks update the state; changes to persistent
settings are observed on the next ALS event, refresh or property-change notification. The
state is not a display-power API.
Normal successful periodic captures keep `active` even if TYPE_LIGHT remains unchanged.

The implementation class is selected at boot by the read-only product property:

```properties
ro.vendor.display.als_correction_class=com.android.server.display.MarbleAlsCorrection
```

### Commands

On a rooted user build:

```sh
# Inspect current overrides (an empty result means that the compiled default is used)
adb shell su -c 'getprop | grep als_correction'

# Disable the correction (seen by the next refresh, at most one interval later)
adb shell su -c 'setprop persist.sys.als_correction.enabled false'

# Enable it again. setprop alone is picked up only by the next ALS event, so also tell
# system_server that a property changed (SYSPROPS_TRANSACTION, as XiaomiParts does)
adb shell su -c 'setprop persist.sys.als_correction.enabled true'
adb shell su -c 'service call display 1599295570'

# Make correction stronger or weaker
adb shell su -c 'setprop persist.sys.als_correction.k 700'
adb shell su -c 'setprop persist.sys.als_correction.k 600'

# Reduce capture rate to at most 2 Hz to favor power consumption
adb shell su -c 'setprop persist.sys.als_correction.interval_ms 500'

# Check whether the correction is actually running
adb shell getprop sys.als_correction.state

# Restore all compiled defaults (an empty value means "use the compiled default")
adb shell su -c 'setprop persist.sys.als_correction.enabled true'
adb shell su -c 'setprop persist.sys.als_correction.k ""'
adb shell su -c 'setprop persist.sys.als_correction.ref_luma ""'
adb shell su -c 'setprop persist.sys.als_correction.gamma ""'
adb shell su -c 'setprop persist.sys.als_correction.cx ""'
adb shell su -c 'setprop persist.sys.als_correction.cy ""'
adb shell su -c 'setprop persist.sys.als_correction.r ""'
adb shell su -c 'setprop persist.sys.als_correction.interval_ms ""'
```

`adb root` can be used instead of `su -c` on builds where adbd root is allowed. Ordinary
unprivileged applications and shell users cannot normally change `persist.sys.*`.

### Calibration guidance

* Increase `k` if white content still raises automatic brightness relative to dark content.
* Decrease `k` if white content causes corrected lux to fall below the dark-content value.
* Increase `ref_luma` if status-bar icons or a dark UI are being over-corrected.
* Do not change `cx`, `cy` or `r` unless panel coordinates or the sampled sensor area are known
  to be wrong.
* The default `k` of 550 was lowered from the measured range to avoid over-subtracting under
  mixed content. The measurements do not agree on one value: white/black at DBV
  250/500/1000/2000 gives `k` = 620..656; white/black at DBV 4095 (497 vs 181 lux) gives 548
  if the same framebuffer delta-luma of 0.5763 applies to it; the red + green + blue sum at
  DBV 4095 is 752 lux; twelve single light/dark theme transitions at DBV 123-252, taken under
  room light that was not held constant, give 420..630 with a median of about 520. By the
  first set the default leaves 11-16% of the white-content leakage uncorrected up to DBV 2000
  and about 640 matches it; by the last one 550 is about right at low brightness. Measuring
  all of them again in one session would settle which one describes the panel.
* Change one parameter at a time under stable external lighting and compare white/dark content
  at the same fixed DBV.

The spectral weights were derived from automated black/red/green/blue/white measurements
at DBV 632, 1276, 2243, 3209 and 4095. At maximum DBV, the measured residual `TYPE_LIGHT`
contributions were 136.13 lux red, 544.17 lux green and 71.95 lux blue. Their normalized
weights are:

```text
R = 0.1810, G = 0.7234, B = 0.0956
```

They sum to 1, so the existing white/gray calibration and the user-facing `k` control are
preserved. Compared with Rec.709 human-vision weights, this reduces red over-correction and
increases blue correction without changing the number or frequency of screen captures.

The dark-UI baseline (`ref_luma`) is subtracted per linearized primary before spectral
weighting, and only pixels inside the inscribed circular sensor disc of the `16 x 16` sample
grid are averaged (using a cached 256-entry gamma LUT). If `cx`, `cy` and `r` put part of the
disc outside the panel, the capture is cut off at the edge and the mask follows the real disc
position inside that smaller capture. Because the RGB weights sum to 1,
neutral white/gray content and dark UIs where all primaries are below `ref_luma` behave
identically to a post-sum threshold, while saturated blue (`#0000FF`) and red (`#FF0000`)
content above the sensor are compensated proportionally to their measured TCS3701 response.

SurfaceFlinger applies the display color transform to the panel but renders screenshots
without it. The corrector therefore rebuilds that transform and applies it to every sampled
pixel before averaging: all matrices held by `DisplayTransformManager` (night light, display
white balance, saturation, grayscale, extra dim, color inversion, LineageOS calibration and
reading mode), composed in ascending level order, times SurfaceFlinger's global saturation
matrix. It is applied in linear light when `DisplayTransformManager.needsLinearColorMatrix()`
says so, and to the gamma-encoded values otherwise. With color inversion a white framebuffer
is a black panel; without this step the correction subtracted leakage that was not there.
The daltonizer of the color-correction accessibility modes is known only inside
SurfaceFlinger and is not modelled; it leaves neutral colors unchanged.

The correction formula is approximately:

```text
meanR/G/B = discMean(colorTransform(pow(capturedR/G/B / 255, gamma)))
excessContent = 0.1810 * max(0, meanR - refLuma)
    + 0.7234 * max(0, meanG - refLuma)
    + 0.0956 * max(0, meanB - refLuma)
correctedLux = max(0, rawLux
    - k * excessContent * currentDbv / maximumDbv)
```

## Capture safety

`ScreenCaptureInternal.captureDisplay()` runs asynchronously on a dedicated background thread.
The display-power/ALS callback never waits for SurfaceFlinger; for a new reading it only posts
a capture request to that thread.
The worker requires the default display to be `STATE_ON` (not OFF or doze), readable positive
DBV, and the enable property before capture, and checks them again before accepting the result. Unknown DBV/max DBV
passes through the original lux; it is never interpreted as maximum brightness. The maximum
DBV read is retried until it succeeds. Small sysfs reads still run on the ALS callback looper.

Both ALS lifecycle transitions discard cached content and invalidate in-flight results. A
queued obsolete request is skipped, and at most one capture per corrector may be pending.
Captures have a hard maximum age of **2000 ms**, measured from the start of capture using
`elapsedRealtime()` (which includes suspend). Old content is never used while waiting for
an update, and a capture taking 2000 ms or longer is not accepted as fresh data. Expiry is
also published independently of both ALS delivery and the capture worker. With intervals above
2000 ms there can deliberately be gaps with no usable correction data; the recommended UI
intervals are 250/500 ms (Fast/Balanced). XiaomiParts migrates the retired 1000 ms preset to
500 ms at boot and once each time its settings screen is opened, persisting the new value.
Other custom intervals remain effective and are shown as custom values until a preset or reset
is selected. An ADB-set 1000 ms is migrated at those same two points.

### Refresh independent of on-change ALS

Device measurements showed gaps of tens of seconds between TYPE_LIGHT events despite a
500 ms requested sensor period. An event-driven capture with a 2-second TTL therefore left
the first lux after every long pause uncorrected, even while the screen remained on.

The worker now refreshes periodically while the ALS listener is enabled. The first request
is scheduled at listener enable, and each completed request schedules at most one successor.
Successful requests wait `interval_ms` after completion: no overlapping captures, catch-up
bursts, or queue growth if SurfaceFlinger is slow. Periodic captures only update the content
cache. An ALS event consumes the latest fresh content with the current DBV.

### Re-evaluation of a reading corrected with outdated content

TYPE_LIGHT is on-change, and what changes the reading is often the screen above the sensor.
Such a reading can arrive before any capture has seen the new content. It is then corrected
with the previous content: after white -> dark at DBV 2000 a true 100 lux became 0 and stayed
0 until the light changed, because the sensor had nothing new to report.

Three things prevent that:

* Every new reading posts a capture, so the screen is sampled as it is right after the
  reading. The same value delivered to a second listener does not count as new.
* The worker tracks the leakage estimate, which covers content and DBV together. When it
  changes by more than max(1 lux, 5%), a new run starts.
* Once a run has held for 600 ms, long enough for a theme or window transition to finish, a
  reading that arrived after the last capture of the previous run is checked. If correcting
  it with the current estimate changes the result by more than max(1 lux, 5%), the corrector
  runs the framework's `Runnable` and each listener passes its latest raw lux through the
  corrector again. A capture is scheduled to land on the 600 ms mark.

The corrected value reaches `AutomaticBrightnessController` as one more sample, at most
600 ms plus three capture durations after the reading. That is inside the 1000 ms brightening
debounce of the sm8450 overlay, so a sample that was too bright is replaced before it can
raise the brightness. A reading that predates the last capture of the previous content is
left alone: it was corrected consistently. The sensor reports a changed leakage anywhere from
0.2 s to several seconds after the screen changed, and at low brightness sometimes not within
7 s; whenever it does, that reading is corrected with the content cached by then.

Measured on marble (light/dark Settings toggled nine times at DBV 252 under about 128 lux):
raw lux alternated between about 128 and 147, corrected lux stayed within 123.6-128.1 and
auto-brightness did not move. In five of the nine transitions the reading arrived before a
capture had seen the new content and came out as 147 or 108; each was replaced by the right
value 0.4-0.65 s later.

This is the only case in which an old raw lux is used again. Periodic captures on their own
still never create samples or force brightness updates.

OFF/doze, unavailable DBV and capture failures use a retry delay of
`max(interval_ms, 1000)` ms. The listener is enabled before the panel is on, so for the first
2 s after enable a panel that is not on yet is checked every 100 ms instead; content is then
ready for the first reading after wake. Disabled correction (`enabled=false`) stops the background
refresh timer completely (zero periodic wakeups) and resumes immediately on a sysprop change
notification from XiaomiParts or on the next ALS event. `SystemProperties.reportSyspropChanged()`
only reaches callbacks inside the calling process, so XiaomiParts sends `SYSPROPS_TRANSACTION`
to the `display` service binder after writing the enable property; that runs the change
callbacks inside `system_server`. A plain `setprop` sends nothing, and the ALS event that
finally wakes the corrector is itself passed through uncorrected.
When the ALS listener is disabled, **all queued refreshes are cancelled**: no polling,
capture or wake lock is kept for an inactive listener. An already running capture cannot
be cancelled, but its old-generation result is discarded. Re-enabling during that capture
schedules a fresh prewarm as soon as it finishes.

The first ALS event immediately after wake can still precede the first successful capture;
it passes through rather than using pre-sleep content or blocking the display, and is
corrected again through the re-evaluation above. A capture also cannot undo redaction,
account for panel-side processing after SurfaceFlinger's color transform, or guarantee
pixel/sensor timestamp alignment. This is a bounded-delay compensation model.

Secure and protected-content policies explicitly reject the capture, rather than treating
redacted black pixels as actual panel emission. Protected pixels are never requested.

The capture crop is transformed from natural panel coordinates into the current layer-stack
rotation (the same way UDFPS transforms its sensor bounds), so landscape content is sampled at
the physical sensor location. The screenshot `HardwareBuffer` (a dmabuf file descriptor inside
`system_server`) is closed deterministically after each capture instead of waiting for GC, and
state decisions are made under the same lock as generation checks. Property writes are coalesced
on a separate background handler: the DisplayPowerController looper never performs a
property-service round-trip, and state updates are not queued behind a stalled capture.

## Performance and power impact

The arithmetic itself is small: each sample processes only a `16 x 16` bitmap (256
pixels); DBV is read on the ALS callback and around each capture. The main cost is the synchronous
`ScreenCaptureInternal.captureDisplay()` request to SurfaceFlinger.

At defaults:

* with the default `interval_ms=500`, periodic capture is limited to at most **2 times per
  second**; actual frequency is lower because the delay starts after capture completes;
* each new sensor reading adds one capture, and a change of the leakage estimate adds one on
  the 600 ms mark instead of at the regular interval;
* only a roughly `94 x 94` source region is requested and downscaled to `16 x 16`;
* new capture requests run only while the ALS listener is enabled; an already running capture
  may finish after disable, but its result is discarded;
* a disabled correction bypasses DBV reads and stops background capture wakeups completely;
* capture failure or secure/unavailable content safely falls back to unmodified lux.

Periodic sampling intentionally performs more captures under stable light than the old
ALS-event-driven implementation. Its power cost must be measured, not assumed unchanged.
CPU arithmetic and temporary Java memory are small; SurfaceFlinger/GPU synchronization dominates. Exact power cost depends on composer behavior,
refresh rate and panel state, so it should be measured rather than inferred from source alone.
The current implementation has not been assigned a laboratory mW figure.

For a lower-power compromise use `interval_ms=500` (maximum 2 captures/s) or `1000`
(maximum 1 capture/s). This makes correction respond more slowly to content changes. Setting
`enabled=false` removes capture and DBV work after any already running capture finishes;
only lightweight validation/property checks remain. Disabling the ALS listener stops the
refresh timer entirely.

## Diagnostics and validation

The framework dump prints the corrector snapshot without extra reflection or log spam:

```sh
adb shell dumpsys display | grep 'mAlsCorrection='
```

Important fields:

* `enableCount`, `disableCount`, `lastEnableElapsedMs`, `lastDisableElapsedMs`: listener
  lifecycle history since this corrector instance was created; timestamps include suspend.
* `generation`: invalidation generation. Old-generation results are never used.
* `capturePending`: a refresh is in progress (including its availability checks).
* `refreshScheduled`: a refresh is queued. After listener disable this must be false.
* `cacheAgeMs`, `cacheValid`, `contentLevel`: the current sample; age -1 means absent.
* `captures`, `successes`, `failures`, `discarded`, `expired`: completed attempt counters,
  accepted successes, capture failures, unusable/obsolete results and expired cached samples.
* `lastCaptureAgeMs`, `lastCaptureDurationMs`: time since completion and processing duration
  of the last attempted capture, including its availability checks.
* `alsSamples`, `lastAlsAgeMs`, `lastRawLux`, `lastCorrectedLux`: actual sensor callbacks of
  all listeners (`AutomaticBrightnessController` and `DisplayModeDirector` each deliver the
  same reading), without re-evaluations.
* `reevaluateRequests`, `reevaluated`: how often the corrector asked the framework to pass the
  latest raw lux through again, and how many such samples came back. Requests should be rare
  and happen right after content changes; a count that grows under a static screen is not
  expected.
* `runLeakage`, `runAgeMs`: the leakage estimate in lux of the current run and how long it has
  held; NaN and -1 without a usable capture.
* `colorTransform`: a display color transform other than identity was applied to the cached
  sample.
* `cacheMisses`: sensor callbacks passed through uncorrected only because no fresh content was
  cached (correction enabled, listener active, DBV readable). TYPE_LIGHT is on-change, so such
  a lux value stays in effect until the sensor reports again. An occasional miss right after
  the listener is enabled is expected; a count that keeps growing while the screen stays on
  is not.
* `lastUsedCacheAgeMs`: age of content used for the last ALS callback; -1 means passthrough
  without a usable sample. A valid sample can still subtract zero below the reference threshold.

With a stable scene and no ALS events, `successes` should continue increasing, the cache
should remain fresh at recommended intervals, and `alsSamples` should remain unchanged.
After disabling the screen/ALS, `disableCount` should increment, cache should be invalid,
and captures should stop (apart from one already in flight). On re-enable, `enableCount`
increments and a new capture is scheduled. Compare snapshots around the transition; a
post-wake dump alone is not proof that no capture happened during the entire sleep interval.

These fields do not validate the RGB calibration or measure power consumption. Avoid
changing sensor/brightness settings merely to query them.

Additional validation commands (the `setprop` examples modify settings):

```sh
adb shell dumpsys display | grep -i -E 'ambient|lux|brightness'
adb shell su -c 'setprop persist.sys.als_correction.enabled false'
# Compare system_server / SurfaceFlinger CPU usage, then enable it again:
adb shell top -H -p "$(adb shell pidof system_server | tr -d '\r')"
adb shell su -c 'setprop persist.sys.als_correction.enabled true'
```

For meaningful energy measurements, compare equal-duration white/dark scripted workloads with
fixed display brightness using Batterystats or external power instrumentation. Battery percentage
alone is not precise enough for this small overhead.
