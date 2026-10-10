/**
 * @file   ibashocommon.h
 * @brief  Shared helpers for the Ibasho LEAP APIs (IbashoIM, IbashoObjects, IbashoBuild, IbashoAgent).
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

#ifndef LL_IBASHOCOMMON_H
#define LL_IBASHOCOMMON_H

#include "llsd.h"
#include "lluuid.h"
#include <functional>

class LLViewerObject;

namespace ibasho
{
    extern const char* const EVENT_PUMP; // "IbashoEvents"

    LLViewerObject* findObject(const LLSD& id);

    // "ids" (array) or "id" (single) from a request, as UUIDs.
    std::vector<LLUUID> idList(const LLSD& request, const char* plural = "ids", const char* single = "id");

    // Compact permission string, e.g. "MCT" (modify, copy, transfer) plus "V" for move.
    std::string permString(U32 mask);

    // Reply later. Each idle frame `poll(reply, expired)` runs until it returns true, then the
    // reply goes to the request's ["reply"] pump (if any). `expired` is true once `timeout` seconds
    // have passed, and the reply is sent then whatever poll returns.
    typedef std::function<bool(LLSD& reply, bool expired)> poll_fn_t;
    void deferReply(const LLSD& request, F64 timeout, poll_fn_t poll);

    // Run `fn` every idle frame for as long as the viewer runs.
    void onIdle(std::function<void()> fn);

    // Spending guard for pay and buy: IbashoMaxPayment per payment, IbashoMaxSessionSpend per
    // session (0 = none), and the balance. Returns why `amount` is refused, or "" if allowed.
    std::string spendRefusal(S32 amount);
    void recordSpend(S32 amount);
    S32 spentThisSession();

    // IbashoAgent ops that live in their own files (ibashomove.cpp, ibashomesh.cpp).
    void move(const LLSD& request);
    void stopMove(const LLSD& request);
    void uploadMesh(const LLSD& request);
}

#endif
