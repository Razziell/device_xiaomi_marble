/*
 * Copyright (C) 2024 The LineageOS Project
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "RawLightNotifier"

#include "RawLightNotifier.h"

#include <android-base/logging.h>
#include <android-base/unique_fd.h>
#include <android-base/scopeguard.h>
#include <cerrno>
#include <display/drm/mi_disp.h>
#include <mutex>
#include <utils/SystemClock.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <vector>

#include "SensorNotifierUtils.h"
#include "SscCalApi.h"

static const std::string kDispFeatureDevice =
        "/dev/mi_display/disp_feature";

// sensor: xiaomi.sensor.ambientlight.raw
static const uint32_t kSensorTypeAmbientlightRaw = 33171111;

// Request the factory ALS at 20 Hz. Reverse engineering of
// libssccalapi@2.0.so showed that its TCS3701 calibration path requests sensor
// data with a 50,000 us period. An automated test on marble also measured an
// effective factory-event interval of about 100 ms even with the old 20,000 us
// request, so 20 Hz retains sufficient input headroom without requesting an
// unnecessary nominal 50 Hz stream.
static constexpr int32_t kRawAlsSamplePeriodUs = 50000;

using android::hardware::Return;
using android::hardware::Void;
using android::hardware::sensors::V1_0::Event;

namespace {

class RawLightSensorCallback : public IEventQueueCallback {
  public:
    void track(bool active) {
        std::lock_guard<std::mutex> lock(mutex_);
        since_ = active ? android::elapsedRealtimeNano() : -1;
    }

    Return<void> onEvent(const Event& e) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (since_ < 0 || e.timestamp < since_) return Void();
        _oem_msg msg = {};
        msg.notifyType = REPORT_VALUE;
        msg.value = e.u.vec4.y;
        msg.notifyTypeFloat = msg.notifyType;
        msg.unknown1 = 2;
        msg.unknown2 = 5;
        msg.sensorType = kSensorTypeAmbientlightRaw;

        SscCalApiWrapper::getInstance().processMsg(&msg);

        return Void();
    }

  private:
    std::mutex mutex_;
    int64_t since_ = -1;
};

}  // namespace

RawLightNotifier::RawLightNotifier(sp<ISensorManager> manager)
    : SensorNotifier(manager) {}

RawLightNotifier::~RawLightNotifier() {
    deactivate();
}

void RawLightNotifier::notify() {
    sp<RawLightSensorCallback> callback = new RawLightSensorCallback();
    if (initializeSensorQueue(
            "xiaomi.sensor.ambientlight.factory",
            false,
            callback) != Result::OK) {
        return;
    }

    android::base::unique_fd disp_fd_ =
            android::base::unique_fd(
                    open(kDispFeatureDevice.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC));
    if (disp_fd_.get() == -1) {
        PLOG(ERROR) << "failed to open "
                    << kDispFeatureDevice;
        return;
    }

    // Boled accepts only BRIGHTNESS and REPORT_VALUE. POWER remains registered
    // solely to manage the raw sensor lifecycle; FPS and DC are not needed.
    const std::vector<disp_event_type> notifyEvents = {
            MI_DISP_EVENT_POWER,
            MI_DISP_EVENT_51_BRIGHTNESS,
            MI_DISP_EVENT_HBM,
    };

    // Register for primary display events.
    for (const disp_event_type& event : notifyEvents) {
        disp_event_req req = {};
        req.base.flag = event == MI_DISP_EVENT_POWER ? MI_DISP_FLAG_REPLAY_POWER : 0;
        req.base.disp_id = MI_DISP_PRIMARY;
        req.type = event;

        if (ioctl(disp_fd_.get(),
                  MI_DISP_IOCTL_REGISTER_EVENT,
                  &req) < 0) {
            PLOG(ERROR) << "failed to register display event "
                        << static_cast<int>(event);
            return;
        }
    }

    struct pollfd pollfds[] = {
            {
                    .fd = disp_fd_.get(),
                    .events = POLLIN,
                    .revents = 0,
            },
            {
                    .fd = stopEventFd(),
                    .events = POLLIN,
                    .revents = 0,
            },
    };

    bool isEnable = false;
    _oem_msg msg = {};
    notify_t notifyType;
    float value;

    // Invalidate in-flight callbacks on every exit, including HAL transaction failures.
    auto closeGate = android::base::make_scope_guard([&] { callback->track(false); });
    while (isActive()) {
        int rc = poll(pollfds, 2, -1);
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }

            PLOG(ERROR) << "failed to poll "
                        << kDispFeatureDevice;
            break;
        }

        if (pollfds[1].revents & POLLIN) {
            consumeStopEvent();
            break;
        }

        if (pollfds[1].revents &
                (POLLERR | POLLHUP | POLLNVAL)) {
            LOG(ERROR) << "stop eventfd failed, revents="
                       << pollfds[1].revents;
            break;
        }

        if (pollfds[0].revents &
                (POLLERR | POLLHUP | POLLNVAL)) {
            LOG(ERROR) << "display event fd failed, revents="
                       << pollfds[0].revents;
            break;
        }

        if (!(pollfds[0].revents & POLLIN)) {
            continue;
        }

        auto response = parseDispEvent(disp_fd_.get());
        if (!response) {
            break;
        }

        if (response->base.type == MI_DISP_EVENT_POWER) {
            switch (response->value) {
                case MI_DISP_POWER_ON:
                    if (!isEnable) {
                        callback->track(true);
                        auto enableResult = mQueue->enableSensor(
                                mSensorHandle,
                                kRawAlsSamplePeriodUs,
                                0 /* latency */);
                        if (!enableResult.isOk()) {
                            LOG(ERROR) << "enableSensor transaction failed: "
                                       << enableResult.description();
                            return;
                        } else if (enableResult != Result::OK) {
                            LOG(ERROR) << "failed to enable raw light sensor";
                            return;
                        } else {
                            isEnable = true;
                        }
                    }
                    break;

                default:
                    if (isEnable) {
                        callback->track(false);
                        auto disableResult =
                                mQueue->disableSensor(mSensorHandle);
                        if (!disableResult.isOk()) {
                            LOG(ERROR) << "disableSensor transaction failed: "
                                       << disableResult.description();
                            return;
                        } else if (disableResult != Result::OK) {
                            LOG(ERROR) << "failed to disable raw light sensor";
                            return;
                        } else {
                            isEnable = false;
                        }
                    }
                    break;
            }

            // POWER is not supported by Lux_Ams_Tcs3701_Boled. It is consumed
            // locally above and must not be forwarded to libssccalapi.
            continue;
        } else {
            switch (response->base.type) {
                case MI_DISP_EVENT_51_BRIGHTNESS:
                    notifyType = BRIGHTNESS;
                    value = response->value;
                    break;

                case MI_DISP_EVENT_HBM:
                    notifyType = BRIGHTNESS;
                    value = response->value ? -1 : -2;
                    break;

                default:
                    continue;
            }
        }

        msg.sensorType = kSensorTypeAmbientlightRaw;
        msg.notifyType = notifyType;
        msg.notifyTypeFloat = notifyType;
        msg.value = value;
        msg.unknown1 = 1;
        msg.unknown2 = 5;

        SscCalApiWrapper::getInstance().processMsg(&msg);
    }

    callback->track(false);
    if (isEnable) {
        auto disableResult = mQueue->disableSensor(mSensorHandle);
        if (!disableResult.isOk()) {
            LOG(ERROR) << "disableSensor transaction failed during shutdown: "
                       << disableResult.description();
        }

        isEnable = false;
    }
}
