/*
 * Copyright (C) 2018 The Android Open Source Project
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

#include "Thermal.h"

#include <cmath>
#include <errno.h>
#include <fstream>
#include <inttypes.h>
#include <regex>
#include <set>
#include <vector>

#include <android-base/logging.h>
#include <automotive/filesystem>
#include <hardware/hardware.h>
#include <hardware/thermal.h>
#include <hidl/HidlTransportSupport.h>
#include <log/log.h>

namespace filesystem = android::hardware::automotive::filesystem;

static const auto kCpuUsageFile("/proc/stat");
static const std::regex txt_regex("cpu([0-9]+)( [0-9]+)( [0-9]+)( [0-9]+)( [0-9]+)(.*)");
static const std::string kTemperaturePath = "/sys/class/thermal";
static const std::string kThermalZone = "thermal_zone";
static const float kThrottlingThreshold = 100;
static const float kShutdownThreshold = 120;

namespace android {
namespace hardware {
namespace thermal {
namespace V2_0 {
namespace renesas {

using ::android::sp;
using ::android::hardware::interfacesEqual;
using ::android::hardware::thermal::V1_0::ThermalStatus;
using ::android::hardware::thermal::V1_0::ThermalStatusCode;

std::set<sp<IThermalChangedCallback>> gCallbacks;

static constexpr float finalizeTemperature(float temperature) {
    return (temperature == UNKNOWN_TEMPERATURE) ? NAN : temperature;
}

// Methods from ::android::hardware::thermal::V1_0::IThermal follow.
Return<void> Thermal::getTemperatures(getTemperatures_cb _hidl_cb) {
    ThermalStatus status;
    status.code = V1_0::ThermalStatusCode::SUCCESS;
    hidl_vec<Temperature_1_0> temperaturesReply;
    std::vector<Temperature_1_0> temperatures = getTemperaturesHelper();

    if (temperatures.size() == 0) {
        status.code = V1_0::ThermalStatusCode::FAILURE;
        status.debugMessage = strerror(-ENOENT);
    }

    temperaturesReply.setToExternal(temperatures.data(), temperatures.size());
    _hidl_cb(status, temperaturesReply);
    return Void();
}

Return<void> Thermal::getCpuUsages(getCpuUsages_cb _hidl_cb) {
    ThermalStatus status;
    hidl_vec<CpuUsage> cpuUsagesReply;
    std::vector<CpuUsage> cpuUsages;
    status.code = ThermalStatusCode::SUCCESS;

    int cpuNum, isOnline = 0;
    uint64_t user, nice, system, idle, active, total;
    std::string cpuName;

    std::ifstream fs;
    std::string tmp;
    fs.open(kCpuUsageFile);
    std::smatch sm;

    if (fs.fail()) {
        LOG(ERROR) << "failed to open: " << kCpuUsageFile;
        status.code = V1_0::ThermalStatusCode::FAILURE;
         _hidl_cb(status, cpuUsages);
        return Void();
    }

    while(getline(fs, tmp)) {
        if (!std::regex_search(tmp, sm, txt_regex) || sm.size() < 6) {
            continue;
        }
        cpuNum = stoi(sm.str(1));
        user = std::stoull(sm.str(2));
        nice = std::stoull(sm.str(3));
        system = std::stoull(sm.str(4));
        idle = std::stoull(sm.str(5));

        active = user + nice + system;
        total = active + idle;
        cpuName = "CPU" + sm.str(1);

        auto cpuOnlineFileName = "/sys/devices/system/cpu/cpu" + sm.str(1) + "/online";
        std::ifstream cpuOnlineFs(cpuOnlineFileName);
        if (cpuOnlineFs.fail()) {
            LOG(ERROR) << "failed to open: " << cpuOnlineFileName;
            isOnline = cpuNum == 0;
        }
        cpuOnlineFs >> isOnline;
        cpuOnlineFs.close();

        CpuUsage usage;
        usage.name = cpuName;
        usage.active = active;
        usage.total = total;
        usage.isOnline = (isOnline != 0);
        cpuUsages.push_back(usage);
    }
    fs.close();

    cpuUsagesReply.setToExternal(cpuUsages.data(), cpuUsages.size());
    _hidl_cb(status, cpuUsagesReply);
    return Void();
}

Return<void> Thermal::getCoolingDevices(getCoolingDevices_cb _hidl_cb) {
    ThermalStatus status;
    status.code = ThermalStatusCode::FAILURE;
    status.debugMessage = "No cooling devices";
    std::vector<CoolingDevice_1_0> coolingDevices;
    _hidl_cb(status, coolingDevices);
    return Void();
}

// Methods from ::android::hardware::thermal::V2_0::IThermal follow.
Return<void> Thermal::getCurrentTemperatures(bool filterType, TemperatureType type,
                                             getCurrentTemperatures_cb _hidl_cb) {
    ThermalStatus status;
    status.code = ThermalStatusCode::SUCCESS;
    std::vector<Temperature_2_0> temperatures;
    std::vector<Temperature_1_0> temperatures_1_0 = getTemperaturesHelper();

    if (temperatures_1_0.size() == 0) {
        status.code = ThermalStatusCode::FAILURE;
        status.debugMessage = strerror(-ENOENT);
    } else {
        for (auto temp : temperatures_1_0) {
            TemperatureType type_1_0 = static_cast<TemperatureType>(temp.type);
            if (!filterType || type == type_1_0) {
                Temperature_2_0 val;
                val.type = type_1_0;
                val.name = temp.name;
                val.value = temp.currentValue;
                val.throttlingStatus = ThrottlingSeverity::NONE;
                temperatures.push_back(val);
            }
        }
    }

    if (temperatures.size() == 0) {
        status.code = ThermalStatusCode::FAILURE;
        status.debugMessage = strerror(-ENOENT);
    }

    _hidl_cb(status, temperatures);
    return Void();
}

Return<void> Thermal::getTemperatureThresholds(bool filterType, TemperatureType type,
                                               getTemperatureThresholds_cb _hidl_cb) {
    ThermalStatus status;
    status.code = ThermalStatusCode::SUCCESS;
    std::vector<TemperatureThreshold> temperatureThresholds;

    if (filterType && type != TemperatureType::CPU) {
        status.code = ThermalStatusCode::FAILURE;
        status.debugMessage = "Wrong filter type";
    } else {
        for (const auto & entry : filesystem::directory_iterator(kTemperaturePath)) {
            const auto filenameStr = entry.path().filename().string();
            if (filenameStr.compare(0, kThermalZone.length(), kThermalZone) == 0) {
                std::ifstream fs;
                auto typeFileName = entry.path();
                typeFileName.append("type");
                fs.open(typeFileName);
                if (fs.fail()) {
                    LOG(ERROR) << "failed to open: " << typeFileName;
                    continue;
                }
                std::string name;
                fs >> name;
                fs.close();

                TemperatureThreshold tempThreshold;
                tempThreshold.type = TemperatureType::CPU;
                tempThreshold.name = std::string(name);
                tempThreshold.hotThrottlingThresholds = {{NAN, NAN, NAN,
                    kThrottlingThreshold, NAN, NAN, kShutdownThreshold}};
                tempThreshold.coldThrottlingThresholds = {{NAN, NAN, NAN, NAN, NAN, NAN, NAN}};
                tempThreshold.vrThrottlingThreshold = NAN;
                temperatureThresholds.push_back(tempThreshold);
            }
        }
    }

    _hidl_cb(status, temperatureThresholds);
    return Void();
}

Return<void> Thermal::getCurrentCoolingDevices(bool filterType __unused,
                                               CoolingType type __unused,
                                               getCurrentCoolingDevices_cb _hidl_cb) {
    ThermalStatus status;
    status.code = ThermalStatusCode::FAILURE;
    status.debugMessage = "No cooling devices";
    std::vector<CoolingDevice_2_0> coolingDevices;
    _hidl_cb(status, coolingDevices);
    return Void();
}

Return<void> Thermal::registerThermalChangedCallback(const sp<IThermalChangedCallback>& callback,
                                                     bool filterType, TemperatureType type,
                                                     registerThermalChangedCallback_cb _hidl_cb) {
    ThermalStatus status;
    if (callback == nullptr) {
        status.code = ThermalStatusCode::FAILURE;
        status.debugMessage = "Invalid nullptr callback";
        LOG(ERROR) << status.debugMessage;
        _hidl_cb(status);
        return Void();
    } else {
        status.code = ThermalStatusCode::SUCCESS;
    }
    std::lock_guard<std::mutex> _lock(thermal_callback_mutex_);
    if (std::any_of(callbacks_.begin(), callbacks_.end(), [&](const CallbackSetting& c) {
            return interfacesEqual(c.callback, callback);
        })) {
        status.code = ThermalStatusCode::FAILURE;
        status.debugMessage = "Same callback interface registered already";
        LOG(ERROR) << status.debugMessage;
    } else {
        callbacks_.emplace_back(callback, filterType, type);
        LOG(INFO) << "A callback has been registered to ThermalHAL, isFilter: " << filterType
                  << " Type: " << android::hardware::thermal::V2_0::toString(type);
    }
    _hidl_cb(status);
    return Void();
}

Return<void> Thermal::unregisterThermalChangedCallback(
    const sp<IThermalChangedCallback>& callback, unregisterThermalChangedCallback_cb _hidl_cb) {
    ThermalStatus status;
    if (callback == nullptr) {
        status.code = ThermalStatusCode::FAILURE;
        status.debugMessage = "Invalid nullptr callback";
        LOG(ERROR) << status.debugMessage;
        _hidl_cb(status);
        return Void();
    } else {
        status.code = ThermalStatusCode::SUCCESS;
    }
    bool removed = false;
    std::lock_guard<std::mutex> _lock(thermal_callback_mutex_);
    callbacks_.erase(
        std::remove_if(callbacks_.begin(), callbacks_.end(),
                       [&](const CallbackSetting& c) {
                           if (interfacesEqual(c.callback, callback)) {
                               LOG(INFO)
                                   << "A callback has been unregistered from ThermalHAL, isFilter: "
                                   << c.is_filter_type << " Type: "
                                   << android::hardware::thermal::V2_0::toString(c.type);
                               removed = true;
                               return true;
                           }
                           return false;
                       }),
        callbacks_.end());
    if (!removed) {
        status.code = ThermalStatusCode::FAILURE;
        status.debugMessage = "The callback was not registered before";
        LOG(ERROR) << status.debugMessage;
    }
    _hidl_cb(status);
    return Void();
}

std::vector<Temperature_1_0> Thermal::getTemperaturesHelper() {
    std::vector<Temperature_1_0> temperatures;

    float temp = 0.f;

    for (const auto & entry : filesystem::directory_iterator(kTemperaturePath)) {
        const auto filenameStr = entry.path().filename().string();
        if (filenameStr.compare(0, kThermalZone.length(), kThermalZone) == 0) {
            auto tempFileName = entry.path();
            tempFileName.append("temp");
            std::ifstream fs(tempFileName);
            fs.open(tempFileName);
            if (fs.fail()) {
                LOG(ERROR) << "failed to open: " << tempFileName;
                continue;
            }
            fs >> temp;
            fs.close();
            auto typeFileName = entry.path();
            typeFileName.append("type");
            fs.open(typeFileName);
            if (fs.fail()) {
                LOG(ERROR) << "failed to open: " << typeFileName;
                continue;
            }
            std::string name;
            fs >> name;
            fs.close();

            Temperature_1_0 temperature;
            temperature.type = V1_0::TemperatureType::CPU;
            temperature.name = std::string(name);
            temperature.currentValue = temp / 1000.f;
            temperature.throttlingThreshold = finalizeTemperature(kThrottlingThreshold);
            temperature.shutdownThreshold = finalizeTemperature(kShutdownThreshold);
            temperature.vrThrottlingThreshold = finalizeTemperature(UNKNOWN_TEMPERATURE);

            temperatures.push_back(temperature);
        }
    }
    return temperatures;
}

}  // namespace renesas
}  // namespace V2_0
}  // namespace thermal
}  // namespace hardware
}  // namespace android
