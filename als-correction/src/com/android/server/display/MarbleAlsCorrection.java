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
 *   This class samples the actual framebuffer content in a small region above the
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
 *   active         - a valid content capture is ready for correction
 *   capture_failed - the last screen capture attempt actually failed
 *   idle           - the panel is off or the ALS listener is inactive
 *   off            - disabled via persist.sys.als_correction.enabled
 *   empty          - waiting for fresh content or a readable DBV
 */

package com.android.server.display;

import android.graphics.Bitmap;
import android.graphics.Rect;
import android.hardware.HardwareBuffer;
import android.hardware.display.DisplayManagerInternal;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Process;
import android.os.SystemClock;
import android.os.SystemProperties;
import android.util.RotationUtils;
import android.util.Slog;
import android.view.Display;
import android.view.DisplayInfo;
import android.view.Surface;
import android.window.ScreenCapture.ScreenCaptureParams;
import android.window.ScreenCaptureInternal;

import com.android.internal.os.BackgroundThread;
import com.android.server.LocalServices;

import java.io.FileInputStream;
import java.nio.charset.StandardCharsets;
import java.util.function.Consumer;
import java.util.function.DoubleUnaryOperator;

/**
 * Removes display self-illumination from under-display ALS readings by measuring the
 * content actually displayed above the sensor.
 */
public final class MarbleAlsCorrection implements DoubleUnaryOperator, Consumer<Boolean> {
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

    /**
     * Spectral response of the complete marble OLED -> under-display TCS3701 -> TYPE_LIGHT
     * path. Derived from automated black/red/green/blue/white measurements at five DBV levels
     * (632, 1276, 2243, 3209 and 4095). At maximum DBV the measured residual contributions were
     * R=136.13, G=544.17 and B=71.95 lux. The normalized weights intentionally sum to 1, preserving
     * the existing white/gray calibration while correcting saturated-color content.
     */
    private static final double RED_WEIGHT = 0.1810;
    private static final double GREEN_WEIGHT = 0.7234;
    private static final double BLUE_WEIGHT = 0.0956;

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
    private static final String STATE_IDLE = "idle";
    private static final String STATE_OFF = "off";
    private static final String STATE_PENDING = "";

    /** Downscaled capture edge, in pixels. 16x16 = 256 samples, plenty for an average. */
    private static final int SAMPLE_EDGE = 16;

    /** Delay after a completed capture; independent of on-change ALS event delivery. */
    private static final long MIN_CAPTURE_INTERVAL_MS = 250;

    /** Retry unavailable/disabled content without hammering SurfaceFlinger or polling rapidly. */
    private static final long RETRY_INTERVAL_MS = 1000;

    /** Never subtract leakage from content sampled before a long gap or a slow capture. */
    private static final long MAX_CONTENT_AGE_MS = 2000;

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
    // State updates must not wait behind a stalled capture (including its expiry notification).
    private static final Handler STATE_HANDLER = BackgroundThread.getHandler();

    /** Cached lazily; LocalServices may not have DisplayManagerInternal yet during early boot. */
    private static volatile DisplayManagerInternal sDisplayManagerInternal;

    private final Object mLock = new Object();

    // Cache/lifecycle fields are guarded by mLock. elapsedRealtime includes time spent asleep.
    private long mLastCaptureElapsed = -1;
    private long mLastCaptureDurationMs = -1;
    private long mLastLumaElapsed = -1;
    private float mLastLumaNorm = -1f;
    private boolean mCapturePending;
    private boolean mRefreshScheduled;
    private boolean mLightSensorEnabled;
    private int mCaptureGeneration;
    private long mLastEnableElapsed = -1;
    private long mLastDisableElapsed = -1;
    private long mEnableCount;
    private long mDisableCount;
    private long mCaptureAttempts;
    private long mCaptureSuccesses;
    private long mCaptureFailures;
    private long mDiscardedCaptures;
    private long mExpiredSamples;
    private long mAlsSamples;
    private long mLastAlsElapsed = -1;
    private float mLastRawLux = Float.NaN;
    private float mLastCorrectedLux = Float.NaN;
    private long mLastUsedCacheAgeMs = -1;
    // Only accessed on the ALS callback looper. Retry sysfs until a valid maximum is available.
    private float mMaxBacklight = -1f;
    private boolean mCaptureFailedLogged;
    private String mRequestedState;
    private final Runnable mRefreshContent = this::updateContentLuma;
    private final Runnable mExpireContent = this::expireContent;
    private final Runnable mPublishState = () -> {
        final String state;
        synchronized (mLock) {
            state = mRequestedState;
        }
        setStateProp(state);
    };

