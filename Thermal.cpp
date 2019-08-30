/*
 * Copyright (C) 2017 The Android Open Source Project
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

#include <errno.h>
#include <math.h>
#include <vector>
#include <log/log.h>
#include <hardware/hardware.h>
#include <hardware/thermal.h>
#include <inttypes.h>

#include "Thermal.h"

#define MAX_LENGTH              50
#define TEMP_NAME_LENGTH        15

#define CPU_USAGE_FILE          "/proc/stat"
#define TEMPERATURE_DIR         "/sys/class/thermal"
#define THERMAL_DIR             "thermal_zone"
#define CPU_ONLINE_FILE_FORMAT  "/sys/devices/system/cpu/cpu%d/online"
#define UNKNOWN_LABEL           "UNKNOWN"
#define THROTTLING_THRESHOLD    100
#define SHUTDOWN_THRESHOLD      120


namespace android {
namespace hardware {
namespace thermal {
namespace V1_1 {
namespace renesas {

sp<IThermalCallback> Thermal::sThermalCb;

static float finalizeTemperature(float temperature) {
    return (temperature == UNKNOWN_TEMPERATURE) ? NAN : temperature;
}

Thermal::Thermal() {}

// Methods from ::android::hardware::thermal::V1_1::IThermal follow.
Return<void> Thermal::getTemperatures(getTemperatures_cb _hidl_cb) {
    ThermalStatus status;
    status.code = V1_0::ThermalStatusCode::SUCCESS;
    hidl_vec<Temperature> temperatures_reply;
    std::vector<Temperature> temperatures;

    char file_name[MAX_LENGTH];
    FILE *file;
    float temp = 0.f;
    char name [TEMP_NAME_LENGTH] = {0};
    DIR *dir;
    struct dirent *de;

    dir = opendir(TEMPERATURE_DIR);
    if (dir == nullptr) {
        ALOGE("%s: failed to open directory %s: %s", __func__, TEMPERATURE_DIR, strerror(-errno));
        status.code = V1_0::ThermalStatusCode::FAILURE;
        _hidl_cb(status, temperatures);
        return Void();
    }

    while ((de = readdir(dir))) {
        if (!strncmp(de->d_name, THERMAL_DIR, strlen(THERMAL_DIR))) {
            snprintf(file_name, MAX_LENGTH, "%s/%s/temp", TEMPERATURE_DIR, de->d_name);
            file = fopen(file_name, "r");
            if (file == nullptr) {
                continue;
            }
            if (1 != fscanf(file, "%f", &temp)) {
                fclose(file);
                continue;
            }

            fclose(file);
            snprintf(file_name, MAX_LENGTH, "%s/%s/type", TEMPERATURE_DIR, de->d_name);
            file = fopen(file_name, "r");
            if (1 != fscanf(file, "%s", name)) {
                fclose(file);
                continue;
            }

            Temperature temperature;
            temperature.type = V1_0::TemperatureType::CPU;
            temperature.name = std::string(name);
            temperature.currentValue = temp / 1000.f;
            temperature.throttlingThreshold = finalizeTemperature(THROTTLING_THRESHOLD);
            temperature.shutdownThreshold = finalizeTemperature(SHUTDOWN_THRESHOLD);
            temperature.vrThrottlingThreshold = finalizeTemperature(UNKNOWN_TEMPERATURE);

            temperatures.push_back(temperature);

        }
    }

    if (temperatures.size() == 0) {
        status.code = V1_0::ThermalStatusCode::FAILURE;
        status.debugMessage = strerror(-ENOENT);
    }
    temperatures_reply.setToExternal(temperatures.data(), temperatures.size());
    _hidl_cb(status, temperatures_reply);
    return Void();
}

Return<void> Thermal::getCpuUsages(getCpuUsages_cb _hidl_cb) {
    ThermalStatus status;
    hidl_vec<CpuUsage> cpuUsages_reply;
    std::vector<CpuUsage> cpuUsages;
    status.code = V1_0::ThermalStatusCode::SUCCESS;

    int vals, cpu_num, online = 0;
    ssize_t read;
    uint64_t user, nice, system, idle, active, total;
    char *line = nullptr;
    size_t len = 0;
    char file_name[MAX_LENGTH];
    char cpu_name[5];
    FILE *cpu_file;
    FILE *file = fopen(CPU_USAGE_FILE, "r");

    if (file == nullptr) {
        ALOGE("%s: failed to open: %s", __func__, strerror(errno));
         _hidl_cb(status, cpuUsages);
        return Void();
    }

    while ((read = getline(&line, &len, file)) != -1) {
        // Skip non "cpu[0-9]" lines.
        if (strnlen(line, read) < 4 || strncmp(line, "cpu", 3) != 0 || !isdigit(line[3])) {
            free(line);
            line = nullptr;
            len = 0;
            continue;
        }
        vals = sscanf(line, "cpu%d %" SCNu64 " %" SCNu64 " %" SCNu64 " %" SCNu64, &cpu_num, &user,
                &nice, &system, &idle);

        free(line);
        line = nullptr;
        len = 0;

        if (vals != 5) {
            ALOGE("%s: failed to read CPU information from file: %s", __func__, strerror(errno));
            fclose(file);
            status.code = V1_0::ThermalStatusCode::FAILURE;
            status.debugMessage = strerror(-EIO);
            _hidl_cb(status, cpuUsages);
            return Void();
        }

        active = user + nice + system;
        total = active + idle;

        // Read online CPU information.
        snprintf(file_name, MAX_LENGTH, CPU_ONLINE_FILE_FORMAT, cpu_num);
        cpu_file = fopen(file_name, "r");
        online = 0;

        snprintf(cpu_name, sizeof(cpu_name), "CPU%d", cpu_num);
        if (cpu_file == nullptr) {
            ALOGE("%s: failed to open file: %s (%s)", __func__, file_name, strerror(errno));
            online = cpu_num == 0;
        } else if (1 != fscanf(cpu_file, "%d", &online)) {
            ALOGE("%s: failed to read CPU online information from file: %s (%s)", __func__,
                    file_name, strerror(errno));
            fclose(file);
            fclose(cpu_file);
            status.code = V1_0::ThermalStatusCode::FAILURE;
            status.debugMessage = strerror(-EIO);
            _hidl_cb(status, cpuUsages);
            return Void();
        } else {
            fclose(cpu_file);
        }

        CpuUsage usage;
        usage.name = cpu_name;
        usage.active = active;
        usage.total = total;
        usage.isOnline = (online != 0) ? true : false;
        cpuUsages.push_back(usage);
    }
    cpuUsages_reply.setToExternal(cpuUsages.data(), cpuUsages.size());
    _hidl_cb(status, cpuUsages_reply);
    return Void();
}

Return<void> Thermal::getCoolingDevices(getCoolingDevices_cb _hidl_cb) {
    ThermalStatus status;
    status.code = V1_0::ThermalStatusCode::SUCCESS;
    hidl_vec<CoolingDevice> coolingDevices;
    _hidl_cb(status, coolingDevices);
    return Void();

}

Return<void> Thermal::registerThermalCallback(const sp<IThermalCallback>& callback)
{
    if (callback == nullptr)  {
        ALOGE("%s: Null callback ignored", __func__);
        return Void();
    }

    sThermalCb = callback;

    Temperature temperature;
    temperature.type = V1_0::TemperatureType::CPU;
    temperature.name = "thermal";
    temperature.currentValue = finalizeTemperature(UNKNOWN_TEMPERATURE);
    temperature.throttlingThreshold = finalizeTemperature(THROTTLING_THRESHOLD);
    temperature.shutdownThreshold = finalizeTemperature(SHUTDOWN_THRESHOLD);
    temperature.vrThrottlingThreshold = finalizeTemperature(UNKNOWN_TEMPERATURE);

    sThermalCb->notifyThrottling(false, temperature);

    return Void();
}

}  // namespace renesas
}  // namespace V1_1
}  // namespace thermal
}  // namespace hardware
}  // namespace android
