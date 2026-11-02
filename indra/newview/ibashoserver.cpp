/**
 * @file   ibashoserver.cpp
 * @brief  Running on a server without a GPU (Mesa's llvmpipe under a virtual display): a frame rate cap
 *         with a lower one while no LEAP plugin is asking anything, no web browser plugin, and a
 *         texture budget that holds. Each is a setting, off by default; Ibasho's tools set them.
 *         Written by Claude Opus 5.5, 2026-11-02.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Ibasho viewer source code: an addition to the Second Life Viewer Source Code
 * (Linden Research, Inc.), written for Ibasho by Claude (Anthropic) models.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"
#include "ibashohooks.h"

#include "llagent.h"
#include "llleap.h"
#include "llstartup.h"
#include "lltimer.h"
#include "llviewercontrol.h"

namespace ibasho
{

void limitFrameRate()
{
    // Software rendering takes every core it's given, so a frame rate cap is what leaves the machine's CPU to
    // the agent's other work. While nothing has asked the viewer anything for a while (and it isn't walking
    // or teleporting), the idle rate applies: the first request after a quiet spell waits up to one idle
    // frame, and the ones after it run at the full rate.
    static LLCachedControl<F32> max_fps(gSavedSettings, "IbashoMaxFPS", 0.f);
    static LLCachedControl<F32> idle_fps(gSavedSettings, "IbashoIdleFPS", 0.f);
    static LLCachedControl<F32> idle_after(gSavedSettings, "IbashoIdleAfter", 5.f);
    static F64 last_frame_end = 0;

    F64 now = LLTimer::getTotalSeconds();
    F32 fps = max_fps;
    if (idle_fps > 0.f && (fps <= 0.f || idle_fps < fps)
        && LLStartUp::getStartupState() == STATE_STARTED
        && now - LLLeap::lastRequestSeconds() > idle_after
        && !gAgent.getAutoPilot()
        && gAgent.getTeleportState() == LLAgent::TELEPORT_NONE)
    {
        fps = idle_fps;
    }
    if (fps > 0.f && last_frame_end > 0)
    {
        F64 wait = 1.0 / llmax(fps, 0.1f) - (now - last_frame_end);
        if (wait > 0.001)
        {
            ms_sleep((U32)(wait * 1000.0));
        }
    }
    last_frame_end = LLTimer::getTotalSeconds();
}

bool allowWebPlugin(const std::string& plugin_basename)
{
    // The CEF plugin starts Chromium processes (about half a GB, and a third of the frame rate under
    // software rendering); an agent on a server has little use for the viewer's web pages.
    return plugin_basename != "media_plugin_cef" || !gSavedSettings.getBOOL("IbashoNoWebPlugin");
}

F32 textureBudgetMB()
{
    static LLCachedControl<U32> budget(gSavedSettings, "IbashoTextureBudget", 0);
    return (F32)(U32)budget;
}

}