    public MarbleAlsCorrection() {}

    /** Optional framework hook: notify us before enabling/disabling the ALS listener. */
    @Override
    public void accept(Boolean enabled) {
        synchronized (mLock) {
            mLightSensorEnabled = Boolean.TRUE.equals(enabled);
            if (mLightSensorEnabled) {
                mLastEnableElapsed = SystemClock.elapsedRealtime();
                mEnableCount++;
            } else {
                mLastDisableElapsed = SystemClock.elapsedRealtime();
                mDisableCount++;
            }
            CAPTURE_HANDLER.removeCallbacks(mRefreshContent);
            mRefreshScheduled = false;
            invalidateContentLumaLocked();
            publishStateLocked(!isCorrectionEnabled() ? STATE_OFF
                    : mLightSensorEnabled ? STATE_PENDING : STATE_IDLE);
            // Prewarm before the first ALS event. A running old generation will arrange the
            // new refresh when it finishes; never queue parallel or duplicate captures.
            scheduleRefreshLocked(0);
        }
    }

    private static boolean isCorrectionEnabled() {
        return SystemProperties.getBoolean("persist.sys.als_correction.enabled", true);
    }

    /**
     * @param rawLux lux as reported by the (already backlight-compensated) sensor
     * @return corrected lux, never negative
     */
    @Override
    public double applyAsDouble(double rawLux) {
        synchronized (mLock) {
            mLastUsedCacheAgeMs = -1;
        }
        final float correctedLux = correct((float) rawLux);
        synchronized (mLock) {
            mAlsSamples++;
            mLastAlsElapsed = SystemClock.elapsedRealtime();
            mLastRawLux = (float) rawLux;
            mLastCorrectedLux = correctedLux;
        }
        return correctedLux;
    }

    private float correct(float rawLux) {
        if (!Float.isFinite(rawLux) || rawLux < 0f) {
            return rawLux;
        }
        final boolean correctionEnabled = isCorrectionEnabled();
        synchronized (mLock) {
            if (!correctionEnabled || !mLightSensorEnabled) {
                invalidateContentLumaLocked();
                publishStateLocked(!correctionEnabled ? STATE_OFF : STATE_IDLE);
                return rawLux;
            }
        }

        // Use current DBV, not the brightness at the time of the cached capture. The worker
        // independently checks the display state and DBV before/after every capture.
        final float dbvNorm = getBacklightNorm();
        if (dbvNorm <= 0f) {
            synchronized (mLock) {
                invalidateContentLumaLocked();
                // An unreadable DBV is unknown, not full brightness or proof of screen OFF.
                publishStateLocked(dbvNorm == 0f ? STATE_IDLE : STATE_PENDING);
            }
            return rawLux;
        }

        final float lumaNorm = getFreshContentLuma();
        if (lumaNorm < 0f) {
            // No fresh sample: pass through until a background capture succeeds.
            // This is not necessarily a capture failure.
            return rawLux;
        }

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
        return Float.isFinite(corrected) ? corrected : rawLux;
    }

    /** Read-only cache consumption: ALS callbacks never schedule or wait for a capture. */
    private float getFreshContentLuma() {
        synchronized (mLock) {
            final long now = SystemClock.elapsedRealtime();
            expireContentLocked(now);
            if (mLastLumaNorm >= 0f) {
                mLastUsedCacheAgeMs = now - mLastLumaElapsed;
            }
            return mLastLumaNorm;
        }
    }

    private static long getCaptureIntervalMs() {
        return clamp(SystemProperties.getInt("persist.sys.als_correction.interval_ms",
                (int) MIN_CAPTURE_INTERVAL_MS), INTERVAL_MIN_MS, INTERVAL_MAX_MS);
    }

