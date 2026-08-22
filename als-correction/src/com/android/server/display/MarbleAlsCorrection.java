/*
 * Copyright (C) 2026 The LineageOS Project
 *
 * Device-specific content-aware ambient light sensor correction for marble.
 *
 * Background (marble / Poco F5, TCS3701 under OLED):
 *   The proprietary SSC algorithm (libssccalapi@2.0.so, class Lux_Ams_Tcs3701_Boled)
 *   compensates panel light leakage using ONLY the global panel brightness
 *   (SNS_PHYSICAL_SENSOR_CONFIG_BACKLIGHT). Its sensorSendnotify() accepts just two
 *   notify types (0x11 BRIGHTNESS, 0xC9 REPORT_VALUE); DC_STATE, DISPLAY_FREQ,
 *   POWER_STATE and BRIGHTNESS_PARSE_REULT_RGB are rejected with "not support msg type".
 *
 *   Because the model does not know WHICH pixels are lit above the sensor, a white
 *   UI leaks far more light than the calibrated average, and a dark UI far less.
 *   Measured on marble at fixed DBV=4095: raw ALS 497 on white vs 181 on black.
 *
 *   This class samples the actual framebuffer content in the small disc above the
 *   sensor (position taken from vendor lightSensorConfig.json "cwbInfo") and removes
 *   the residual, content dependent part of the leakage.
 *
 * All tunables are live-adjustable via system properties so the correction can be
 * calibrated without rebuilding:
 *   persist.sys.als_correction.enabled   (bool, default true)
 *   persist.sys.als_correction.k         (int, content leakage at luma=1, dbv=1)
 *   persist.sys.als_correction.ref_luma  (float, no-correction dark-content baseline)
 *   persist.sys.als_correction.gamma     (float, display EOTF, default 2.2)
 *   persist.sys.als_correction.cx/cy/r   (int, sensor disc in natural panel pixels)
 *   persist.sys.als_correction.interval_ms (int, capture interval, default 250 ms)
 *
 * All tunables are clamped to sane hardware limits on read, so out-of-range values
 * written via adb/UI degrade gracefully instead of breaking auto-brightness:
 *   k 0..1500, ref_luma 0..1, gamma 1..4, interval 100..5000 ms,
 *   cx 0..1079, cy 0..2399, r 16..96 (natural panel 1080x2400).
 *
 * The current operating state is published (on change only) in the read-only
 * property "sys.als_correction.state" for consumption by settings UIs:
 *   active         - correction is being applied
 *   capture_failed - screen capture unavailable, lux passed through
 *   off            - disabled via persist.sys.als_correction.enabled
 */

package com.android.server.display;

import android.graphics.Bitmap;
import android.graphics.Rect;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Process;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.util.Slog;
import android.window.ScreenCaptureInternal;

import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.util.function.DoubleUnaryOperator;

/**
 * Removes display self-illumination from under-display ALS readings by measuring the
 * content actually displayed above the sensor.
 */
public final class MarbleAlsCorrection implements DoubleUnaryOperator {
    private static final String TAG = "MarbleAlsCorrection";
    private static final boolean DEBUG = false;

    /** Sensor disc, natural panel coordinates. From vendor cwbInfo: Center 745,51 R 18/34/47. */
    private static final int DEF_CENTER_X = 745;
    private static final int DEF_CENTER_Y = 51;
    private static final int DEF_RADIUS = 47;

    /**
     * Lux subtracted for a fully white disc at maximum panel brightness.
     * Derived from fixed-DBV measurements on marble.
     */
    // Measured on the connected marble across DBV 250/500/1000/2000:
    // delta(white-black) = 21.96/43.63/88.41/184.61 lux. Together with the measured
    // framebuffer delta-luma=0.5763 this gives k=625..656; use the rounded mean.
    private static final int DEF_K = 650;

