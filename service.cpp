/*
 * Copyright (C) 2016 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "ThermalHAL"

#include <android-base/logging.h>
#include <android/hardware/thermal/1.1/IThermal.h>
#include <hidl/HidlSupport.h>
#include <hidl/HidlTransportSupport.h>

#include "Thermal.h"

using android::hardware::configureRpcThreadpool;
using android::hardware::joinRpcThreadpool;

using android::hardware::thermal::V1_1::IThermal;
using namespace android::hardware::thermal::V1_1::renesas;

int main() {
    android::sp<IThermal> thermal_hal = new Thermal;

    configureRpcThreadpool(1, true);

    const auto status = thermal_hal->registerAsService();
    CHECK_EQ(status, android::OK) << "Failed to register IThermal";

    joinRpcThreadpool();
}
