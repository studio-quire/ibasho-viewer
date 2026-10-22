/**
 * @file   ibasholimits.cpp
 * @brief  Hard limits that hold however the viewer is driven (LEAP APIs, or UI clicks and keys sent
 *         over LEAP): a daily cap on outgoing chat lines and IMs, the spending caps on the UI's own
 *         payment and purchase paths, and no land or L$ purchases unless the person allows them.
 *         Written by Claude Opus 5.5, 2026-10-22.
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

#include "lldir.h"
#include "llnotificationsutil.h"
#include "llsdutil.h"
#include "llstring.h"
#include "lltimer.h"
#include "llviewercontrol.h"

#include <fstream>

namespace
{

// Today's count of outgoing chat lines and IMs, by Second Life time (Pacific) date, kept in a small file
// in the settings folder so a restart (or a crash) doesn't reset it.
struct MessageCount
{
    std::string date;
    S32 count = 0;
    bool loaded = false;
};
MessageCount sMessages;

std::string countFile()
{
    return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "ibasho_messages.txt");
}

std::string sltDate()
{
    struct tm* t = utc_to_pacific_time(time(nullptr), LLStringOps::getPacificDaylightTime());
    return llformat("%04d-%02d-%02d", t->tm_year + 1900, t->tm_mon + 1, t->tm_mday);
}

void rollDate()
{
    if (!sMessages.loaded)
    {
        sMessages.loaded = true;
        std::ifstream in(countFile());
        in >> sMessages.date >> sMessages.count;
    }
    std::string today = sltDate();
    if (today != sMessages.date)
    {
        sMessages.date = today;
        sMessages.count = 0;
    }
}

void saveCount()
{
    std::ofstream out(countFile(), std::ios::trunc);
    out << sMessages.date << ' ' << sMessages.count << '\n';
}

} // namespace

namespace ibasho
{

void refuse(const std::string& what, const std::string& reason)
{
    LL_WARNS("Ibasho") << reason << LL_ENDL;
    LLNotificationsUtil::add("SystemMessageTip", llsd::map("MESSAGE", reason));
    postEvent("refused", llsd::map("what", what, "reason", reason));
}

bool allowOutgoingMessage(const std::string& kind)
{
    rollDate();
    S32 max = gSavedSettings.getS32("IbashoMaxMessagesPerDay");
    if (max > 0 && sMessages.count >= max)
    {
        refuse(kind, llformat("Ibasho: this %s wasn't sent. Today's limit of %d chat lines and IMs "
                              "(IbashoMaxMessagesPerDay, counted by Second Life time) is reached.",
                              kind.c_str(), max));
        return false;
    }
    ++sMessages.count;
    saveCount();
    return true;
}

LLSD messageCount()
{
    rollDate();
    S32 max = gSavedSettings.getS32("IbashoMaxMessagesPerDay");
    return llsd::map("date", sMessages.date, "count", sMessages.count, "max", max,
                     "left", max > 0 ? LLSD(llmax(0, max - sMessages.count)) : LLSD());
}

bool allowSpend(S32 amount, const std::string& what)
{
    std::string refusal = spendRefusal(amount);
    if (!refusal.empty())
    {
        refuse(what, "Ibasho: " + what + " refused: " + refusal + ".");
        return false;
    }
    recordSpend(amount);
    return true;
}

bool allowLandBuy()
{
    if (gSavedSettings.getBOOL("IbashoAllowLandBuy"))
        return true;
    refuse("land", "Ibasho: buying land is off in this viewer (setting IbashoAllowLandBuy).");
    return false;
}

bool allowCurrencyBuy()
{
    if (gSavedSettings.getBOOL("IbashoAllowCurrencyBuy"))
        return true;
    refuse("currency", "Ibasho: buying L$ with real money is off in this viewer (setting IbashoAllowCurrencyBuy).");
    return false;
}

} // namespace ibasho