    // Settings in dark mode is not pixel-black around the sensor because of status-bar icons.
    // Using that content as the baseline preserves the vendor algorithm's dark-content output
    // while removing only the additional leakage introduced by brighter content.
    private static final float DEF_REFERENCE_LUMA = 0.14f;

    private static final float DEF_GAMMA = 2.2f;

    /** Natural panel size of the marble display, for cx/cy/r validation. */
    private static final int PANEL_WIDTH = 1080;
    private static final int PANEL_HEIGHT = 2400;

    /** Hard limits applied to live-tunable properties. */
    private static final int K_MIN = 0;
    private static final int K_MAX = 1500;
    private static final float REF_LUMA_MIN = 0f;
    private static final float REF_LUMA_MAX = 1f;
    private static final float GAMMA_MIN = 1f;
    private static final float GAMMA_MAX = 4f;
    private static final int INTERVAL_MIN_MS = 100;
    private static final int INTERVAL_MAX_MS = 5000;
    private static final int RADIUS_MIN = 16;
    private static final int RADIUS_MAX = 96;

    /** Read-only state published for settings UIs. Written only on change. */
    private static final String STATE_PROP = "sys.als_correction.state";
    private static final String STATE_ACTIVE = "active";
    private static final String STATE_CAPTURE_FAILED = "capture_failed";
    private static final String STATE_OFF = "off";

    /** Downscaled capture edge, in pixels. 16x16 = 256 samples, plenty for an average. */
    private static final int SAMPLE_EDGE = 16;

    /** Do not re-capture more often than this. The ALS itself reports at ~5 Hz. */
    private static final long MIN_CAPTURE_INTERVAL_MS = 250;

    private static final String BACKLIGHT_PATH =
            "/sys/class/backlight/panel0-backlight/brightness";
    private static final String MAX_BACKLIGHT_PATH =
            "/sys/class/backlight/panel0-backlight/max_brightness";

    /**
     * ScreenCaptureInternal is synchronous and may wait for SurfaceFlinger/HWC. Never call it from
     * AutomaticBrightnessController's DisplayPowerController looper: blocking that looper can
     * prevent the display from waking. All captures are isolated on this background thread.
     */
    private static final HandlerThread CAPTURE_THREAD = createCaptureThread();
    private static final Handler CAPTURE_HANDLER = new Handler(CAPTURE_THREAD.getLooper());

    private final Object mLock = new Object();

    private long mLastCaptureUptime;
    private float mLastLumaNorm = -1f;
    private boolean mCapturePending;
    private int mCaptureGeneration;
    private float mMaxBacklight = 4095f;
    private boolean mMaxBacklightRead;
    private boolean mCaptureFailedLogged;
    private String mPublishedState = "";
    public MarbleAlsCorrection() {}

    /**
     * @param rawLux lux as reported by the (already backlight-compensated) sensor
     * @return corrected lux, never negative
     */
    @Override
    public double applyAsDouble(double rawLux) {
        return correct((float) rawLux);
    }

    private float correct(float rawLux) {
        if (!SystemProperties.getBoolean("persist.sys.als_correction.enabled", true)) {
            invalidateContentLuma();
            publishState(STATE_OFF);
            return rawLux;
        }

        // Read DBV before requesting a screenshot. ALS can remain enabled in doze; attempting a
        // display capture while the panel is off or transitioning is both useless and unsafe.
        final float dbvNorm = getBacklightNorm();
        if (dbvNorm <= 0f) {
            invalidateContentLuma();
            publishState(STATE_CAPTURE_FAILED);
            return rawLux;
        }

        final float lumaNorm = getContentLumaNormAsync();
        if (lumaNorm < 0f) {
            // The first sample is deliberately passed through while the background capture runs.
            publishState(STATE_CAPTURE_FAILED);
            return rawLux;
        }
        publishState(STATE_ACTIVE);

        final float k = clamp(
                SystemProperties.getInt("persist.sys.als_correction.k", DEF_K), K_MIN, K_MAX);
        final float referenceLuma = clamp(
                getFloatProp("persist.sys.als_correction.ref_luma", DEF_REFERENCE_LUMA),
                REF_LUMA_MIN, REF_LUMA_MAX);

        final float leakage = k * Math.max(0f, lumaNorm - referenceLuma) * dbvNorm;
        final float corrected = Math.max(0f, rawLux - leakage);

        if (DEBUG) {
            Slog.d(TAG, "raw=" + rawLux + " luma=" + lumaNorm + " dbv=" + dbvNorm
                    + " leak=" + leakage + " -> " + corrected);
        }
        return corrected;
    }

