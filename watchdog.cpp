/*
 *  Copyright 2021 Google, Inc
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#define LOG_TAG "lowmemorykiller"

#include <errno.h>
#include <log/log.h>
#include <string.h>
#include <system/thread_defs.h>
#include <unistd.h>

#include "ax_process_utils.h"
#include "watchdog.h"

static void* watchdog_main(void* param) {
    Watchdog *watchdog = static_cast<Watchdog*>(param);
    sigset_t sigset;
    int signum;

    pid_t tid = gettid();
    if (!axion::process::SetThreadProfile(tid, "CPUSET_SP_TOP_APP", true)
            && !axion::process::SetThreadProfile(tid, "CPUSET_SP_FOREGROUND", true)) {
        ALOGE("Failed to assign cpuset to the watchdog thread");
    }
    if (!axion::process::SetThreadPriority(tid, ANDROID_PRIORITY_HIGHEST)) {
        ALOGW("Unable to raise watchdog thread priority (%d): errno=%d", tid, errno);
    }

    if (!watchdog->create_timer(sigset)) {
        ALOGE("Watchdog timer creation failed!");
        return NULL;
    }

    while (true) {
        if (sigwait(&sigset, &signum) == -1) {
            ALOGE("sigwait failed: %s", strerror(errno));
        }

        watchdog->bite();
    }

    return NULL;
}

bool Watchdog::init() {
    pthread_t thread;

    if (pthread_create(&thread, NULL, watchdog_main, this)) {
        ALOGE("pthread_create failed: %s", strerror(errno));
        return false;
    }
    if (pthread_setname_np(thread, "lmkd_watchdog")) {
        ALOGW("pthread_setname_np failed: %s", strerror(errno));
    }

    return true;
}

bool Watchdog::start() {
    // Start the timer and keep it active until it's disarmed
    struct itimerspec new_timer;

    if (!timer_created_) {
        return false;
    }

    new_timer.it_value.tv_sec = timeout_;
    new_timer.it_value.tv_nsec = 0;
    new_timer.it_interval.tv_sec = timeout_;
    new_timer.it_interval.tv_nsec = 0;

    if (timer_settime(timer_, 0, &new_timer, NULL)) {
        ALOGE("timer_settime failed: %s", strerror(errno));
        return false;
    }

    return true;
}

bool Watchdog::stop() {
    struct itimerspec new_timer = {};

    if (!timer_created_) {
        return false;
    }

    if (timer_settime(timer_, 0, &new_timer, NULL)) {
        ALOGE("timer_settime failed: %s", strerror(errno));
        return false;
    }

    return true;
}

bool Watchdog::create_timer(sigset_t &sigset) {
    struct sigevent sevent;

    sigemptyset(&sigset);
    sigaddset(&sigset, SIGALRM);
    if (sigprocmask(SIG_BLOCK, &sigset, NULL)) {
        ALOGE("sigprocmask failed: %s", strerror(errno));
        return false;
    }

    sevent.sigev_notify = SIGEV_THREAD_ID;
    sevent.sigev_notify_thread_id = gettid();
    sevent.sigev_signo = SIGALRM;
    if (timer_create(CLOCK_MONOTONIC, &sevent, &timer_)) {
        ALOGE("timer_create failed: %s", strerror(errno));
        return false;
    }

    timer_created_ = true;
    return true;
}