    /** Caller holds mLock. Scheduling is completion-based: no backlog or catch-up burst. */
    private void scheduleRefreshLocked(long delayMs) {
        if (!mLightSensorEnabled || mCapturePending) {
            return;
        }
        CAPTURE_HANDLER.removeCallbacks(mRefreshContent);
        mRefreshScheduled = CAPTURE_HANDLER.postDelayed(mRefreshContent, delayMs);
    }

    /** Runs solely on the capture worker, even if no TYPE_LIGHT event arrives for minutes. */
    private void updateContentLuma() {
        final int generation;
        synchronized (mLock) {
            // Coalesce a prewarm posted while this runnable was already being dispatched.
            CAPTURE_HANDLER.removeCallbacks(mRefreshContent);
            mRefreshScheduled = false;
            if (!mLightSensorEnabled || mCapturePending) {
                return;
            }
            mCapturePending = true;
            generation = mCaptureGeneration;
        }

        final long captureStarted = SystemClock.elapsedRealtime();
        float luma = -1f;
        boolean attempted = false;
        String unavailableState = STATE_PENDING;
        try {
            // Skip expensive work when disabled, screen OFF/dozing, or DBV is unreadable.
            unavailableState = getUnavailableState();
            if (unavailableState == null) {
                attempted = true;
                final Bitmap bitmap = captureSensorRegion();
                if (bitmap != null) {
                    try {
                        luma = averageLuma(bitmap);
                    } finally {
                        bitmap.recycle();
                    }
                }
                unavailableState = getUnavailableState();
            }
        } catch (RuntimeException | LinkageError e) {
            logCaptureFailure(e);
            luma = -1f;
            unavailableState = STATE_CAPTURE_FAILED;
        } finally {
            synchronized (mLock) {
                mCapturePending = false;
                final long now = SystemClock.elapsedRealtime();
                if (attempted) {
                    mCaptureAttempts++;
                    mLastCaptureElapsed = now;
                    mLastCaptureDurationMs = now - captureStarted;
                }
                final boolean accepted = generation == mCaptureGeneration && mLightSensorEnabled;
                long nextDelayMs = Math.max(getCaptureIntervalMs(), RETRY_INTERVAL_MS);
                if (!accepted) {
                    if (attempted) {
                        mDiscardedCaptures++;
                    }
                    // A disable/enable cycle during capture needs a new-generation prewarm.
                    nextDelayMs = 0;
                } else {
                    clearContentLumaLocked();
                    if (unavailableState != null) {
                        if (attempted) {
                            if (STATE_CAPTURE_FAILED.equals(unavailableState)) {
                                mCaptureFailures++;
                            } else {
                                mDiscardedCaptures++;
                            }
                        }
                        publishStateLocked(unavailableState);
                    } else if (!Float.isFinite(luma) || luma < 0f) {
                        mCaptureFailures++;
                        publishStateLocked(STATE_CAPTURE_FAILED);
                    } else if (now - captureStarted >= MAX_CONTENT_AGE_MS) {
                        mDiscardedCaptures++;
                        publishStateLocked(STATE_PENDING);
                    } else {
                        mLastLumaNorm = luma;
                        mLastLumaElapsed = captureStarted;
                        mCaptureSuccesses++;
                        mCaptureFailedLogged = false;
                        publishStateLocked(STATE_ACTIVE);
                        STATE_HANDLER.postDelayed(mExpireContent,
                                MAX_CONTENT_AGE_MS - (now - captureStarted));
                        nextDelayMs = getCaptureIntervalMs();
                    }
                }
                // Disabled correction polls only its property; disabled ALS stops all work.
                // This also recovers after a live property change without needing an ALS event.
                scheduleRefreshLocked(nextDelayMs);
            }
        }
    }

    /** null means capture is usable; an empty state means display/DBV information is unavailable. */
    private static String getUnavailableState() {
        if (!isCorrectionEnabled()) {
            return STATE_OFF;
        }
        final DisplayInfo info = getDefaultDisplayInfo();
        if (info == null) {
            return STATE_PENDING;
        }
        if (info.state != Display.STATE_ON) {
            return STATE_IDLE;
        }
        final float dbv = readSysfsFloat(BACKLIGHT_PATH);
        return dbv < 0f ? STATE_PENDING : dbv == 0f ? STATE_IDLE : null;
    }

