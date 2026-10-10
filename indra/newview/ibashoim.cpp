/**
 * @file   ibashoim.cpp
 * @brief  IbashoIM LEAP API: send IMs, list sessions and history; IM and money events.
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

#include "llagent.h"
#include "llavatarnamecache.h"
#include "lleventapi.h"
#include "llgroupactions.h"
#include "llimview.h"
#include "message.h"
#include "llsdutil.h"
#include "llstatusbar.h"

namespace
{
const char* sessionTypeName(const LLIMModel::LLIMSession* session)
{
    if (!session)
        return "unknown";
    switch (session->mSessionType)
    {
    case LLIMModel::LLIMSession::P2P_SESSION:   return "p2p";
    case LLIMModel::LLIMSession::GROUP_SESSION: return "group";
    case LLIMModel::LLIMSession::ADHOC_SESSION: return "conference";
    default:                                    return "other";
    }
}

LLSD sessionInfo(const LLIMModel::LLIMSession* s)
{
    return llsd::map(
        "session_id", s->mSessionID,
        "name", s->mName,
        "type", sessionTypeName(s),
        "other_id", s->mOtherParticipantID,
        "unread", s->mParticipantUnreadMessageCount,
        "offline", s->mHasOfflineMessage,
        "ready", s->isP2PSessionType() || s->mSessionInitialized,
        "messages", (LLSD::Integer)s->mMsgs.size());
}

class IbashoIMListener : public LLEventAPI
{
public:
    IbashoIMListener()
        : LLEventAPI("IbashoIM",
                     "Instant messages (P2P, group, conference). Incoming and outgoing messages are\n"
                     "posted to the \"IbashoEvents\" pump as {event:\"im\", ...}.")
    {
        add("send",
            "Send [\"message\"] to one of: [\"to\"] avatar id (opens a P2P session if needed),\n"
            "[\"group_id\"] (joins the group chat if needed) or an existing [\"session_id\"].\n"
            "Group/conference messages wait (up to 20 s) for the session to start. Replies {session_id} or {error}.",
            &IbashoIMListener::send,
            llsd::map("message", LLSD()));
        add("sessions",
            "Reply [\"sessions\"]: open IM sessions {session_id, name, type, other_id, unread, offline, ready, messages}",
            &IbashoIMListener::sessions,
            llsd::map("reply", LLSD()));
        add("history",
            "Reply [\"messages\"]: the last [\"count\"] (default 20) messages of [\"session_id\"], oldest first,\n"
            "each {from, from_id, message, time, timestamp}. Doesn't mark them read.",
            &IbashoIMListener::history,
            llsd::map("session_id", LLSD(), "reply", LLSD()));
        add("markRead",
            "Clear the unread count of [\"session_id\"]",
            &IbashoIMListener::markRead,
            llsd::map("session_id", LLSD()));
        add("leave",
            "Leave/close IM session [\"session_id\"]",
            &IbashoIMListener::leave,
            llsd::map("session_id", LLSD()));
        add("conference",
            "Start an ad-hoc conference with avatar ids [\"ids\"]; optional first [\"message\"].\n"
            "Replies {session_id} once the server has started it.",
            &IbashoIMListener::conference,
            llsd::map("ids", LLSD()));
    }

private:
    // Send now if the session is ready; group and conference sessions first wait for the
    // server to initialize them (as the IM floater queues messages until then).
    static void sendWhenReady(const LLSD& data, const LLUUID& session_id, const LLUUID& other_id,
                              EInstantMessage dialog, const std::string& text)
    {
        ibasho::deferReply(data, 20.0, [=](LLSD& reply, bool expired)
        {
            LLIMModel::LLIMSession* s = LLIMModel::getInstance()->findIMSession(session_id);
            // P2P text doesn't wait: P2P sessions only "initialize" for voice.
            bool ready = s && (s->isP2PSessionType() || s->mSessionInitialized);
            if (s && !ready && !expired)
                return false;
            if (!s)
            {
                reply["error"] = "session closed";
                return true;
            }
            if (!ready)
            {
                reply["error"] = "session didn't start (not sent)";
                return true;
            }
            LLIMModel::sendMessage(text, session_id, other_id, dialog);
            reply["session_id"] = session_id;
            return true;
        });
    }

    void send(const LLSD& data)
    {
        std::string err;
        std::string text = data["message"].asString();
        LLUUID session_id;
        LLUUID other_id;
        EInstantMessage dialog = IM_NOTHING_SPECIAL;
        if (!gIMMgr)
            err = "IM manager not ready (not logged in?)";
        else if (text.empty())
            err = "empty message";
        else if (data.has("session_id"))
        {
            session_id = data["session_id"].asUUID();
            LLIMModel::LLIMSession* s = LLIMModel::getInstance()->findIMSession(session_id);
            if (s)
            {
                other_id = s->mOtherParticipantID;
                dialog = s->mType;
            }
            else
            {
                err = "no such session";
            }
        }
        else if (data.has("group_id"))
        {
            other_id = data["group_id"].asUUID();
            dialog = IM_SESSION_GROUP_START;
            session_id = LLIMMgr::computeSessionID(dialog, other_id);
            if (!gAgent.isInGroup(other_id))
                err = "not a member of that group";
            else if (!gIMMgr->hasSession(session_id))
                LLGroupActions::startIM(other_id);
        }
        else if (data.has("to"))
        {
            other_id = data["to"].asUUID();
            session_id = LLIMMgr::computeSessionID(dialog, other_id);
            if (other_id.isNull() || other_id == gAgentID)
                err = "bad recipient";
            else if (!gIMMgr->hasSession(session_id))
            {
                LLAvatarName av_name;
                std::string name = LLAvatarNameCache::get(other_id, &av_name) ? av_name.getDisplayName()
                                                                               : other_id.asString();
                session_id = gIMMgr->addSession(name, dialog, other_id);
            }
        }
        else
        {
            err = "need to, group_id or session_id";
        }
        if (err.empty() && session_id.isNull())
            err = "could not open session";
        if (!err.empty())
        {
            Response response(LLSD(), data);
            return response.error(err);
        }
        sendWhenReady(data, session_id, other_id, dialog, text);
    }

    void sessions(const LLSD& data)
    {
        Response response(LLSD(), data);
        response["sessions"] = LLSD::emptyArray();
        for (const auto& [id, s] : LLIMModel::getInstance()->mId2SessionMap)
        {
            if (s)
                response["sessions"].append(sessionInfo(s));
        }
    }

    void history(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLIMModel::LLIMSession* s = LLIMModel::getInstance()->findIMSession(data["session_id"].asUUID());
        if (!s)
            return response.error("no such session");
        response["session"] = sessionInfo(s);
        S32 count = data.has("count") ? data["count"].asInteger() : 20;
        // mMsgs holds the newest first
        std::vector<LLSD> picked;
        for (const LLSD& m : s->mMsgs)
        {
            if ((S32)picked.size() >= count)
                break;
            picked.push_back(llsd::map("from", m["from"], "from_id", m["from_id"], "message", m["message"],
                                       "time", m["time"], "timestamp", m["timestamp"]));
        }
        response["messages"] = LLSD::emptyArray();
        for (auto it = picked.rbegin(); it != picked.rend(); ++it)
            response["messages"].append(*it);
    }

    void markRead(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLIMModel::LLIMSession* s = LLIMModel::getInstance()->findIMSession(data["session_id"].asUUID());
        if (!s)
            return response.error("no such session");
        s->mNumUnread = 0;
        s->mParticipantUnreadMessageCount = 0;
    }

    void leave(const LLSD& data)
    {
        Response response(LLSD(), data);
        if (!gIMMgr || !gIMMgr->leaveSession(data["session_id"].asUUID()))
            response.error("no such session");
    }

    void conference(const LLSD& data)
    {
        uuid_vec_t ids = ibasho::idList(data);
        LLUUID session_id = ids.empty() || !gIMMgr ? LLUUID()
                          : gIMMgr->addSession("Conference", IM_SESSION_CONFERENCE_START, ids[0], ids);
        if (session_id.isNull())
        {
            Response response(LLSD(), data);
            return response.error("could not start conference");
        }
        // The server gives the conference its real id when it starts; find it by its members.
        std::string text = data["message"].asString();
        ibasho::deferReply(data, 20.0, [=](LLSD& reply, bool expired)
        {
            for (const auto& [id, s] : LLIMModel::getInstance()->mId2SessionMap)
            {
                if (s && s->isAdHocSessionType() && s->mSessionInitialized && s->mInitialTargetIDs == ids)
                {
                    if (!text.empty())
                        LLIMModel::sendMessage(text, s->mSessionID, ids[0], IM_SESSION_CONFERENCE_START);
                    reply["session_id"] = s->mSessionID;
                    return true;
                }
            }
            if (expired)
                reply["error"] = "conference didn't start";
            return false;
        });
    }
};

IbashoIMListener sIbashoIMListener;
} // namespace

namespace ibasho
{
void onIncomingIM(const LLUUID& session_id, const LLUUID& from_id, const std::string& from,
                  const std::string& message, bool offline, EInstantMessage dialog, U32 timestamp)
{
    if (from_id == gAgentID)
        return; // server echo of our own group/conference message; onOutgoingIM reported it
    LLIMModel::LLIMSession* s = LLIMModel::getInstance()->findIMSession(session_id);
    postEvent("im", llsd::map(
        "dir", "in",
        "session_id", session_id,
        "session_type", sessionTypeName(s),
        "session_name", s ? s->mName : std::string(),
        "from_id", from_id,
        "from", from,
        "message", message,
        "offline", offline,
        "dialog", (S32)dialog,
        "timestamp", (LLSD::Integer)(timestamp ? timestamp : (U32)time_corrected())));
}

void onOutgoingIM(const LLUUID& session_id, const LLUUID& to_id, const std::string& message, EInstantMessage dialog)
{
    LLIMModel::LLIMSession* s = LLIMModel::getInstance()->findIMSession(session_id);
    postEvent("im", llsd::map(
        "dir", "out",
        "session_id", session_id,
        "session_type", s ? sessionTypeName(s) : (dialog == IM_NOTHING_SPECIAL ? "p2p" : "other"),
        "session_name", s ? s->mName : std::string(),
        "to_id", to_id,
        "message", message,
        "dialog", (S32)dialog,
        "timestamp", (LLSD::Integer)time_corrected()));
}

void onObjectIM(const LLUUID& object_id, const LLUUID& owner_id, const std::string& name,
                const std::string& message, const std::string& slurl)
{
    postEvent("object_im", llsd::map(
        "object_id", object_id,
        "owner_id", owner_id,
        "from", name,
        "message", message,
        "slurl", slurl,
        "timestamp", (LLSD::Integer)time_corrected()));
}

void onMoneyBalance(LLMessageSystem* msg)
{
    S32 balance = 0;
    LLUUID tid;
    std::string desc;
    msg->getUUID("MoneyData", "TransactionID", tid);
    msg->getS32("MoneyData", "MoneyBalance", balance);
    msg->getStringFast(_PREHASH_MoneyData, _PREHASH_Description, desc);
    LLSD ev = llsd::map("balance", balance, "transaction_id", tid, "desc", desc);
    if (msg->has("TransactionInfo"))
    {
        S32 type = 0, amount = 0;
        LLUUID source, dest;
        bool source_group = false, dest_group = false;
        std::string item;
        msg->getS32("TransactionInfo", "TransactionType", type);
        msg->getUUID("TransactionInfo", "SourceID", source);
        msg->getBOOL("TransactionInfo", "IsSourceGroup", source_group);
        msg->getUUID("TransactionInfo", "DestID", dest);
        msg->getBOOL("TransactionInfo", "IsDestGroup", dest_group);
        msg->getS32("TransactionInfo", "Amount", amount);
        msg->getString("TransactionInfo", "ItemDescription", item);
        ev["type"] = type;
        ev["amount"] = amount;
        ev["source_id"] = source;
        ev["dest_id"] = dest;
        ev["source_group"] = source_group;
        ev["dest_group"] = dest_group;
        ev["item"] = item;
        ev["dir"] = dest == gAgentID ? "in" : (source == gAgentID ? "out" : "");
    }
    // Balance replies without a transaction carry no news.
    if ((ev.has("amount") && ev["amount"].asInteger() != 0) || (tid.notNull() && !desc.empty()))
        postEvent("money", ev);
}
} // namespace ibasho
