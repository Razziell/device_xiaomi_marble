// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <aidl/vendor/marble/power/BnPerf.h>
#include "HintController.h"
#include <memory>
namespace marble::power {
class PerfService final : public aidl::vendor::marble::power::BnPerf {
  public:
    explicit PerfService(std::shared_ptr<HintController> controller);
    ndk::ScopedAStatus openSession(
        std::shared_ptr<aidl::vendor::marble::power::IPerfSession>* result) override;
  private:
    std::shared_ptr<HintController> mController;
};
}
