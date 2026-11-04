/**
 * @file   ibashohooks.h
 * @brief  Hooks the Ibasho LEAP APIs need from core viewer code. Each one is a single call
 *         inserted where the viewer already handles the event, so the patch stays small.
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

#ifndef LL_IBASHOHOOKS_H
#define LL_IBASHOHOOKS_H

#include "llcoord.h"
#include "llinstantmessage.h"

class LLMessageSystem;
class LLViewerObject;

namespace ibasho
{
    // All Ibasho events go out on this pump as maps with an "event" key.
    LLSD postEvent(const std::string& event, LLSD data);

    // IMs that reach an IM session (P2P, group, conference), after mute filtering.
    void onIncomingIM(const LLUUID& session_id, const LLUUID& from_id, const std::string& from,
                      const std::string& message, bool offline, EInstantMessage dialog, U32 timestamp);
    void onOutgoingIM(const LLUUID& session_id, const LLUUID& to_id, const std::string& message,
                      EInstantMessage dialog);
    // llInstantMessage from an object (the viewer shows these in nearby chat).
    void onObjectIM(const LLUUID& object_id, const LLUUID& owner_id, const std::string& name,
                    const std::string& message, const std::string& slurl);

    // Read the message currently being handled; called from LLSelectMgr's handlers.
    void onObjectProperties(LLMessageSystem* msg, S32 block);
    void onObjectPropertiesFamily(LLMessageSystem* msg);
    // An object we created/rezzed/duplicated arrived (it carried FLAGS_CREATE_SELECTED).
    void onObjectCreated(LLViewerObject* objectp);
    void onMoneyBalance(LLMessageSystem* msg);

    // Mirror the IbashoSpectator setting into the window layer (real mouse/keyboard ignored); from
    // LLViewerWindow's constructor, before the OS window is created.
    void initSpectator();
    // LLWindow mouse events from LEAP; while spectating, the window's cursor position follows them.
    void onInjectedMouse(const LLCoordGL& pos);
    // Mirror the IbashoNoActivate setting into the window layer (the viewer never activates or raises its
    // window); from LLAppViewer::initConfiguration, before the splash screen.
    void initNoActivate();

    // Hard limits (ibasholimits.cpp), checked where the viewer sends, so they hold for UI clicks and keys
    // sent over LEAP too. Each returns false, after a notice and an IbashoEvents "refused" event, to refuse.
    // Outgoing local chat line or IM: counted against IbashoMaxMessagesPerDay (by SLT date, persisted).
    bool allowOutgoingMessage(const std::string& kind);
    // A payment or purchase through the viewer's own UI: the spending caps; counts toward the session.
    bool allowSpend(S32 amount, const std::string& what);
    bool allowLandBuy();      // IbashoAllowLandBuy
    bool allowCurrencyBuy();  // IbashoAllowCurrencyBuy (buying L$ with real money)
    // The built-in browser (web and media floaters, prim and parcel media, parcel music): files, and pages on
    // this machine or its network (loopback, private, link-local and shared addresses, localhost names, any
    // IPv6 literal) are refused, except the grid's own login pages. Checked before a navigation is sent and
    // again when a link or redirect inside a page arrives somewhere (that request has gone out by then).
    bool allowBrowserURL(const std::string& url);

    // IbashoAgent findAvatars and profile (ibashoagent.cpp). An AvatarPickerReply carrying one of our searches'
    // query ids is ours: returns true, and the avatar picker skips it. Groups and picks replies only matter on
    // grids without the AgentProfile capability (LLAvatarPropertiesProcessor itself ignores them).
    bool onAvatarPickerReply(LLMessageSystem* msg, const LLUUID& query_id);
    void onAvatarGroupsReply(LLMessageSystem* msg);
    void onAvatarPicksReply(LLMessageSystem* msg);

    // IbashoAgent findRegion (ibashoagent.cpp): each block of a MapBlockReply, as LLWorldMapMessage reads it.
    // `map_layer`: the reply is to a map-layer request (a name search is one); an empty name ends a search.
    void onMapBlock(bool map_layer, const std::string& name, U32 x_world, U32 y_world, U8 access);
    // IbashoAgent findPlaces (ibashoagent.cpp): a DirPlacesReply carrying one of our searches' query ids is ours:
    // returns true, and the directory's panels skip it.
    bool onDirPlacesReply(LLMessageSystem* msg, const LLUUID& query_id);

    // Running on a server without a GPU (ibashoserver.cpp). At the end of each frame: sleeps as long as
    // IbashoMaxFPS, or IbashoIdleFPS while no LEAP request has come for IbashoIdleAfter seconds, asks.
    void limitFrameRate();
    // false for the CEF (web) plugin when IbashoNoWebPlugin is on; the viewer then preloads no web floaters.
    bool allowWebPlugin(const std::string& plugin_basename);
    // IbashoTextureBudget in MB, or 0 for the viewer's own budget from the reported video memory.
    F32 textureBudgetMB();
    // With IbashoTextureBudget set, the lowest discard level a texture of this boost and full size may have:
    // avatar bakes and my avatar's textures at most 512, the world's at most RenderMaxTextureResolution, even
    // where a loaded callback asks for more. 0 otherwise.
    S32 textureDiscardFloor(S32 boost, S32 full_width, S32 full_height);
    // IbashoAgent textureStats: where texture memory goes.
    LLSD textureStats(const LLSD& data);
}

#endif
