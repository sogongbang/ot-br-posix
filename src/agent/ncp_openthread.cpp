/*
 *    Copyright (c) 2019, The OpenThread Authors.
 *    All rights reserved.
 *
 *    Redistribution and use in source and binary forms, with or without
 *    modification, are permitted provided that the following conditions are met:
 *    1. Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *    2. Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *    3. Neither the name of the copyright holder nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 *    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *    POSSIBILITY OF SUCH DAMAGE.
 */

#include "agent/ncp_openthread.hpp"

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <openthread/cli.h>
#include <openthread/dataset.h>
#include <openthread/tasklet.h>
#include <openthread/thread.h>
#include <openthread/thread_ftd.h>
#include <openthread/platform/logging.h>
#include <openthread/platform/misc.h>
#include <openthread/platform/settings.h>

#include "common/code_utils.hpp"
#include "common/logging.hpp"
#include "common/types.hpp"

static bool sReset;
using std::chrono::duration_cast;
using std::chrono::microseconds;
using std::chrono::seconds;
using std::chrono::steady_clock;

namespace otbr {
namespace Ncp {

ControllerOpenThread::ControllerOpenThread(const char *aInterfaceName, char *aRadioFile, char *aRadioConfig)
    : mTriedAttach(false)
{
    memset(&mConfig, 0, sizeof(mConfig));

    mConfig.mInterfaceName = aInterfaceName;
    mConfig.mRadioConfig   = aRadioConfig;
    mConfig.mRadioFile     = aRadioFile;
    mConfig.mResetRadio    = true;
    mConfig.mSpeedUpFactor = 1;
}

ControllerOpenThread::~ControllerOpenThread(void)
{
    otInstanceFinalize(mInstance);
    otSysDeinit();
}

otbrError ControllerOpenThread::Init(void)
{
    otbrError error = OTBR_ERROR_NONE;

    mInstance = otSysInit(&mConfig);
    otCliUartInit(mInstance);

    {
        otError result = otSetStateChangedCallback(mInstance, &ControllerOpenThread::HandleStateChanged, this);

        agent::ThreadHelper::LogOpenThreadResult("Set state callback", result);
        VerifyOrExit(result == OT_ERROR_NONE, error = OTBR_ERROR_OPENTHREAD);
    }

    mThreadHelper = std::unique_ptr<otbr::agent::ThreadHelper>(new otbr::agent::ThreadHelper(mInstance, this));

exit:
    return error;
}

void ControllerOpenThread::HandleStateChanged(otChangedFlags aFlags)
{
    if (aFlags & OT_CHANGED_THREAD_NETWORK_NAME)
    {
        EventEmitter::Emit(kEventNetworkName, otThreadGetNetworkName(mInstance));
    }

    if (aFlags & OT_CHANGED_THREAD_EXT_PANID)
    {
        EventEmitter::Emit(kEventExtPanId, otThreadGetExtendedPanId(mInstance));
    }

    if (aFlags & OT_CHANGED_THREAD_ROLE)
    {
        bool attached = false;

        switch (otThreadGetDeviceRole(mInstance))
        {
        case OT_DEVICE_ROLE_CHILD:
        case OT_DEVICE_ROLE_ROUTER:
        case OT_DEVICE_ROLE_LEADER:
            attached = true;
            break;
        default:
            break;
        }

        EventEmitter::Emit(kEventThreadState, attached);
    }

    mThreadHelper->StateChangedCallback(aFlags);
}

static struct timeval ToTimeVal(const microseconds &aTime)
{
    constexpr int  kUsPerSecond = 1000000;
    struct timeval val;

    val.tv_sec  = aTime.count() / kUsPerSecond;
    val.tv_usec = aTime.count() % kUsPerSecond;

