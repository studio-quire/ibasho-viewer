/**
 * @file   ibashoagent.cpp
 * @brief  IbashoAgent LEAP API: money, giving, attaching, social offers, groups, parcel, people.
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
#include "llagentui.h"
#include "llappearancemgr.h"
#include "llavatarnamecache.h"
#include "llcallingcard.h"
#include "lleventapi.h"
#include "llgiveinventory.h"
#include "llgroupactions.h"
#include "llinventorymodel.h"
#include "llnotifications.h"
#include "llparcel.h"
#include "llregionflags.h"
#include "llsdutil.h"
#include "llsdutil_math.h"
#include "llstartup.h"
#include "llstatusbar.h"
#include "lltransactiontypes.h"
#include "lluserrelations.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewermessage.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "message.h"

void send_lures(const LLSD& notification, const LLSD& response); // llviewermessage.cpp

namespace
{

LLSD nameSD(const LLUUID& id)
{
    LLAvatarName av;
    if (!LLAvatarNameCache::get(id, &av))
        return LLSD();
    return llsd::map("display_name", av.getDisplayName(), "username", av.getUserName());
}

LLViewerJointAttachment* findAttachPoint(const LLSD& point)
{
    if (!isAgentAvatarValid() || !point.isDefined())
        return nullptr;
    if (point.isInteger())
    {
        auto it = gAgentAvatarp->mAttachmentPoints.find(point.asInteger());
        return it == gAgentAvatarp->mAttachmentPoints.end() ? nullptr : it->second;
    }
    std::string want = point.asString();
    LLStringUtil::toLower(want);
    for (const auto& [id, att] : gAgentAvatarp->mAttachmentPoints)
    {
        if (!att)
            continue;
        std::string name = att->getName();
        LLStringUtil::toLower(name);
        if (name == want)
            return att;
    }
    return nullptr;
}

class IbashoAgentListener : public LLEventAPI
{
public:
    IbashoAgentListener()
        : LLEventAPI("IbashoAgent",
                     "Money, giving, attachments, friendship/teleport offers, groups, parcel and people.\n"
                     "Incoming offers arrive as notifications; answer them with LLNotifications respond.")
    {
        add("balance",
            "Reply {balance, known} (L$); known is false before the first balance update.",
            &IbashoAgentListener::balance,
            llsd::map("reply", LLSD()));
        add("pay",
            "Pay [\"amount\"] L$ to [\"to\"]: an avatar id, or an object in view (which must accept money).\n"
            "Optional [\"desc\"] for avatar payments. Refused above the IbashoMaxPayment setting, or past\n"
            "IbashoMaxSessionSpend (if non-zero) for this session. Replies {ok} or {error}; the result\n"
            "arrives as an IbashoEvents {event:\"money\"} event.",
            &IbashoAgentListener::pay,
            llsd::map("to", LLSD(), "amount", LLSD()));
        add("give",
            "Give inventory [\"item_id\"] or folder [\"folder_id\"] to avatar [\"to\"].",
            &IbashoAgentListener::give,
            llsd::map("to", LLSD()));
        add("attach",
            "Wear inventory object [\"item_id\"] on [\"point\"] (name like \"Right Hand\" or number;\n"
            "default: the object's last point). [\"replace\"] replaces what's there, else adds.",
            &IbashoAgentListener::attach,
            llsd::map("item_id", LLSD()));
        add("detach",
            "Detach the attachment with inventory [\"item_id\"] or in-world [\"object_id\"].",
            &IbashoAgentListener::detach);
        add("attachPoints",
            "Reply [\"points\"]: {id, name, objects} for every attachment point.",
            &IbashoAgentListener::attachPoints,
            llsd::map("reply", LLSD()));
        add("offerFriendship",
            "Offer friendship to avatar [\"to\"] with optional [\"message\"].",
            &IbashoAgentListener::offerFriendship,
            llsd::map("to", LLSD()));
        add("offerTeleport",
            "Offer a teleport to my location to avatars [\"ids\"] (or [\"to\"]) with optional [\"message\"].",
            &IbashoAgentListener::offerTeleport);
        add("requestTeleport",
            "Ask avatar [\"to\"] to teleport me to them, with optional [\"message\"].",
            &IbashoAgentListener::requestTeleport,
            llsd::map("to", LLSD()));
        add("joinGroup",
            "Start joining group [\"group_id\"]; a JoinGroup notification asks to confirm (and shows any fee).",
            &IbashoAgentListener::joinGroup,
            llsd::map("group_id", LLSD()));
        add("leaveGroup",
            "Start leaving group [\"group_id\"]; a notification asks to confirm.",
            &IbashoAgentListener::leaveGroup,
            llsd::map("group_id", LLSD()));
        add("activateGroup",
            "Make [\"group_id\"] the active group (null id: none).",
            &IbashoAgentListener::activateGroup);
        add("setHome",
            "Set home to the current location (the region replies with an alert).",
            &IbashoAgentListener::setHome);
        add("parcel",
            "Reply with the current parcel and region: name, desc, owner, area, flags, music/media, rights.",
            &IbashoAgentListener::parcel,
            llsd::map("reply", LLSD()));
        add("people",
            "Reply [\"people\"]: avatars within [\"dist\"] m (default 64; region-wide coarse positions),\n"
            "nearest first: id, display_name, username, pos (region), dist, friend.",
            &IbashoAgentListener::people,
            llsd::map("reply", LLSD()));
        add("friends",
            "Reply [\"friends\"]: id, display_name, username, online, rights_to/rights_from (bitmasks).",
            &IbashoAgentListener::friends,
            llsd::map("reply", LLSD()));
        add("names",
            "Reply [\"names\"]: {id: {display_name, username}} for avatar [\"ids\"], waiting up to\n"
            "[\"timeout\"] s (default 5) for lookups.",
            &IbashoAgentListener::names,
            llsd::map("ids", LLSD(), "reply", LLSD()));
    }

private:
    void balance(const LLSD& data)
    {
        Response response(LLSD(), data);
        response["known"] = gStatusBar && gStatusBar->getBalance() >= 0 && LLStartUp::getStartupState() >= STATE_STARTED;
        response["balance"] = gStatusBar ? gStatusBar->getBalance() : 0;
    }

    void pay(const LLSD& data)
    {
        Response response(LLSD(), data);
        S32 amount = data["amount"].asInteger();
        LLUUID to = data["to"].asUUID();
        if (amount <= 0)
            return response.error("amount must be positive");
        if (to.isNull() || to == gAgentID)
            return response.error("bad recipient");
        std::string refusal = ibasho::spendRefusal(amount);
        if (!refusal.empty())
            return response.error(refusal);

        LLViewerObject* obj = gObjectList.findObject(to);
        if (obj && !obj->isAvatar())
        {
            if (!obj->flagTakesMoney() && !obj->getRootEdit()->flagTakesMoney())
                return response.error("that object doesn't accept payment");
            if (!obj->flagTakesMoney())
                obj = obj->getRootEdit();
            give_money(obj->getID(), obj->getRegion(), amount, false, TRANS_PAY_OBJECT, data["desc"].asString());
            response["target"] = "object";
        }
        else
        {
            give_money(to, gAgent.getRegion(), amount, false, TRANS_GIFT, data["desc"].asString());
            response["target"] = "avatar";
        }
        ibasho::recordSpend(amount);
        response["ok"] = true;
        response["spent_this_session"] = ibasho::spentThisSession();
    }

    void give(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLUUID to = data["to"].asUUID();
        if (to.isNull() || to == gAgentID)
            return response.error("bad recipient");
        if (data.has("folder_id"))
        {
            LLViewerInventoryCategory* cat = gInventory.getCategory(data["folder_id"].asUUID());
            if (!cat)
                return response.error("no such folder");
            if (!LLGiveInventory::doGiveInventoryCategory(to, cat))
                return response.error("can't give that folder");
            return;
        }
        LLViewerInventoryItem* item = gInventory.getItem(data["item_id"].asUUID());
        if (!item)
            return response.error("no such item");
        if (!LLGiveInventory::isInventoryGiveAcceptable(item))
            return response.error("item can't be given (no transfer, or worn)");
        if (!LLGiveInventory::doGiveInventoryItem(to, item))
            return response.error("give failed");
    }

    void attach(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLViewerInventoryItem* item = gInventory.getItem(data["item_id"].asUUID());
        if (!item)
            return response.error("no such item");
        if (!isAgentAvatarValid())
            return response.error("no avatar");
        S32 point_id = 0;
        if (data.has("point"))
        {
            LLViewerJointAttachment* att = findAttachPoint(data["point"]);
            if (!att)
                return response.error("unknown attachment point " + data["point"].asString());
            for (const auto& [id, a] : gAgentAvatarp->mAttachmentPoints)
                if (a == att)
                    point_id = id;
        }
        if (gAgentAvatarp->isWearingAttachment(item->getLinkedUUID()))
            return response.error("already worn");
        // Same as rez_attachment(), minus the "replace?" confirmation dialog.
        LLSD payload = llsd::map("item_id", item->getLinkedUUID(), "attachment_point", point_id,
                                 "is_add", !data["replace"].asBoolean());
        LLNotifications::instance().forceResponse(LLNotification::Params("ReplaceAttachment").payload(payload), 0);
        response["point"] = point_id;
    }

    void detach(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLUUID item_id = data["item_id"].asUUID();
        LLViewerObject* obj = nullptr;
        if (data.has("object_id"))
        {
            obj = ibasho::findObject(data["object_id"]);
            if (obj)
                item_id = obj->getRootEdit()->getAttachmentItemID();
        }
        else if (isAgentAvatarValid())
        {
            obj = gAgentAvatarp->getWornAttachment(item_id);
        }
        if (item_id.isNull() && !obj)
            return response.error("need the worn item_id or object_id");
        if (item_id.notNull())
            LLAppearanceMgr::instance().removeItemFromAvatar(item_id); // drops the outfit (COF) link
        if (obj && obj->getRootEdit()->isAttachment())
        {
            // Also ask the region directly; the COF route does nothing if the link isn't there yet.
            LLMessageSystem* msg = gMessageSystem;
            msg->newMessageFast(_PREHASH_ObjectDetach);
            msg->nextBlockFast(_PREHASH_AgentData);
            msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
            msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
            msg->nextBlockFast(_PREHASH_ObjectData);
            msg->addU32Fast(_PREHASH_ObjectLocalID, obj->getRootEdit()->getLocalID());
            msg->sendReliable(obj->getRegion()->getHost());
        }
    }

    void attachPoints(const LLSD& data)
    {
        Response response(LLSD(), data);
        response["points"] = LLSD::emptyArray();
        if (!isAgentAvatarValid())
            return;
        for (const auto& [id, att] : gAgentAvatarp->mAttachmentPoints)
        {
            if (!att)
                continue;
            LLSD objects = LLSD::emptyArray();
            for (const auto& o : att->mAttachedObjects)
                if (o)
                    objects.append(llsd::map("object_id", o->getID(), "item_id", o->getAttachmentItemID(),
                                             "name", o->getAttachmentItemName()));
            response["points"].append(llsd::map("id", id, "name", att->getName(), "hud", att->getIsHUDAttachment(),
                                                "objects", objects));
        }
    }

    void offerFriendship(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLUUID to = data["to"].asUUID();
        if (to.isNull() || to == gAgentID)
            return response.error("bad recipient");
        LLSD name = nameSD(to);
        const LLUUID calling_cards = gInventory.findCategoryUUIDForType(LLFolderType::FT_CALLINGCARD);
        send_improved_im(to, name.isDefined() ? name["display_name"].asString() : to.asString(),
                         data["message"].asString(), IM_ONLINE, IM_FRIENDSHIP_OFFERED, calling_cards);
    }

    void offerTeleport(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLSD ids = LLSD::emptyArray();
        for (const LLUUID& id : ibasho::idList(data, "ids", "to"))
            ids.append(id);
        if (!ids.size())
            return response.error("need ids or to");
        LLSD notification = llsd::map("payload", llsd::map("ids", ids));
        send_lures(notification, llsd::map("message", data["message"].asString()));
    }

    void requestTeleport(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLUUID to = data["to"].asUUID();
        if (to.isNull() || to == gAgentID)
            return response.error("bad recipient");
        std::string my_name;
        LLAgentUI::buildFullname(my_name);
        send_improved_im(to, my_name, data["message"].asString(), IM_ONLINE, IM_TELEPORT_REQUEST);
    }

    void joinGroup(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLGroupActions::join(data["group_id"].asUUID());
    }

    void leaveGroup(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLGroupActions::leave(data["group_id"].asUUID());
    }

    void activateGroup(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLGroupActions::activate(data["group_id"].asUUID());
    }

    void setHome(const LLSD& data)
    {
        Response response(LLSD(), data);
        gAgent.setStartPosition(START_LOCATION_ID_HOME);
    }

    void parcel(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLViewerRegion* region = gAgent.getRegion();
        LLViewerParcelMgr* mgr = LLViewerParcelMgr::getInstance();
        LLParcel* p = mgr->getAgentParcel();
        if (region)
        {
            response["region"] = llsd::map("name", region->getName(), "id", region->getRegionID(),
                                           "maturity", region->getSimAccessString(),
                                           "sandbox", region->getRegionFlag(REGION_FLAGS_SANDBOX));
        }
        if (!p)
            return response.error("no parcel data yet");
        LLSD flags = LLSD::emptyArray();
        if (mgr->allowAgentBuild(p))                  flags.append("build");
        if (mgr->allowAgentScripts(region, p))        flags.append("scripts");
        if (mgr->allowAgentFly(region, p))            flags.append("fly");
        if (mgr->allowAgentPush(region, p))           flags.append("push");
        if (mgr->allowAgentDamage(region, p))         flags.append("damage");
        if (mgr->allowAgentVoice(region, p))          flags.append("voice");
        if (p->getSeeAVs())                           flags.append("see_avatars");
        response["parcel"] = llsd::map(
            "name", p->getName(),
            "desc", p->getDesc(),
            "local_id", p->getLocalID(),
            "owner_id", p->getOwnerID(),
            "group_id", p->getGroupID(),
            "group_owned", p->getIsGroupOwned(),
            "area", p->getArea(),
            "category", LLParcel::getCategoryString(p->getCategory()),
            "music_url", p->getMusicURL(),
            "media_url", p->getMediaURL(),
            "for_sale", p->getForSale(),
            "sale_price", p->getSalePrice(),
            "allowed", flags);
    }

    void people(const LLSD& data)
    {
        Response response(LLSD(), data);
        F32 radius = data.has("dist") ? (F32)data["dist"].asReal() : 64.f;
        uuid_vec_t ids;
        std::vector<LLVector3d> positions;
        LLVector3d me = gAgent.getPositionGlobal();
        LLWorld::getInstance()->getAvatars(&ids, &positions, me, radius);
        std::vector<std::pair<F64, LLSD>> found;
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (ids[i] == gAgentID)
                continue;
            F64 d = dist_vec(positions[i], me);
            LLSD entry = llsd::map("id", ids[i], "dist", d,
                                   "pos", ll_sd_from_vector3(gAgent.getPosAgentFromGlobal(positions[i])),
                                   "friend", LLAvatarTracker::instance().isBuddy(ids[i]));
            LLSD name = nameSD(ids[i]);
            if (name.isDefined())
            {
                entry["display_name"] = name["display_name"];
                entry["username"] = name["username"];
            }
            found.emplace_back(d, entry);
        }
        std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        response["people"] = LLSD::emptyArray();
        for (const auto& f : found)
            response["people"].append(f.second);
    }

    void friends(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLAvatarTracker::buddy_map_t buddies;
        LLAvatarTracker::instance().copyBuddyList(buddies);
        response["friends"] = LLSD::emptyArray();
        for (const auto& [id, rel] : buddies)
        {
            LLSD entry = llsd::map("id", id, "online", rel && rel->isOnline(),
                                   "rights_to", rel ? rel->getRightsGrantedTo() : 0,
                                   "rights_from", rel ? rel->getRightsGrantedFrom() : 0);
            LLSD name = nameSD(id);
            if (name.isDefined())
            {
                entry["display_name"] = name["display_name"];
                entry["username"] = name["username"];
            }
            response["friends"].append(entry);
        }
    }

    void names(const LLSD& data)
    {
        std::vector<LLUUID> ids = ibasho::idList(data);
        for (const LLUUID& id : ids)
            LLAvatarNameCache::get(id, [](const LLUUID&, const LLAvatarName&) {}); // starts the lookup
        F64 timeout = data.has("timeout") ? data["timeout"].asReal() : 5.0;
        ibasho::deferReply(data, timeout, [ids](LLSD& reply, bool expired)
        {
            LLSD out = LLSD::emptyMap();
            bool all = true;
            for (const LLUUID& id : ids)
            {
                LLSD n = nameSD(id);
                if (n.isDefined())
                    out[id.asString()] = n;
                else
                    all = false;
            }
            if (!all && !expired)
                return false;
            reply["names"] = out;
            return true;
        });
    }
};

IbashoAgentListener sIbashoAgentListener;
} // namespace