    /**
     * Returns the most recent content sample without blocking the caller and schedules an updated
     * capture when the cache expires. The caller is the DisplayPowerController looper.
     */
    private float getContentLumaNormAsync() {
        final long now = SystemClock.uptimeMillis();
        final long captureIntervalMs = clamp(
                SystemProperties.getInt("persist.sys.als_correction.interval_ms",
                        (int) MIN_CAPTURE_INTERVAL_MS), INTERVAL_MIN_MS, INTERVAL_MAX_MS);
        final int generation;
        synchronized (mLock) {
            if (now - mLastCaptureUptime < captureIntervalMs) {
                return mLastLumaNorm;
            }
            if (mCapturePending) {
                return mLastLumaNorm;
            }
            mCapturePending = true;
            generation = mCaptureGeneration;
        }
        CAPTURE_HANDLER.post(() -> updateContentLuma(generation));
        synchronized (mLock) {
            return mLastLumaNorm;
        }
    }

    /** Performs the potentially blocking SurfaceFlinger transaction off the display-power looper. */
    private void updateContentLuma(int generation) {
        final Bitmap bitmap = captureSensorRegion();
        float luma = -1f;
        if (bitmap != null) {
            try {
                luma = averageLuma(bitmap);
            } finally {
                bitmap.recycle();
            }
        }

        synchronized (mLock) {
            if (generation == mCaptureGeneration) {
                mLastLumaNorm = luma;
                // Rate-limit both successful captures and failures.
                mLastCaptureUptime = SystemClock.uptimeMillis();
            }
            mCapturePending = false;
        }
    }

    /** Drops content captured before/while the panel was turned off. */
    private void invalidateContentLuma() {
        synchronized (mLock) {
            if (mLastLumaNorm >= 0f || mCapturePending) {
                mCaptureGeneration++;
                mLastLumaNorm = -1f;
                mLastCaptureUptime = 0;
            }
        }
    }

    private static HandlerThread createCaptureThread() {
        final HandlerThread thread = new HandlerThread(
                "MarbleAlsCapture", Process.THREAD_PRIORITY_BACKGROUND);
        thread.start();
        return thread;
    }

    private Bitmap captureSensorRegion() {
        final int r = clamp(
                SystemProperties.getInt("persist.sys.als_correction.r", DEF_RADIUS),
                RADIUS_MIN, RADIUS_MAX);
        final int cx = clamp(
                SystemProperties.getInt("persist.sys.als_correction.cx", DEF_CENTER_X),
                0, PANEL_WIDTH - 1);
        final int cy = clamp(
                SystemProperties.getInt("persist.sys.als_correction.cy", DEF_CENTER_Y),
                0, PANEL_HEIGHT - 1);

        final Rect crop = new Rect(cx - r, cy - r, cx + r, cy + r);
        if (crop.left < 0) crop.left = 0;
        if (crop.top < 0) crop.top = 0;
        if (crop.right > PANEL_WIDTH) crop.right = PANEL_WIDTH;
        if (crop.bottom > PANEL_HEIGHT) crop.bottom = PANEL_HEIGHT;
        if (crop.isEmpty()) {
            return null;
        }

        final float scale = (float) SAMPLE_EDGE / Math.max(crop.width(), crop.height());

        try {
            final long[] ids = DisplayControl.getPhysicalDisplayIds();
            if (ids == null || ids.length == 0) {
                return null;
            }
            final IBinder token = DisplayControl.getPhysicalDisplayToken(ids[0]);
            if (token == null) {
                return null;
            }

            final ScreenCaptureInternal.DisplayCaptureArgs args =
                    new ScreenCaptureInternal.DisplayCaptureArgs.Builder(token)
                            .setSourceCrop(crop)
                            .setFrameScale(scale)
                            .build();

            final ScreenCaptureInternal.ScreenshotHardwareBuffer buffer =
                    ScreenCaptureInternal.captureDisplay(args);
            if (buffer == null) {
                return null;
            }

            final Bitmap hw = buffer.asBitmap();
            if (hw == null) {
                return null;
            }
            // asBitmap() returns a HARDWARE bitmap; getPixels() needs a software copy.
            final Bitmap sw = hw.copy(Bitmap.Config.ARGB_8888, false);
            hw.recycle();
            return sw;
        } catch (Throwable t) {
            if (!mCaptureFailedLogged) {
                mCaptureFailedLogged = true;
                Slog.w(TAG, "screen capture unavailable, correction disabled", t);
            }
            return null;
        }
    }

