# Content-aware ALS correction for marble

Poco F5 (`marble`) uses an ambient-light sensor below the OLED panel. The proprietary
SSC calibration compensates panel leakage using global backlight brightness (DBV), but
it does not know which pixels are lit above the sensor. Consequently, light content in
the sensor area can increase reported lux and automatic brightness.

`MarbleAlsCorrection` samples a small framebuffer region above the sensor, converts each
RGB primary to approximate linear panel emission, applies the measured TCS3701 spectral
response and subtracts the estimated residual panel leakage before the value is consumed
by `AutomaticBrightnessController`.

## Integration

The implementation is built as `marble-als-correction.jar`, installed in
`/system_ext/framework`, and added to `SYSTEMSERVERCLASSPATH` by `device.mk`.

The framework side deliberately contains only a generic optional plugin hook. The exact
patch is stored in [`frameworks-base.patch`](frameworks-base.patch). Apply it from the
Android source root with:

```sh
git -C frameworks/base apply \
    "$(pwd)/device/xiaomi/marble/als-correction/frameworks-base.patch"
```

For reference, the functional part of the patch is:

```java
private final DoubleUnaryOperator mAlsCorrection = loadAlsCorrection();

private static DoubleUnaryOperator loadAlsCorrection() {
    final String className = SystemProperties.get(
            "ro.vendor.display.als_correction_class");
    if (className.isEmpty()) {
        return value -> value;
    }

    try {
        return (DoubleUnaryOperator) Class.forName(className)
                .getDeclaredConstructor().newInstance();
    } catch (ReflectiveOperationException | ClassCastException | LinkageError e) {
        Slog.e(TAG, "Unable to load ALS correction: " + className, e);
        return value -> value;
    }
}
```

Finite, non-negative light-sensor events are passed through the operator. The framework
validates its result and falls back to the original lux if correction returns an invalid
value or throws a runtime/linkage exception.

The implementation also implements `Consumer<Boolean>`. The optional framework lifecycle
hook calls `accept(true)` before registering the ALS listener and `accept(false)` when
unregistering it. Each transition invalidates the content cache and in-flight generation.
Enable schedules an immediate background prewarm; disable cancels queued refreshes. No final
sensor event with DBV=0 is needed to discard pre-sleep content. Install the
updated framework hook together with the JAR; this implementation needs the enable callback.

If the property, JAR or implementation is missing, the operator is a no-op and stock
automatic brightness behavior is preserved for valid sensor events. Implementations that
only implement `DoubleUnaryOperator` remain supported without lifecycle notifications.

## Runtime parameters

All calibration parameters are persistent system properties and are read at runtime.
They do not require a reboot or framework restart. A changed value normally takes effect
on the next ALS event or periodic framebuffer refresh. When correction is disabled but ALS
is still enabled, a lightweight worker check detects re-enabling without needing a new ALS event.

| Property | Default | Clamped to | Meaning |
|---|---:|---:|---|
| `persist.sys.als_correction.enabled` | `true` | — | Enables or bypasses correction. |
| `persist.sys.als_correction.k` | `650` | `0..1500` | Leakage in lux for a fully lit weighted RGB sample at maximum DBV. Larger values apply stronger correction. |
| `persist.sys.als_correction.ref_luma` | `0.14` | `0..1` | Weighted content level below which no leakage is subtracted. |
| `persist.sys.als_correction.gamma` | `2.2` | `1..4` | Gamma used to convert each captured RGB primary to approximate panel emission. |
| `persist.sys.als_correction.cx` | `745` | `0..1079` | Sensor-area center X in natural panel coordinates. |
| `persist.sys.als_correction.cy` | `51` | `0..2399` | Sensor-area center Y in natural panel coordinates. |
| `persist.sys.als_correction.r` | `47` | `16..96` | Capture radius in natural panel pixels. |
| `persist.sys.als_correction.interval_ms` | `250` | `100..5000` | Delay after a completed successful capture before refreshing again, in milliseconds. |

