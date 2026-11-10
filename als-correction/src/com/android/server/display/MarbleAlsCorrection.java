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
 *   persist.sys.als_correction.interval_ms (int, capture interval, default 500 ms)
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
import android.opengl.Matrix;
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
import com.android.server.display.color.DisplayTransformManager;

import java.io.FileInputStream;
import java.lang.ref.WeakReference;
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
     * Lux subtracted for a fully lit disc at maximum panel brightness.
     *
     * Three sets of measurements exist on marble, and no single k that is linear in DBV fits
     * all of them:
     *   - white vs black at DBV 250/500/1000/2000: 21.96/43.63/88.41/184.61 lux. With the
     *     framebuffer delta-luma of 0.5763 measured for that content, k = 624/620/628/656.
     *   - white vs black at DBV 4095: 497 vs 181 lux (file header). k = 548 if the delta-luma
     *     was the same 0.5763, which is an assumption.
     *   - full red + green + blue at DBV 4095 (the spectral weights below): 752 lux in total.
     *   - light vs dark Settings at DBV 123-252, twelve single transitions taken while
     *     checking this code under room light that was not held constant: k = 420..630,
     *     median about 520.
     * The default was lowered to 550 to avoid over-subtracting under mixed content. By the
     * first set it leaves 11-16% of the white-content leakage in the reading up to DBV 2000
     * (155 instead of 185 lux at DBV 2000), and persist.sys.als_correction.k = 640 matches
     * that set; by the last one 550 is about right at low brightness. Which set describes
     * the panel best is open until they are measured again in one session.
     */
    private static final int DEF_K = 550;

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

    /** Precomputed circular sensor mask for the 16x16 sample grid (excludes corner pixels). */
    private static final boolean[] DISC_MASK = createDiscMask();

    /** Delay after a completed capture; independent of on-change ALS event delivery. */
    private static final long DEFAULT_CAPTURE_INTERVAL_MS = 500;

    /** Retry unavailable/disabled content without hammering SurfaceFlinger or polling rapidly. */
    private static final long RETRY_INTERVAL_MS = 1000;

    /** Never subtract leakage from content sampled before a long gap or a slow capture. */
    private static final long MAX_CONTENT_AGE_MS = 2000;

    /**
     * How long the leakage estimate has to hold before the latest raw lux is corrected again
     * with it: long enough for a theme or window transition above the sensor to finish, so
     * that the estimate is the one for the content that stays. Every new reading triggers a
     * capture, so the second look comes at most SETTLE_MS plus three capture durations after
     * the reading; 0.4-0.65 s was measured on marble. That is inside the 1000 ms brightening
     * debounce of the sm8450 overlay: a sample corrected with outdated content is replaced
     * before it can raise the brightness.
     */
    private static final long SETTLE_MS = 600;

    /** A re-evaluation that was asked for is expected back within this time. */
    private static final long REEVALUATE_WINDOW_MS = 1000;

    /** Lux and leakage differences below max(1 lux, 5%) are not worth a new sample. */
    private static final float SIGNIFICANT_LUX = 1f;
    private static final float SIGNIFICANT_FRACTION = 0.05f;

    /** The ALS listener starts while the panel is still turning on; look again this often. */
    private static final long WAKE_RETRY_INTERVAL_MS = 100;
    private static final long WAKE_RETRY_WINDOW_MS = 2000;

    /**
     * DisplayTransformManager hands out its color matrices per level only. The levels used in
     * this tree end at 350 (framework tints, LineageOS calibration and reading mode).
     */
    private static final int COLOR_MATRIX_LEVEL_LIMIT = 512;

    /** Global saturation of SurfaceFlinger: as XiaomiParts applied it, else the framework's. */
    private static final String PARTS_SATURATION_PROP = "sys.parts.sf_saturation";
    private static final String SF_SATURATION_PROP = "persist.sys.sf.color_saturation";

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
    private float mLastRedNorm = -1f;
    private float mLastGreenNorm = -1f;
    private float mLastBlueNorm = -1f;
    private boolean mCapturePending;
    // A new reading arrived during the running capture; capture again as soon as it is done.
    private boolean mRefreshAfterCapture;
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
    // ALS events that passed through uncorrected only because no fresh content was cached.
    private long mCacheMisses;
    private long mLastAlsElapsed = -1;
    private float mLastRawLux = Float.NaN;
    private float mLastCorrectedLux = Float.NaN;
    private long mLastUsedCacheAgeMs = -1;
    // A run is a stretch of captures with the same leakage estimate. Guarded by mLock.
    private long mRunStartElapsed = -1;
    private float mRunLeakage;
    // Start of the last capture that still belonged to the previous run, -1 if there was none.
    private long mPrevRunEndElapsed = -1;
    private long mLastGoodCaptureElapsed = -1;
    private long mReevaluateRequestElapsed = -1;
    private long mReevaluateRequests;
    private long mReevaluatedSamples;
    private boolean mColorTransformApplied;
    // Asks the framework to pass the latest raw lux through again. Null with an older hook.
    private final Runnable mReevaluate;
    private boolean mReevaluateFailedLogged;
    // Read on the ALS callback loopers and the capture worker; retried until it is valid.
    private volatile float mMaxBacklight = -1f;
    // Only accessed while holding mAlsSysfsBuf: more than one looper may deliver ALS events.
    private final byte[] mAlsSysfsBuf = new byte[16];
    // Only accessed on the capture worker thread.
    private final byte[] mWorkerSysfsBuf = new byte[16];
    private float mWorkerDbvNorm = -1f;
    private float[] mColorTransform;
    private boolean mColorTransformLinear;
    private boolean mColorTransformFailedLogged;
    private final float[] mMatrixA = new float[16];
    private final float[] mMatrixB = new float[16];
    private final float[] mSaturationMatrix = new float[16];
    private double mSumR;
    private double mSumG;
    private double mSumB;
    private final int[] mPixelBuffer = new int[SAMPLE_EDGE * SAMPLE_EDGE];
    private final float[] mRgbScratch = new float[3];
    private final float[] mGammaLut = new float[256];
    private float mCachedGamma = Float.NaN;
    // Sensor disc inside the last capture crop, as fractions of the crop size. They differ from
    // the inscribed circle only when the crop was cut off by a panel edge.
    private boolean mDiscClipped;
    private float mDiscCenterX = 0.5f;
    private float mDiscCenterY = 0.5f;
    private float mDiscRadiusX = 0.5f;
    private float mDiscRadiusY = 0.5f;
    private IBinder mDisplayToken;
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

    /** For a framework hook that cannot ask for re-evaluation. */
    public MarbleAlsCorrection() {
        this(null);
    }

    /**
     * @param reevaluate makes the framework pass the latest raw lux of every light sensor
     *                   listener through {@link #applyAsDouble} again; safe on any thread
     */
    public MarbleAlsCorrection(Runnable reevaluate) {
        mReevaluate = reevaluate;
        SystemProperties.addChangeCallback(new SyspropCallback(this));
    }

    /**
     * SystemProperties keeps its callbacks forever and there is no teardown hook. The current
     * framework hook creates one corrector per system_server, but the first version of the hook
     * created one per AutomaticBrightnessController, which is recreated on display
     * configuration changes. Hold the corrector weakly and unregister once it is gone, so
     * replaced instances can be collected.
     */
    private static final class SyspropCallback implements Runnable {
        private final WeakReference<MarbleAlsCorrection> mCorrection;

        SyspropCallback(MarbleAlsCorrection correction) {
            mCorrection = new WeakReference<>(correction);
        }

        @Override
        public void run() {
            final MarbleAlsCorrection correction = mCorrection.get();
            if (correction == null) {
                // Safe during dispatch: SystemProperties iterates over a copy of the list.
                SystemProperties.removeChangeCallback(this);
            } else {
                correction.onSyspropChanged();
            }
        }
    }

    /** Runs on a binder thread when a client reports a property change to system_server. */
    private void onSyspropChanged() {
        synchronized (mLock) {
            if (!mLightSensorEnabled) {
                // No capture without the ALS listener; only keep the published state truthful.
                publishStateLocked(isCorrectionEnabled() ? STATE_IDLE : STATE_OFF);
                return;
            }
            if (!isCorrectionEnabled()) {
                CAPTURE_HANDLER.removeCallbacks(mRefreshContent);
                mRefreshScheduled = false;
                invalidateContentLumaLocked();
                publishStateLocked(STATE_OFF);
            } else if (!mCapturePending && !mRefreshScheduled) {
                publishStateLocked(STATE_PENDING);
                scheduleRefreshLocked(0);
            }
        }
    }

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
            mRefreshAfterCapture = false;
            // Readings of the previous listener session must not be corrected again.
            mLastRawLux = Float.NaN;
            mReevaluateRequestElapsed = -1;
            invalidateContentLumaLocked();
            final boolean correctionEnabled = isCorrectionEnabled();
            publishStateLocked(!correctionEnabled ? STATE_OFF
                    : mLightSensorEnabled ? STATE_PENDING : STATE_IDLE);
            // Prewarm before the first ALS event only when correction is enabled.
            if (correctionEnabled) {
                scheduleRefreshLocked(0);
            }
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
        final float raw = (float) rawLux;
        final long now = SystemClock.elapsedRealtime();
        // The same value again is the reading we already have: delivered to a second listener
        // of the sensor, or passed through again because we asked for it.
        final boolean newReading;
        final boolean reevaluation;
        synchronized (mLock) {
            mLastUsedCacheAgeMs = -1;
            newReading = raw != mLastRawLux;
            reevaluation = !newReading && mReevaluateRequestElapsed >= 0
                    && now - mReevaluateRequestElapsed < REEVALUATE_WINDOW_MS;
        }
        final float correctedLux = correct(raw, newReading);
        synchronized (mLock) {
            if (reevaluation) {
                mReevaluatedSamples++;
            } else {
                mAlsSamples++;
                mLastAlsElapsed = now;
            }
            mLastRawLux = raw;
            mLastCorrectedLux = correctedLux;
        }
        return correctedLux;
    }

    private float correct(float rawLux, boolean newReading) {
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
        final float dbvNorm;
        synchronized (mAlsSysfsBuf) {
            dbvNorm = getBacklightNorm(mAlsSysfsBuf);
        }
        if (dbvNorm <= 0f) {
            synchronized (mLock) {
                invalidateContentLumaLocked();
                // An unreadable DBV is unknown, not full brightness or proof of screen OFF.
                publishStateLocked(dbvNorm == 0f ? STATE_IDLE : STATE_PENDING);
            }
            return rawLux;
        }

        final float rNorm;
        final float gNorm;
        final float bNorm;
        final float lumaNorm;
        synchronized (mLock) {
            final long now = SystemClock.elapsedRealtime();
            expireContentLocked(now);
            lumaNorm = mLastLumaNorm;
            rNorm = mLastRedNorm;
            gNorm = mLastGreenNorm;
            bNorm = mLastBlueNorm;
            if (lumaNorm >= 0f) {
                mLastUsedCacheAgeMs = now - mLastLumaElapsed;
            } else {
                mCacheMisses++;
            }
            if (newReading) {
                // The sensor reports on change, and what changed is often the screen above
                // it. Look at the screen as it is after this reading instead of waiting for
                // the periodic capture: if the content is new, this reading is corrected
                // again once the estimate has settled. Also recovers after enabling via adb
                // setprop, which does not notify system_server.
                if (mCapturePending) {
                    mRefreshAfterCapture = true;
                } else {
                    scheduleRefreshLocked(0);
                }
            } else if (lumaNorm < 0f && !mCapturePending && !mRefreshScheduled) {
                scheduleRefreshLocked(0);
            }
        }
        if (lumaNorm < 0f) {
            // No fresh sample: pass through until a background capture succeeds.
            // This is not necessarily a capture failure.
            return rawLux;
        }

        final float leakage = leakageLux(rNorm, gNorm, bNorm, dbvNorm);
        final float corrected = Math.max(0f, rawLux - leakage);

        if (DEBUG) {
            Slog.d(TAG, "raw=" + rawLux + " luma=" + lumaNorm + " dbv=" + dbvNorm
                    + " leak=" + leakage + " -> " + corrected
                    + (newReading ? "" : " (again)"));
        }
        return Float.isFinite(corrected) ? corrected : rawLux;
    }

    /** Lux that the panel adds to the sensor reading for this content at this DBV. */
    private static float leakageLux(float rNorm, float gNorm, float bNorm, float dbvNorm) {
        final float k = clamp(
                SystemProperties.getInt("persist.sys.als_correction.k", DEF_K), K_MIN, K_MAX);
        final float referenceLuma = clamp(
                getFloatProp("persist.sys.als_correction.ref_luma", DEF_REFERENCE_LUMA),
                REF_LUMA_MIN, REF_LUMA_MAX);

        // Apply the dark-UI baseline per primary before spectral weighting. For neutral gray/white
        // (R = G = B) this is algebraically identical to max(0, lumaNorm - referenceLuma) because
        // the weights sum to 1, while allowing saturated blue and red content to be compensated.
        final float effR = Math.max(0f, rNorm - referenceLuma);
        final float effG = Math.max(0f, gNorm - referenceLuma);
        final float effB = Math.max(0f, bNorm - referenceLuma);
        final float excessContent = (float) (RED_WEIGHT * effR
                + GREEN_WEIGHT * effG
                + BLUE_WEIGHT * effB);
        return k * excessContent * dbvNorm;
    }

    private static boolean isSignificantChange(float a, float b) {
        return Math.abs(a - b) >= Math.max(SIGNIFICANT_LUX,
                SIGNIFICANT_FRACTION * Math.max(Math.abs(a), Math.abs(b)));
    }

    /**
     * Caller holds mLock, on the capture worker, after a capture was accepted. Returns true if
     * the latest raw lux has to be corrected again.
     *
     * An ALS event that arrived after the last capture of the previous content was most
     * likely caused by the new content, but may have been corrected with the previous one,
     * and nothing else would correct it until the light changes. Once the new estimate has
     * held for SETTLE_MS, that reading is corrected again with it. An event older than that
     * was corrected consistently and is left alone. On marble the sensor reported a changed
     * leakage anywhere from 0.2 s to several seconds after the screen changed, and at low
     * brightness sometimes not within 7 s; whenever it does report, that reading is corrected
     * with the content cached by then.
     */
    private boolean trackLeakageLocked(long captureStarted, long now, float leakage) {
        final long lastGoodCapture = mLastGoodCaptureElapsed;
        mLastGoodCaptureElapsed = captureStarted;
        if (mRunStartElapsed < 0 || isSignificantChange(leakage, mRunLeakage)) {
            mPrevRunEndElapsed = lastGoodCapture;
            mRunStartElapsed = captureStarted;
            mRunLeakage = leakage;
            return false;
        }
        if (mReevaluate == null || now - mRunStartElapsed < SETTLE_MS
                || Float.isNaN(mLastRawLux) || mLastAlsElapsed <= mPrevRunEndElapsed) {
            return false;
        }
        if (mReevaluateRequestElapsed >= 0
                && now - mReevaluateRequestElapsed < REEVALUATE_WINDOW_MS) {
            return false;
        }
        if (!isSignificantChange(Math.max(0f, mLastRawLux - leakage), mLastCorrectedLux)) {
            return false;
        }
        mReevaluateRequestElapsed = now;
        mReevaluateRequests++;
        return true;
    }

    /** Caller holds mLock. Lands a capture on the moment the current run has settled. */
    private long refreshDelayLocked(long now, long intervalMs) {
        final long untilSettled = mRunStartElapsed + SETTLE_MS - now;
        if (mReevaluate == null || mRunStartElapsed < 0 || untilSettled <= 0) {
            return intervalMs;
        }
        // Wait a little longer than the interval rather than capture twice in quick succession.
        return untilSettled - intervalMs < intervalMs / 2 ? untilSettled : intervalMs;
    }

    private static long getCaptureIntervalMs() {
        return clamp(SystemProperties.getInt("persist.sys.als_correction.interval_ms",
                (int) DEFAULT_CAPTURE_INTERVAL_MS), INTERVAL_MIN_MS, INTERVAL_MAX_MS);
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
        float rNorm = -1f;
        float gNorm = -1f;
        float bNorm = -1f;
        float leakage = 0f;
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
                        luma = averageRgb(bitmap, mRgbScratch);
                        rNorm = mRgbScratch[0];
                        gNorm = mRgbScratch[1];
                        bNorm = mRgbScratch[2];
                    } finally {
                        bitmap.recycle();
                    }
                }
                unavailableState = getUnavailableState();
                if (unavailableState == null && luma >= 0f) {
                    leakage = leakageLux(rNorm, gNorm, bNorm, mWorkerDbvNorm);
                }
            }
        } catch (RuntimeException | LinkageError e) {
            logCaptureFailure(e);
            luma = -1f;
            unavailableState = STATE_CAPTURE_FAILED;
        } finally {
            boolean reevaluate = false;
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
                boolean shouldSchedule = true;
                if (!accepted) {
                    if (attempted) {
                        mDiscardedCaptures++;
                    }
                    // A disable/enable cycle during capture needs a new-generation prewarm.
                    nextDelayMs = 0;
                    shouldSchedule = mLightSensorEnabled && isCorrectionEnabled();
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
                        mRunStartElapsed = -1;
                        publishStateLocked(unavailableState);
                        if (STATE_OFF.equals(unavailableState)) {
                            // Stop background wakeups completely when correction is disabled.
                            shouldSchedule = false;
                        } else if (STATE_IDLE.equals(unavailableState) && mLastEnableElapsed >= 0
                                && now - mLastEnableElapsed < WAKE_RETRY_WINDOW_MS) {
                            // The listener was enabled before the panel is on. Have content
                            // ready for the first reading instead of a second later.
                            nextDelayMs = WAKE_RETRY_INTERVAL_MS;
                        }
                    } else if (!Float.isFinite(luma) || luma < 0f) {
                        mRunStartElapsed = -1;
                        mCaptureFailures++;
                        publishStateLocked(STATE_CAPTURE_FAILED);
                    } else if (now - captureStarted >= MAX_CONTENT_AGE_MS) {
                        mRunStartElapsed = -1;
                        mDiscardedCaptures++;
                        publishStateLocked(STATE_PENDING);
                    } else {
                        mLastLumaNorm = luma;
                        mLastRedNorm = rNorm;
                        mLastGreenNorm = gNorm;
                        mLastBlueNorm = bNorm;
                        mLastLumaElapsed = captureStarted;
                        mCaptureSuccesses++;
                        mCaptureFailedLogged = false;
                        publishStateLocked(STATE_ACTIVE);
                        STATE_HANDLER.postDelayed(mExpireContent,
                                MAX_CONTENT_AGE_MS - (now - captureStarted));
                        mColorTransformApplied = mColorTransform != null;
                        reevaluate = trackLeakageLocked(captureStarted, now, leakage);
                        nextDelayMs = refreshDelayLocked(now, getCaptureIntervalMs());
                    }
                }
                if (mRefreshAfterCapture) {
                    // Started before the latest reading: look again at what is shown after it.
                    mRefreshAfterCapture = false;
                    nextDelayMs = 0;
                }
                if (shouldSchedule) {
                    scheduleRefreshLocked(nextDelayMs);
                }
            }
            if (reevaluate) {
                // Outside mLock: the framework takes its own lock, which it also holds while
                // calling accept().
                try {
                    mReevaluate.run();
                } catch (RuntimeException | LinkageError e) {
                    if (!mReevaluateFailedLogged) {
                        mReevaluateFailedLogged = true;
                        Slog.w(TAG, "cannot ask for re-evaluation", e);
                    }
                }
            }
        }
    }

    /**
     * null means capture is usable and mWorkerDbvNorm holds the DBV; an empty state means
     * display/DBV information is unavailable. Runs only on the capture worker.
     */
    private String getUnavailableState() {
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
        mWorkerDbvNorm = getBacklightNorm(mWorkerSysfsBuf);
        return mWorkerDbvNorm < 0f ? STATE_PENDING : mWorkerDbvNorm == 0f ? STATE_IDLE : null;
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
        mRunStartElapsed = -1;
        mLastGoodCaptureElapsed = -1;
        clearContentLumaLocked();
    }

    private void clearContentLumaLocked() {
        mLastLumaNorm = -1f;
        mLastRedNorm = -1f;
        mLastGreenNorm = -1f;
        mLastBlueNorm = -1f;
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

        final Rect disc = new Rect(cx - r, cy - r, cx + r, cy + r);
        final Rect crop = new Rect(disc);
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
                RotationUtils.rotateBounds(disc, PANEL_WIDTH, PANEL_HEIGHT, rotation);
            }

            // A crop cut off by a panel edge is no longer centered on the sensor. Tell
            // averageRgb() where the disc really is instead of assuming the inscribed circle.
            mDiscClipped = !crop.equals(disc);
            mDiscCenterX = (disc.exactCenterX() - crop.left) / crop.width();
            mDiscCenterY = (disc.exactCenterY() - crop.top) / crop.height();
            mDiscRadiusX = (float) r / crop.width();
            mDiscRadiusY = (float) r / crop.height();

            // SurfaceFlinger truncates crop * scale, and 16f / 94 * 94 is 15.999999 in float.
            // Aim for the middle of the last pixel so that the default crop really is 16 x 16.
            final float scale = (SAMPLE_EDGE + 0.5f) / Math.max(crop.width(), crop.height());

            IBinder token = mDisplayToken;
            if (token == null) {
                final long[] ids = DisplayControl.getPhysicalDisplayIds();
                if (ids == null || ids.length == 0) {
                    return null;
                }
                token = DisplayControl.getPhysicalDisplayToken(ids[0]);
                if (token == null) {
                    return null;
                }
                mDisplayToken = token;
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
                mDisplayToken = null;
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
            mDisplayToken = null;
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

    private static boolean[] createDiscMask() {
        final boolean[] mask = new boolean[SAMPLE_EDGE * SAMPLE_EDGE];
        final float center = SAMPLE_EDGE * 0.5f;
        final float radiusSq = center * center;
        for (int y = 0; y < SAMPLE_EDGE; y++) {
            final float dy = (y + 0.5f) - center;
            for (int x = 0; x < SAMPLE_EDGE; x++) {
                final float dx = (x + 0.5f) - center;
                mask[y * SAMPLE_EDGE + x] = (dx * dx + dy * dy) <= radiusSq;
            }
        }
        return mask;
    }

    private float[] getGammaLut(float gamma) {
        if (gamma != mCachedGamma) {
            mGammaLut[0] = 0f;
            for (int i = 1; i < 255; i++) {
                mGammaLut[i] = (float) Math.pow(i / 255.0, gamma);
            }
            mGammaLut[255] = 1f;
            mCachedGamma = gamma;
        }
        return mGammaLut;
    }

    /**
     * Computes average linearized R, G, B emission inside the sensor disc and returns the
     * TCS3701-weighted total. Runs only on the capture worker thread.
     */
    private float averageRgb(Bitmap bitmap, float[] outRgb) {
        final int w = bitmap.getWidth();
        final int h = bitmap.getHeight();
        if (w <= 0 || h <= 0) {
            outRgb[0] = 0f;
            outRgb[1] = 0f;
            outRgb[2] = 0f;
            return 0f;
        }
        final int count = w * h;
        final int[] pixels = count <= mPixelBuffer.length ? mPixelBuffer : new int[count];
        bitmap.getPixels(pixels, 0, w, 0, 0, w, h);

        final float gamma = clamp(
                getFloatProp("persist.sys.als_correction.gamma", DEF_GAMMA),
                GAMMA_MIN, GAMMA_MAX);
        final float[] lut = getGammaLut(gamma);
        final boolean useStaticMask = !mDiscClipped && w == SAMPLE_EDGE && h == SAMPLE_EDGE;
        updateColorTransform();

        mSumR = 0;
        mSumG = 0;
        mSumB = 0;
        int samples = 0;

        if (useStaticMask) {
            for (int i = 0; i < count; i++) {
                if (!DISC_MASK[i]) {
                    continue;
                }
                addPixel(pixels[i], lut);
                samples++;
            }
        } else {
            // Pixel centers as fractions of the crop, relative to the disc set by the capture.
            final float invW = 1f / w;
            final float invH = 1f / h;
            final float invRadiusX = 1f / mDiscRadiusX;
            final float invRadiusY = 1f / mDiscRadiusY;
            for (int y = 0; y < h; y++) {
                final float dy = ((y + 0.5f) * invH - mDiscCenterY) * invRadiusY;
                for (int x = 0; x < w; x++) {
                    final float dx = ((x + 0.5f) * invW - mDiscCenterX) * invRadiusX;
                    if (dx * dx + dy * dy > 1f) {
                        continue;
                    }
                    addPixel(pixels[y * w + x], lut);
                    samples++;
                }
            }
        }

        if (samples == 0) {
            outRgb[0] = 0f;
            outRgb[1] = 0f;
            outRgb[2] = 0f;
            return 0f;
        }
        final float meanR = (float) (mSumR / samples);
        final float meanG = (float) (mSumG / samples);
        final float meanB = (float) (mSumB / samples);
        outRgb[0] = meanR;
        outRgb[1] = meanG;
        outRgb[2] = meanB;
        return (float) (RED_WEIGHT * meanR + GREEN_WEIGHT * meanG + BLUE_WEIGHT * meanB);
    }

    /** Adds one captured pixel the way the panel emits it. Capture worker only. */
    private void addPixel(int px, float[] lut) {
        final int r8 = (px >> 16) & 0xFF;
        final int g8 = (px >> 8) & 0xFF;
        final int b8 = px & 0xFF;
        final float[] m = mColorTransform;
        if (m == null) {
            mSumR += lut[r8];
            mSumG += lut[g8];
            mSumB += lut[b8];
            return;
        }
        // Column-major 4x4 as SurfaceFlinger receives it; the fourth column is an offset.
        final boolean linear = mColorTransformLinear;
        final float r = linear ? lut[r8] : r8 / 255f;
        final float g = linear ? lut[g8] : g8 / 255f;
        final float b = linear ? lut[b8] : b8 / 255f;
        final float tr = clamp(m[0] * r + m[4] * g + m[8] * b + m[12], 0f, 1f);
        final float tg = clamp(m[1] * r + m[5] * g + m[9] * b + m[13], 0f, 1f);
        final float tb = clamp(m[2] * r + m[6] * g + m[10] * b + m[14], 0f, 1f);
        mSumR += linear ? tr : lut[Math.round(tr * 255f)];
        mSumG += linear ? tg : lut[Math.round(tg * 255f)];
        mSumB += linear ? tb : lut[Math.round(tb * 255f)];
    }

    /**
     * SurfaceFlinger applies the display color transform (night light, color inversion, extra
     * dim, saturation and so on) to the panel but renders screenshots without it. Rebuild it,
     * so that the sampled pixels are the ones the panel emits: with inverted colors the
     * screenshot of a white screen is white while the panel is black. Capture worker only.
     *
     * The transform is SurfaceFlinger's client matrix (everything DisplayTransformManager
     * holds, composed in ascending level order) times its global saturation matrix. Not
     * covered: the daltonizer of the color-correction accessibility modes, which is known
     * only inside SurfaceFlinger and leaves neutral colors unchanged.
     */
    private void updateColorTransform() {
        float[] transform = null;
        boolean linear = true;
        try {
            final DisplayTransformManager dtm =
                    LocalServices.getService(DisplayTransformManager.class);
            if (dtm != null) {
                linear = dtm.needsLinearColorMatrix();
                for (int level = 0; level < COLOR_MATRIX_LEVEL_LIMIT; level++) {
                    final float[] matrix = dtm.getColorMatrix(level);
                    if (matrix != null && matrix.length == 16) {
                        transform = multiply(transform, matrix);
                    }
                }
            }
            final float saturation = Math.max(0f, getFloatProp(PARTS_SATURATION_PROP,
                    getFloatProp(SF_SATURATION_PROP, 1f)));
            if (saturation != 1f) {
                // SurfaceFlinger::calculateColorMatrix()
                final float lr = 0.213f * (1f - saturation);
                final float lg = 0.715f * (1f - saturation);
                final float lb = 0.072f * (1f - saturation);
                final float[] s = mSaturationMatrix;
                Matrix.setIdentityM(s, 0);
                s[0] = lr + saturation;
                s[1] = lr;
                s[2] = lr;
                s[4] = lg;
                s[5] = lg + saturation;
                s[6] = lg;
                s[8] = lb;
                s[9] = lb;
                s[10] = lb + saturation;
                transform = multiply(transform, s);
            }
        } catch (RuntimeException | LinkageError e) {
            // An unreadable transform must not stop the correction of ordinary content.
            if (!mColorTransformFailedLogged) {
                mColorTransformFailedLogged = true;
                Slog.w(TAG, "display color transform unavailable, assuming none", e);
            }
            transform = null;
        }
        mColorTransform = isUsableTransform(transform) ? transform : null;
        mColorTransformLinear = linear;
    }

    /** lhs * rhs into one of two scratch matrices; a null lhs stands for identity. */
    private float[] multiply(float[] lhs, float[] rhs) {
        if (lhs == null) {
            System.arraycopy(rhs, 0, mMatrixA, 0, 16);
            return mMatrixA;
        }
        final float[] result = lhs == mMatrixA ? mMatrixB : mMatrixA;
        Matrix.multiplyMM(result, 0, lhs, 0, rhs, 0);
        return result;
    }

    /** False for no transform, a broken one, or one that changes nothing visible. */
    private static boolean isUsableTransform(float[] m) {
        if (m == null) {
            return false;
        }
        boolean identity = true;
        // Only the three color rows and the offset column are applied.
        for (int i = 0; i < 16; i++) {
            if (i % 4 == 3) {
                continue;
            }
            if (!Float.isFinite(m[i])) {
                return false;
            }
            if (Math.abs(m[i] - (i == 0 || i == 5 || i == 10 ? 1f : 0f)) > 0.005f) {
                identity = false;
            }
        }
        return !identity;
    }

    /** Current panel DBV normalized to 0..1, or -1 if it cannot be read reliably. */
    private float getBacklightNorm(byte[] sysfsBuf) {
        final float cur = readSysfsFloat(BACKLIGHT_PATH, sysfsBuf);
        if (cur <= 0f) {
            return cur;
        }
        float max = mMaxBacklight;
        if (max <= 0f) {
            max = readSysfsFloat(MAX_BACKLIGHT_PATH, sysfsBuf);
            mMaxBacklight = max;
        }
        return max > 0f ? Math.min(1f, cur / max) : -1f;
    }

    private static float readSysfsFloat(String path, byte[] buf) {
        try (FileInputStream is = new FileInputStream(path)) {
            final int n = is.read(buf);
            if (n <= 0) {
                return -1f;
            }
            int value = 0;
            boolean hasDigits = false;
            for (int i = 0; i < n; i++) {
                final byte b = buf[i];
                if (b >= '0' && b <= '9') {
                    value = value * 10 + (b - '0');
                    hasDigits = true;
                } else if (b == ' ' || b == '\n' || b == '\r' || b == '\t') {
                    if (hasDigits) {
                        break;
                    }
                } else {
                    // Fallback for non-integer formatting.
                    final float parsed = Float.parseFloat(
                            new String(buf, 0, n, StandardCharsets.UTF_8).trim());
                    return Float.isFinite(parsed) && parsed >= 0f ? parsed : -1f;
                }
            }
            return hasDigits ? (float) value : -1f;
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
                    + ", colorTransform=" + mColorTransformApplied
                    + ", runLeakage=" + (mRunStartElapsed < 0 ? Float.NaN : mRunLeakage)
                    + ", runAgeMs=" + ageMs(now, mRunStartElapsed)
                    + ", alsSamples=" + mAlsSamples
                    + ", reevaluateRequests=" + mReevaluateRequests
                    + ", reevaluated=" + mReevaluatedSamples
                    + ", cacheMisses=" + mCacheMisses
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