    private void expireContent() {
        synchronized (mLock) {
            expireContentLocked(SystemClock.elapsedRealtime());
        }
    }

    private void expireContentLocked(long now) {
        if (mLastLumaNorm >= 0f && now - mLastLumaElapsed >= MAX_CONTENT_AGE_MS) {
            mExpiredSamples++;
            clearContentLumaLocked();
            publishStateLocked(STATE_PENDING);
        }
    }

    /** Caller holds mLock. Keep pending set until the old worker exits; never queue duplicates. */
    private void invalidateContentLumaLocked() {
        mCaptureGeneration++;
        clearContentLumaLocked();
    }

    private void clearContentLumaLocked() {
        mLastLumaNorm = -1f;
        mLastLumaElapsed = -1;
        STATE_HANDLER.removeCallbacks(mExpireContent);
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

        try {
            // The capture sourceCrop is interpreted in layer-stack (logical, rotated) coordinates,
            // while the sensor position is fixed in natural panel coordinates. Transform the crop
            // the same way UDFPS transforms its sensor bounds, or a rotated UI would sample a
            // screen region unrelated to the physical sensor.
            final int rotation = getDisplayRotation();
            if (rotation != Surface.ROTATION_0) {
                RotationUtils.rotateBounds(crop, PANEL_WIDTH, PANEL_HEIGHT, rotation);
            }

            final float scale = (float) SAMPLE_EDGE / Math.max(crop.width(), crop.height());

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
                            // Redaction would look like real black pixels to the ALS model.
                            // Reject unavailable content instead; never capture secure/DRM pixels.
                            .setSecureContentPolicy(
                                    ScreenCaptureParams.SECURE_CONTENT_POLICY_THROW_EXCEPTION)
                            .setProtectedContentPolicy(
                                    ScreenCaptureParams.PROTECTED_CONTENT_POLICY_THROW_EXCEPTION)
                            .build();

            final ScreenCaptureInternal.ScreenshotHardwareBuffer buffer =
                    ScreenCaptureInternal.captureDisplay(args);
            if (buffer == null) {
                return null;
            }

            // The HardwareBuffer owns a dmabuf file descriptor inside system_server. Close it
            // deterministically: at up to several captures per second, leaving it to GC finalizers
            // accumulates dead fds and native memory between collections.
            try {
                final Bitmap hw = buffer.asBitmap();
                if (hw == null) {
                    return null;
                }
                // asBitmap() returns a HARDWARE bitmap; getPixels() needs a software copy.
                try {
                    return hw.copy(Bitmap.Config.ARGB_8888, false);
                } finally {
                    hw.recycle();
                }
            } finally {
                final HardwareBuffer hb = buffer.getHardwareBuffer();
                if (hb != null) {
                    hb.close();
                }
            }
        } catch (RuntimeException | LinkageError e) {
            logCaptureFailure(e);
            return null;
        }
    }

    private void logCaptureFailure(Throwable error) {
        if (!mCaptureFailedLogged) {
            mCaptureFailedLogged = true;
            Slog.w(TAG, "screen capture unavailable, passing through unmodified lux", error);
        }
    }

    /** Average TCS3701-weighted panel emission, linearized per RGB channel. */
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
            final double r = ((px >> 16) & 0xFF) / 255.0;
            final double g = ((px >> 8) & 0xFF) / 255.0;
            final double b = (px & 0xFF) / 255.0;

