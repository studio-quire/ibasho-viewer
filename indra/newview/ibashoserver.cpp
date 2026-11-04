/**
 * @file   ibashoserver.cpp
 * @brief  Running on a server without a GPU (Mesa's llvmpipe under a virtual display): a frame rate cap
 *         with a lower one while no LEAP plugin is asking anything, no web browser plugin, a
 *         texture budget that holds (avatar bakes held to 512 within it), and memory given back to the
 *         system now and then. Each is a setting, off by default; Ibasho's tools set them. And where
 *         texture memory goes (IbashoAgent textureStats).
 *         Written by Claude Opus 5.5, 2026-11-02 and 2026-11-03.
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
#include "llface.h"
#include "llimagegl.h"
#include "llleap.h"
#include "llsdutil.h"
#include "llstartup.h"
#include "lltimer.h"
#include "llvertexbuffer.h"
#include "llviewercontrol.h"
#include "llviewertexture.h"
#include "llviewertexturelist.h"

#if LL_LINUX
#include <malloc.h>
#endif

namespace ibasho
{

namespace
{
// glibc keeps memory the viewer has freed (textures, meshes, objects left behind on leaving a busy region) for
// reuse rather than handing it back, so a viewer that once saw a crowd stays that size. Every IbashoTrimMemory
// seconds, malloc_trim gives the free pages back.
void trimMemory()
{
#if LL_LINUX
    static LLCachedControl<F32> every(gSavedSettings, "IbashoTrimMemory", 0.f);
    static F64 last = 0;
    F64 now = LLTimer::getTotalSeconds();
    if (every <= 0.f || now - last < every)
    {
        return;
    }
    last = now;
    malloc_trim(0);
#endif
}
}

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
    trimMemory(); // once a frame is a good moment: nothing is being drawn
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

S32 textureDiscardFloor(S32 boost, S32 full_width, S32 full_height)
{
    // With a budget set (a server), avatar bakes and my own avatar's textures are held to 512 and the world's
    // textures to RenderMaxTextureResolution, whatever else asks for more. On Second Life avatars wear 2048
    // bakes, which the viewer keeps whole: bakes are never scaled down, and the head, upper and lower bakes
    // hold a loaded callback at discard 0 (for the morph masks) that keeps their desired level at 0. In London
    // City, 2026-11-03, 37 such bakes were 790 of the viewer's 1,547 MB of textures, and my own avatar's 187.
    // Sculpt maps, bump maps, selected objects and the UI keep what they need.
    if (textureBudgetMB() <= 0.f || full_width <= 0 || full_height <= 0)
        return 0;
    S32 most;
    switch (boost)
    {
    case LLGLTexture::BOOST_AVATAR:
    case LLGLTexture::BOOST_AVATAR_BAKED:
    case LLGLTexture::BOOST_AVATAR_BAKED_SELF:
    case LLGLTexture::BOOST_AVATAR_SELF:
        most = 512;
        break;
    case LLGLTexture::BOOST_NONE:
    case LLGLTexture::BOOST_TERRAIN:
    {
        static LLCachedControl<U32> max_res(gSavedSettings, "RenderMaxTextureResolution", 2048);
        most = llclamp((S32)(U32)max_res, 512, 2048);
        break;
    }
    default:
        return 0;
    }
    S32 floor = 0;
    for (S32 side = llmax(full_width, full_height); side > most; side >>= 1)
        ++floor;
    return floor;
}

// Where the texture memory goes, for finding why a budget doesn't hold: the viewer's own totals and bias, then
// the fetched textures grouped by boost level (bytes of each one's GL image at its current discard level, its
// raw copies in memory), and the largest few. MB are real ones, not the doubled count the budget is measured in.
LLSD textureStats(const LLSD& data)
{
    constexpr F64 MB = 1024.0 * 1024.0;
    LLSD out;
    out["gl_texture_mb"] = (F64)LLImageGL::getTextureBytesAllocated() / MB;
    out["vertex_mb"] = (F64)LLVertexBuffer::getBytesAllocated() / MB;
    out["budget_mb"] = textureBudgetMB();
    out["free_budget_mb"] = LLViewerTexture::sFreeVRAMMegabytes;
    out["discard_bias"] = LLViewerTexture::sDesiredDiscardBias;
    out["system_memory_low"] = LLViewerTexture::isSystemMemoryLow();

    struct Group { S32 count = 0, faceless = 0, on_screen = 0, raw_count = 0; F64 bytes = 0, raw = 0; LLSD sizes; };
    std::map<S32, Group> groups;
    struct Big { F64 bytes; LLSD info; };
    std::vector<Big> biggest;
    S32 top = data.has("top") ? llclamp(data["top"].asInteger(), 0, 200) : 15;
    F64 listed = 0;
    for (const auto& image : gTextureList)
    {
        LLViewerFetchedTexture* t = image.get();
        if (!t)
            continue;
        LLImageGL* gl = t->getGLTexture();
        F64 bytes = 0;
        S32 side = 0;
        if (gl && gl->getHasGLTexture() && t->getDiscardLevel() >= 0)
        {
            S32 discard = t->getDiscardLevel();
            S32 w = llmax(t->getFullWidth() >> discard, 1);
            S32 h = llmax(t->getFullHeight() >> discard, 1);
            side = llmax(w, h);
            bytes = (F64)w * h * llmax((S32)gl->getComponents(), 1) * (gl->getUseMipMaps() ? 4.0 / 3.0 : 1.0);
        }
        S32 faces = 0;
        bool on_screen = false;
        for (U32 ch = 0; ch < LLRender::NUM_TEXTURE_CHANNELS; ++ch)
        {
            faces += t->getNumFaces(ch);
            const LLViewerTexture::ll_face_list_t* list = t->getFaceList(ch);
            for (S32 i = 0; list && i < t->getNumFaces(ch) && !on_screen; ++i)
                on_screen = (*list)[i] && (*list)[i]->mInFrustum;
        }
        F64 raw = 0;
        if (t->getRawImage())
            raw += t->getRawImage()->getDataSize();
        if (t->hasSavedRawImage() && t->getSavedRawImage())
            raw += t->getSavedRawImage()->getDataSize();
        Group& g = groups[t->getBoostLevel()];
        g.count++;
        g.bytes += bytes;
        g.raw += raw;
        g.raw_count += raw > 0;
        g.faceless += faces == 0 && t->getNumVolumes(LLRender::LIGHT_TEX) == 0;
        g.on_screen += on_screen;
        if (side > 0)
        {
            std::string key = std::to_string(side);
            g.sizes[key] = g.sizes[key].asInteger() + 1;
        }
        listed += bytes;
        if (top > 0 && bytes > 0)
        {
            LLSD info;
            info["id"] = t->getID();
            info["boost"] = t->getBoostLevel();
            info["type"] = t->getType();
            info["full"] = llsd::array(t->getFullWidth(), t->getFullHeight());
            info["side"] = side;
            info["discard"] = t->getDiscardLevel();
            info["desired"] = t->getDesiredDiscardLevel();
            info["faces"] = faces;
            info["on_screen"] = on_screen;
            info["vsize"] = t->getMaxVirtualSize();
            info["mb"] = bytes / MB;
            biggest.push_back({ bytes, info });
        }
    }
    std::sort(biggest.begin(), biggest.end(), [](const Big& a, const Big& b) { return a.bytes > b.bytes; });
    LLSD by_boost = LLSD::emptyMap();
    for (auto& [boost, g] : groups)
    {
        LLSD e;
        e["count"] = g.count;
        e["mb"] = g.bytes / MB;
        e["raw_mb"] = g.raw / MB;
        e["raw_count"] = g.raw_count;
        e["faceless"] = g.faceless;
        e["on_screen"] = g.on_screen;
        e["sizes"] = g.sizes;
        by_boost[std::to_string(boost)] = e;
    }
    out["by_boost"] = by_boost;
    out["fetched_mb"] = listed / MB;
    out["fetched_count"] = (S32)gTextureList.getNumImages();
    LLSD big = LLSD::emptyArray();
    for (size_t i = 0; i < biggest.size() && (S32)i < top; ++i)
        big.append(biggest[i].info);
    out["biggest"] = big;
    return out;
}

}