    /** Average light emission of the bitmap, linearized through the display gamma. */
    private static float averageLuma(Bitmap bitmap) {
        final int w = bitmap.getWidth();
        final int h = bitmap.getHeight();
        if (w <= 0 || h <= 0) {
            return 0f;
        }
        final int[] pixels = new int[w * h];
        bitmap.getPixels(pixels, 0, w, 0, 0, w, h);

        final float gamma = clamp(
                getFloatProp("persist.sys.als_correction.gamma", DEF_GAMMA),
                GAMMA_MIN, GAMMA_MAX);

        double sum = 0;
        for (int px : pixels) {
            final int r = (px >> 16) & 0xFF;
            final int g = (px >> 8) & 0xFF;
            final int b = px & 0xFF;
            // Rec.709 luma; the panel emits roughly proportional to the linearized value.
            final double srgb = (0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0;
            sum += Math.pow(srgb, gamma);
        }
        return (float) (sum / pixels.length);
    }

    /** Current panel DBV normalized to 0..1, or 1.0 if it cannot be read. */
    private float getBacklightNorm() {
        if (!mMaxBacklightRead) {
            mMaxBacklightRead = true;
            final float max = readSysfsFloat(MAX_BACKLIGHT_PATH);
            if (max > 0f) {
                mMaxBacklight = max;
            }
        }
        final float cur = readSysfsFloat(BACKLIGHT_PATH);
        if (cur < 0f) {
            return 1f;
        }
        return Math.min(1f, cur / mMaxBacklight);
    }

    private static float readSysfsFloat(String path) {
        try (FileInputStream is = new FileInputStream(path)) {
            final byte[] buf = new byte[16];
            final int n = is.read(buf);
            if (n <= 0) {
                return -1f;
            }
            return Float.parseFloat(new String(buf, 0, n, StandardCharsets.UTF_8).trim());
        } catch (Exception e) {
            return -1f;
        }
    }

    /** Publishes the operating state for settings UIs. Writes only on change. */
    private void publishState(String state) {
        synchronized (mLock) {
            if (state.equals(mPublishedState)) {
                return;
            }
            mPublishedState = state;
        }
        try {
            SystemProperties.set(STATE_PROP, state);
        } catch (RuntimeException e) {
            // Missing sepolicy for sys.als_correction.state must never break brightness.
            Slog.w(TAG, "cannot publish state: " + e.getMessage());
        }
    }

    private static int clamp(int value, int min, int max) {
        return Math.max(min, Math.min(max, value));
    }

    private static float clamp(float value, float min, float max) {
        return Math.max(min, Math.min(max, value));
    }

    private static float getFloatProp(String key, float def) {
        try {
            final String v = SystemProperties.get(key);
            return (v == null || v.isEmpty()) ? def : Float.parseFloat(v);
        } catch (NumberFormatException e) {
            return def;
        }
    }
}