            // Linearize each OLED primary independently, then apply the measured spectral response
            // of the under-display ALS. Rec.709 weights describe human vision and under-corrected
            // blue while over-correcting red on the measured marble panel.
            sum += RED_WEIGHT * Math.pow(r, gamma)
                    + GREEN_WEIGHT * Math.pow(g, gamma)
                    + BLUE_WEIGHT * Math.pow(b, gamma);
        }
        return (float) (sum / pixels.length);
    }

    /** Current panel DBV normalized to 0..1, or -1 if it cannot be read reliably. */
    private float getBacklightNorm() {
        final float cur = readSysfsFloat(BACKLIGHT_PATH);
        if (cur <= 0f) {
            return cur;
        }
        if (mMaxBacklight <= 0f) {
            mMaxBacklight = readSysfsFloat(MAX_BACKLIGHT_PATH);
        }
        return mMaxBacklight > 0f ? Math.min(1f, cur / mMaxBacklight) : -1f;
    }

    private static float readSysfsFloat(String path) {
        try (FileInputStream is = new FileInputStream(path)) {
            final byte[] buf = new byte[16];
            final int n = is.read(buf);
            if (n <= 0) {
                return -1f;
            }
            final float value = Float.parseFloat(
                    new String(buf, 0, n, StandardCharsets.UTF_8).trim());
            return Float.isFinite(value) && value >= 0f ? value : -1f;
        } catch (Exception e) {
            return -1f;
        }
    }

    /** Current rotation of the built-in display, or ROTATION_0 when not yet available. */
    private static int getDisplayRotation() {
        final DisplayInfo info = getDefaultDisplayInfo();
        return info != null ? info.rotation : Surface.ROTATION_0;
    }

    private static DisplayInfo getDefaultDisplayInfo() {
        DisplayManagerInternal dmi = sDisplayManagerInternal;
        if (dmi == null) {
            dmi = LocalServices.getService(DisplayManagerInternal.class);
            if (dmi == null) {
                return null;
            }
            sDisplayManagerInternal = dmi;
        }
        return dmi.getDisplayInfo(Display.DEFAULT_DISPLAY);
    }

    /** Compact snapshot used by AutomaticBrightnessController.dump(); no I/O or state changes. */
    @Override
    public String toString() {
        synchronized (mLock) {
            final long now = SystemClock.elapsedRealtime();
            final long cacheAgeMs = ageMs(now, mLastLumaElapsed);
            return "MarbleAlsCorrection{state="
                    + (mRequestedState == null || mRequestedState.isEmpty()
                            ? "pending" : mRequestedState)
                    + ", alsEnabled=" + mLightSensorEnabled
                    + ", generation=" + mCaptureGeneration
                    + ", enableCount=" + mEnableCount + ", disableCount=" + mDisableCount
                    + ", lastEnableElapsedMs=" + mLastEnableElapsed
                    + ", lastDisableElapsedMs=" + mLastDisableElapsed
                    + ", capturePending=" + mCapturePending
                    + ", refreshScheduled=" + mRefreshScheduled
                    + ", cacheAgeMs=" + cacheAgeMs
                    + ", cacheValid=" + (mLastLumaNorm >= 0f && cacheAgeMs >= 0
                            && cacheAgeMs < MAX_CONTENT_AGE_MS)
                    + ", contentLevel=" + mLastLumaNorm
                    + ", lastCaptureAgeMs=" + ageMs(now, mLastCaptureElapsed)
                    + ", lastCaptureDurationMs=" + mLastCaptureDurationMs
                    + ", captures=" + mCaptureAttempts + ", successes=" + mCaptureSuccesses
                    + ", failures=" + mCaptureFailures + ", discarded=" + mDiscardedCaptures
                    + ", expired=" + mExpiredSamples
                    + ", alsSamples=" + mAlsSamples
                    + ", lastAlsAgeMs=" + ageMs(now, mLastAlsElapsed)
                    + ", lastRawLux=" + mLastRawLux + ", lastCorrectedLux=" + mLastCorrectedLux
                    + ", lastUsedCacheAgeMs=" + mLastUsedCacheAgeMs
                    + "}";
        }
    }

    private static long ageMs(long now, long timestamp) {
        return timestamp < 0 ? -1 : now - timestamp;
    }

    /**
     * Caller holds mLock, including when checking capture generation and accepting a result.
     * Coalesce queued updates; the writer reads the latest desired state, never a stale capture's
     * state. A separate background handler keeps property I/O off the display/capture loopers.
     */
    private void publishStateLocked(String state) {
        if (state.equals(mRequestedState)) {
            return;
        }
        mRequestedState = state;
        STATE_HANDLER.removeCallbacks(mPublishState);
        STATE_HANDLER.post(mPublishState);
    }

    private static void setStateProp(String state) {
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
            final float value = (v == null || v.isEmpty()) ? def : Float.parseFloat(v);
            return Float.isFinite(value) ? value : def;
        } catch (NumberFormatException e) {
            return def;
        }
    }
}