Out-of-range values are clamped inside the corrector itself, so unvalidated writes via
adb or a settings UI degrade gracefully instead of breaking auto-brightness. Malformed
values (non-numeric strings, `NaN` or infinities) silently fall back to the compiled defaults.
The settings UI also rejects non-finite values when reading overrides. Note that the
float properties must use a dot as the decimal separator (`0.14`, not `0,14`).

Recommended user-facing UI ranges are narrower than the hard clamps:
`k` 400–900, `ref_luma` 0.05–0.30, `gamma` 1.8–2.6, `interval_ms` one of 250/500/1000.

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
settings are observed on the next ALS event or refresh. The state is not a display-power API.
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

# Disable/enable the correction immediately
adb shell su -c 'setprop persist.sys.als_correction.enabled false'
adb shell su -c 'setprop persist.sys.als_correction.enabled true'

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

The model remains deliberately conservative: with the default `ref_luma=0.14`, a pure blue
sample (`contentLevel=0.0956`) is below the subtraction threshold. These weights do not by
themselves guarantee complete cancellation for every color. Changing this baseline or using
a circular/spatially weighted sample requires a separate calibration.

The correction formula is approximately:

```text
linearR/G/B = pow(capturedR/G/B / 255, gamma)
contentLevel = 0.1810 * linearR + 0.7234 * linearG + 0.0956 * linearB
correctedLux = max(0, rawLux
    - k * max(0, contentLevel - refLuma) * currentDbv / maximumDbv)
```

## Capture safety

`ScreenCaptureInternal.captureDisplay()` runs asynchronously on a dedicated background thread.
The display-power/ALS callback never waits for SurfaceFlinger and does not schedule captures.
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
intervals are 250/500/1000 ms.

### Refresh independent of on-change ALS

Device measurements showed gaps of tens of seconds between TYPE_LIGHT events despite a
500 ms requested sensor period. An event-driven capture with a 2-second TTL therefore left
the first lux after every long pause uncorrected, even while the screen remained on.

The worker now refreshes periodically while the ALS listener is enabled. The first request
is scheduled at listener enable, and each completed request schedules at most one successor.
Successful requests wait `interval_ms` after completion: no overlapping captures, catch-up
bursts, or queue growth if SurfaceFlinger is slow. Captures only update the content cache;
they never inject synthetic sensor events, reuse old lux, or force brightness updates.
The next real ALS event consumes the latest fresh content with the current DBV.

Disabled correction, OFF/doze, unavailable DBV and capture failures use a retry delay of
`max(interval_ms, 1000)` ms. Disabled correction checks only its enable property (no display,
sysfs or capture work). This allows live re-enabling to recover even with no TYPE_LIGHT event.
When the ALS listener is disabled, **all queued refreshes are cancelled**: no polling,
capture or wake lock is kept for an inactive listener. An already running capture cannot
be cancelled, but its old-generation result is discarded. Re-enabling during that capture
schedules a fresh prewarm as soon as it finishes.

The first ALS event immediately after wake can still precede the first successful capture;
it intentionally passes through rather than using pre-sleep content or blocking the display.
A capture also cannot undo redaction, account for every post-composition panel transform,
or guarantee pixel/sensor timestamp alignment. This is a bounded-delay compensation model.

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

* with the default `interval_ms=250`, capture is limited to at most **4 times per second**;
* only a roughly `94 x 94` source region is requested and downscaled to `16 x 16`;
* new capture requests run only while the ALS listener is enabled; an already running capture
  may finish after disable, but its result is discarded;
* a disabled correction bypasses DBV reads and captures; while ALS is enabled it retains
  only a low-rate property check to notice live re-enabling;
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
* `alsSamples`, `lastAlsAgeMs`, `lastRawLux`, `lastCorrectedLux`: actual sensor callbacks.
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
