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
}

#endif
