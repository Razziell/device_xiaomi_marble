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

The light-sensor event is then passed through the operator:

```java
final float lux = (float) mAlsCorrection.applyAsDouble(event.values[0]);
handleLightSensorEvent(time, lux);
```

If the property, JAR or implementation is missing, the operator is a no-op and stock
automatic brightness behavior is preserved.

## Runtime parameters

All calibration parameters are persistent system properties and are read at runtime.
They do not require a reboot or framework restart. A changed value normally takes effect
on the next ALS event or framebuffer sample.

| Property | Default | Clamped to | Meaning |
|---|---:|---:|---|
| `persist.sys.als_correction.enabled` | `true` | — | Enables or bypasses correction. |
| `persist.sys.als_correction.k` | `650` | `0..1500` | Leakage in lux for a fully lit weighted RGB sample at maximum DBV. Larger values apply stronger correction. |
| `persist.sys.als_correction.ref_luma` | `0.14` | `0..1` | Weighted content level below which no leakage is subtracted. |
| `persist.sys.als_correction.gamma` | `2.2` | `1..4` | Gamma used to convert each captured RGB primary to approximate panel emission. |
| `persist.sys.als_correction.cx` | `745` | `0..1079` | Sensor-area center X in natural panel coordinates. |
| `persist.sys.als_correction.cy` | `51` | `0..2399` | Sensor-area center Y in natural panel coordinates. |
| `persist.sys.als_correction.r` | `47` | `16..96` | Capture radius in natural panel pixels. |
| `persist.sys.als_correction.interval_ms` | `250` | `100..5000` | Minimum time between captures, in milliseconds. |

Out-of-range values are clamped inside the corrector itself, so unvalidated writes via
adb or a settings UI degrade gracefully instead of breaking auto-brightness. Malformed
values (non-numeric strings) silently fall back to the compiled defaults. Note that the
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
| `active` | A valid content capture is ready for correction. |
| `capture_failed` | The last capture attempt actually failed (for example, secure content or a SurfaceFlinger error); lux is passed through unmodified. |
| `idle` | The panel is off, so no screen capture is attempted. |
| `off` | Disabled via `persist.sys.als_correction.enabled`. |

An empty value means the corrector has not processed any ALS event since boot
(for example, auto-brightness has never been enabled).

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

The correction formula is approximately:

```text
linearR/G/B = pow(capturedR/G/B / 255, gamma)
contentLevel = 0.1810 * linearR + 0.7234 * linearG + 0.0956 * linearB
correctedLux = max(0, rawLux
    - k * max(0, contentLevel - refLuma) * currentDbv / maximumDbv)
```

## Capture safety

`ScreenCaptureInternal.captureDisplay()` runs asynchronously on a dedicated background thread.
The display-power/ALS callback never waits for SurfaceFlinger, and no capture is requested while
panel DBV is zero. Cached content is discarded when the panel turns off.

The capture crop is transformed from natural panel coordinates into the current layer-stack
rotation (the same way UDFPS transforms its sensor bounds), so landscape content is sampled at
the physical sensor location. The screenshot `HardwareBuffer` (a dmabuf file descriptor inside
`system_server`) is closed deterministically after each capture instead of waiting for GC, and
state-property writes are posted to the capture thread so the DisplayPowerController looper never
performs a property-service round-trip.

## Performance and power impact

The arithmetic itself is negligible: each sample processes only a `16 x 16` bitmap (256
pixels), followed by a short sysfs read. The main cost is the synchronous
`ScreenCaptureInternal.captureDisplay()` request to SurfaceFlinger.

At defaults:

* capture is limited to at most **4 times per second** (`interval_ms=250`);
* only a roughly `94 x 94` source region is requested and downscaled to `16 x 16`;
* work runs only when `AutomaticBrightnessController` has enabled its light-sensor listener;
* disabling correction returns before framebuffer capture and DBV reads;
* capture failure or secure/unavailable content safely falls back to unmodified lux.

The expected impact is small but not zero. CPU work and temporary Java memory are tiny;
SurfaceFlinger/GPU synchronization dominates. Exact power cost depends on composer behavior,
refresh rate and panel state, so it should be measured rather than inferred from source alone.
The current implementation has not been assigned a laboratory mW figure.

For a lower-power compromise use `interval_ms=500` (maximum 2 captures/s) or `1000`
(maximum 1 capture/s). This makes correction respond more slowly to content changes. Setting
`enabled=false` removes virtually all runtime overhead except one property lookup per ALS event.

Useful validation commands:

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