    return val;
};

void ControllerOpenThread::UpdateFdSet(otSysMainloopContext &aMainloop)
{
    microseconds timeout = microseconds(aMainloop.mTimeout.tv_usec) + seconds(aMainloop.mTimeout.tv_sec);
    auto         now     = steady_clock::now();

    if (otTaskletsArePending(mInstance))
    {
        timeout = microseconds::zero();
    }
    else if (!mTimers.empty())
    {
        if (mTimers.begin()->first < now)
        {
            timeout = microseconds::zero();
        }
        else
        {
            timeout = std::min(timeout, duration_cast<microseconds>(mTimers.begin()->first - now));
        }
    }

    aMainloop.mTimeout = ToTimeVal(timeout);

    otSysMainloopUpdate(mInstance, &aMainloop);
}

void ControllerOpenThread::Process(const otSysMainloopContext &aMainloop)
{
    auto now = steady_clock::now();

    otTaskletsProcess(mInstance);

    otSysMainloopProcess(mInstance, &aMainloop);

    while (!mTimers.empty() && mTimers.begin()->first <= now)
    {
        mTimers.begin()->second();
        mTimers.erase(mTimers.begin());
    }

    if (!mTriedAttach && mThreadHelper->TryResumeNetwork() == OT_ERROR_NONE)
    {
        mTriedAttach = true;
    }
}

void ControllerOpenThread::Reset(void)
{
    otInstanceFinalize(mInstance);
    otSysDeinit();
    Init();
    sReset = false;
}

bool ControllerOpenThread::IsResetRequested(void)
{
    return sReset;
}

otbrError ControllerOpenThread::RequestEvent(int aEvent)
{
    otbrError ret = OTBR_ERROR_NONE;

    switch (aEvent)
    {
    case kEventExtPanId:
    {
        EventEmitter::Emit(kEventExtPanId, otThreadGetExtendedPanId(mInstance));
        break;
    }
    case kEventThreadState:
    {
        bool attached = false;

        switch (otThreadGetDeviceRole(mInstance))
        {
        case OT_DEVICE_ROLE_CHILD:
        case OT_DEVICE_ROLE_ROUTER:
        case OT_DEVICE_ROLE_LEADER:
            attached = true;
            break;
        default:
            break;
        }

        EventEmitter::Emit(kEventThreadState, attached);
        break;
    }
    case kEventNetworkName:
    {
        EventEmitter::Emit(kEventNetworkName, otThreadGetNetworkName(mInstance));
        break;
    }
    case kEventPSKc:
    {
        EventEmitter::Emit(kEventPSKc, otThreadGetPskc(mInstance));
        break;
    }
    case kEventThreadVersion:
    {
        EventEmitter::Emit(kEventThreadVersion, otThreadGetVersion());
        break;
    }
    default:
        assert(false);
        break;
    }

    return ret;
}

void ControllerOpenThread::PostTimerTask(std::chrono::steady_clock::time_point aTimePoint,
                                         const std::function<void(void)> &     aTask)
{
    mTimers.insert({aTimePoint, aTask});
}

Controller *Controller::Create(const char *aInterfaceName, char *aRadioFile, char *aRadioConfig)
{
    return new ControllerOpenThread(aInterfaceName, aRadioFile, aRadioConfig);
}

/*
 * Provide, if required an "otPlatLog()" function
 */
extern "C" void otPlatLog(otLogLevel aLogLevel, otLogRegion aLogRegion, const char *aFormat, ...)
{
    OT_UNUSED_VARIABLE(aLogRegion);

    int otbrLogLevel;

    switch (aLogLevel)
    {
    case OT_LOG_LEVEL_NONE:
        otbrLogLevel = OTBR_LOG_EMERG;
        break;
    case OT_LOG_LEVEL_CRIT:
        otbrLogLevel = OTBR_LOG_CRIT;
        break;
    case OT_LOG_LEVEL_WARN:
        otbrLogLevel = OTBR_LOG_WARNING;
        break;
    case OT_LOG_LEVEL_NOTE:
        otbrLogLevel = OTBR_LOG_NOTICE;
        break;
    case OT_LOG_LEVEL_INFO:
        otbrLogLevel = OTBR_LOG_INFO;
        break;
    case OT_LOG_LEVEL_DEBG:
        otbrLogLevel = OTBR_LOG_DEBUG;
        break;
    default:
        otbrLogLevel = OTBR_LOG_DEBUG;
        break;
    }

    va_list ap;
    va_start(ap, aFormat);
    otbrLogv(otbrLogLevel, aFormat, ap);
    va_end(ap);
}

} // namespace Ncp
} // namespace otbr

void otPlatReset(otInstance *aInstance)
{
    OT_UNUSED_VARIABLE(aInstance);
    sReset = true;
}
