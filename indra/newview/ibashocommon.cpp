/**
 * @file   ibashocommon.cpp
 * @brief  Shared helpers for the Ibasho LEAP APIs.
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
#include "ibashocommon.h"
#include "ibashohooks.h"

#include "llcallbacklist.h"
#include "llevents.h"
#include "llpermissionsflags.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewermessage.h" // can_afford_transaction
#include "llviewerobject.h"
#include "llviewerobjectlist.h"

namespace ibasho
{
const char* const EVENT_PUMP = "IbashoEvents";

namespace
{
    // Created eagerly so a LEAP client can listen before the first event; obtain() never
    // collides with a later obtain() of the same name.
    struct PumpInit { PumpInit() { LLEventPumps::instance().obtain(EVENT_PUMP); } } sPumpInit;

    struct Deferred
    {
        LLSD request;
        F64 deadline;
        poll_fn_t poll;
    };
    std::list<Deferred> sDeferred;
    std::vector<std::function<void()>> sIdleFns;
    bool sIdleStarted = false;

    void startIdle()
    {
        if (sIdleStarted)
            return;
        sIdleStarted = true;
        doOnIdleRepeating([]()
        {
            for (auto& fn : sIdleFns)
                fn();
            F64 now = (F64)LLTimer::getTotalSeconds();
            for (auto it = sDeferred.begin(); it != sDeferred.end();)
            {
                LLSD reply = LLSD::emptyMap();
                bool expired = now >= it->deadline;
                if (it->poll(reply, expired) || expired)
                {
                    sendReply(reply, it->request);
                    it = sDeferred.erase(it);
                }
                else
                {
                    ++it;
                }
            }
            return false; // keep running
        });
    }
}

LLSD postEvent(const std::string& event, LLSD data)
{
    data["event"] = event;
    LLEventPumps::instance().obtain(EVENT_PUMP).post(data);
    return data;
}

LLViewerObject* findObject(const LLSD& id)
{
    LLUUID uuid(id.asUUID());
    return uuid.isNull() ? nullptr : gObjectList.findObject(uuid);
}

std::vector<LLUUID> idList(const LLSD& request, const char* plural, const char* single)
{
    std::vector<LLUUID> ids;
    if (request.has(plural))
    {
        for (const auto& v : llsd::inArray(request[plural]))
            ids.push_back(v.asUUID());
    }
    if (request.has(single))
        ids.push_back(request[single].asUUID());
    return ids;
}

std::string permString(U32 mask)
{
    std::string s;
    if (mask & PERM_MODIFY)   s += 'M';
    if (mask & PERM_COPY)     s += 'C';
    if (mask & PERM_TRANSFER) s += 'T';
    if (mask & PERM_MOVE)     s += 'V';
    return s.empty() ? "-" : s;
}

void deferReply(const LLSD& request, F64 timeout, poll_fn_t poll)
{
    startIdle();
    sDeferred.push_back({ request, (F64)LLTimer::getTotalSeconds() + timeout, std::move(poll) });
}

void onIdle(std::function<void()> fn)
{
    startIdle();
    sIdleFns.push_back(std::move(fn));
}

namespace { S32 sSpent = 0; }

std::string spendRefusal(S32 amount)
{
    if (amount < 0)
        return "negative amount";
    S32 cap = gSavedSettings.getS32("IbashoMaxPayment");
    if (amount > cap)
        return llformat("L$%d is over the per-payment cap of L$%d (IbashoMaxPayment)", amount, cap);
    S32 session_cap = gSavedSettings.getS32("IbashoMaxSessionSpend");
    if (session_cap > 0 && sSpent + amount > session_cap)
        return llformat("would pass this session's cap of L$%d (spent L$%d)", session_cap, sSpent);
    if (amount > 0 && !can_afford_transaction(amount))
        return "can't afford it";
    return "";
}

void recordSpend(S32 amount)
{
    sSpent += amount;
}

S32 spentThisSession()
{
    return sSpent;
}

} // namespace ibasho
