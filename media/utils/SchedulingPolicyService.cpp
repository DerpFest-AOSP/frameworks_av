/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define LOG_TAG "SchedulingPolicyService"
//#define LOG_NDEBUG 0

#include <audio_utils/threads.h>
#include <binder/IServiceManager.h>
#include <cutils/android_filesystem_config.h>
#include <cutils/properties.h>
#include <utils/Mutex.h>
#include "ISchedulingPolicyService.h"
#include "mediautils/SchedulingPolicyService.h"

namespace android {

static sp<ISchedulingPolicyService> sSchedulingPolicyService;
static const String16 _scheduling_policy("scheduling_policy");
static Mutex sMutex;

int requestPriority(pid_t pid, pid_t tid, int32_t prio, bool isForApp, bool asynchronous)
{
    // audioserver thread priority boosted internally to reduce binder latency and boot time.
    if (!isForApp && pid == getpid() && getuid() == AID_AUDIOSERVER && prio >= 1 && prio <= 3) {
        const status_t status = audio_utils::set_thread_priority(
                tid, audio_utils::rtprio_to_unified_priority(prio));
        if (status == NO_ERROR) return NO_ERROR;
        ALOGD("%s: set priority %d, status:%d needs to fallback to SchedulingPolicyService",
                __func__, prio, status);
    }

    // FIXME merge duplicated code related to service lookup, caching, and error recovery
    int ret;
    for (;;) {
        sMutex.lock();
        sp<ISchedulingPolicyService> sps = sSchedulingPolicyService;
        sMutex.unlock();
        if (sps == 0) {
            sp<IBinder> binder = defaultServiceManager()->checkService(_scheduling_policy);
            if (binder == 0) {
                sleep(1);
                continue;
            }
            sps = interface_cast<ISchedulingPolicyService>(binder);
            sMutex.lock();
            sSchedulingPolicyService = sps;
            sMutex.unlock();
        }
        ret = sps->requestPriority(pid, tid, prio, isForApp, asynchronous);
        if (ret != DEAD_OBJECT) {
            break;
        }
        ALOGW("SchedulingPolicyService died");
        sMutex.lock();
        sSchedulingPolicyService.clear();
        sMutex.unlock();
    }
    return ret;
}

int requestCpusetBoost(bool enable, const sp<IBinder> &client)
{
    int ret;
    sMutex.lock();
    sp<ISchedulingPolicyService> sps = sSchedulingPolicyService;
    sMutex.unlock();
    if (sps == 0) {
        sp<IBinder> binder = defaultServiceManager()->checkService(_scheduling_policy);
        if (binder == 0) {
            return DEAD_OBJECT;
        }
        sps = interface_cast<ISchedulingPolicyService>(binder);
        sMutex.lock();
        sSchedulingPolicyService = sps;
        sMutex.unlock();
    }
    ret = sps->requestCpusetBoost(enable, client);
    if (ret != DEAD_OBJECT) {
        return ret;
    }
    ALOGW("SchedulingPolicyService died");
    sMutex.lock();
    sSchedulingPolicyService.clear();
    sMutex.unlock();
    return ret;
}

int requestSpatializerPriority(pid_t pid, pid_t tid) {
    if (pid == -1 || tid == -1) return BAD_VALUE;

    // update priority to RT if specified.
    constexpr int32_t kRTPriorityMin = 1;
    constexpr int32_t kRTPriorityMax = 3;
    const int32_t priorityBoost =
            property_get_int32("audio.spatializer.priority", kRTPriorityMin);
    if (priorityBoost >= kRTPriorityMin && priorityBoost <= kRTPriorityMax) {
        const status_t status = requestPriority(
                pid, tid, priorityBoost, false /* isForApp */, true /*asynchronous*/);
        if (status != OK) {
            ALOGW("%s: Cannot request spatializer priority boost %d, status:%d",
                    __func__, priorityBoost, status);
            return status < 0 ? status : UNKNOWN_ERROR;
        }
        return priorityBoost;
    }
    return 0;  // no boost requested
}

}   // namespace android
