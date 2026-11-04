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
#include "ibashohide.h"

#include "llagent.h"
#include "llagentbenefits.h"
#include "llagentui.h"
#include "llagentwearables.h"
#include "llappearancemgr.h"
#include "llassetstorage.h"
#include "llavatarappearancedefines.h"
#include "llavatarnamecache.h"
#include "llavatarpropertiesprocessor.h"
#include "llcachename.h"
#include "llcallingcard.h"
#include "llchatitemscontainerctrl.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldbstrings.h"
#include "lleventapi.h"
#include "llfilesystem.h"
#include "llfloater.h"
#include "llfloaterperms.h"
#include "llfocusmgr.h"
#include "llgiveinventory.h"
#include "llgroupactions.h"
#include "llimagej2c.h"
#include "llinventorymodel.h"
#include "lllocaltextureobject.h"
#include "llmodaldialog.h"
#include "llnotecard.h"
#include "llnotifications.h"
#include "llpermissionsflags.h"
#include "llviewerassetupload.h"
#include "llviewermenufile.h"
#include "llviewertexturelist.h"
#include "llviewerwearable.h"
#include "llviewerwindow.h"
#include "llwindow.h"
#include "llwearabletype.h"
#include "llparcel.h"
#include "llqueryflags.h"
#include "llremoteparcelrequest.h"
#include "llregionflags.h"
#include "llsdutil.h"
#include "llsdutil_math.h"
#include "llstartup.h"
#include "llstatusbar.h"
#include "lltexteditor.h"
#include "lltoast.h"
#include "lltrans.h"
#include "lltransactiontypes.h"
#include "lluri.h"
#include "lluserrelations.h"
#include "llviewercontrol.h"
#include "llviewerdisplayname.h"
#include "llviewergenericmessage.h"
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewermessage.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llvoiceclient.h"
#include "llvoavatarself.h"
#include "llworld.h"
#include "llworldmap.h"
#include "llworldmapmessage.h"
#include "llcharacter.h"
#include "lldrawable.h"
#include "llspatialpartition.h"
#include "llviewerpartsim.h"
#include "llviewerpartsource.h"
#include "llvopartgroup.h"
#include "pipeline.h"
#include "roles_constants.h"
#include "message.h"

#include <atomic>
#include <map>
#include <optional>
#include <set>
#include <unordered_set>

extern bool gSnapshot; // llviewerdisplay.cpp: the current display() is for a snapshot

void send_lures(const LLSD& notification, const LLSD& response); // llviewermessage.cpp
void handle_rebake_textures();                                  // llviewermenu.cpp
extern std::function<bool(LLFloater*, bool)> gIbashoFloaterVisibility; // llfloater.cpp
#if LL_WINDOWS
extern std::atomic<bool> gIbashoSpectator; // llwindowwin32.cpp
extern std::atomic<bool> gIbashoNoActivate; // llwindowwin32.cpp
#endif

using namespace LLAvatarAppearanceDefines;

namespace
{

void replyError(const LLSD& request, const std::string& error)
{
    LLEventAPI::Response response(LLSD(), request);
    response.error(error);
}

std::string inventoryText(const LLSD& value, S32 max_len)
{
    std::string s = value.asString();
    LLStringUtil::replaceNonstandardASCII(s, '?');
    LLStringUtil::replaceChar(s, '|', '?');
    LLStringUtil::stripNonprintable(s);
    LLStringUtil::trim(s);
    return utf8str_truncate(s, max_len);
}

// ---------- uploads ----------

struct UploadState
{
    bool done = false;
    LLUUID item_id;
    LLUUID asset_id;
    S32 cost = 0;
    S32 charged = 0;
    std::string error;
};
typedef std::shared_ptr<UploadState> upload_state_t;

// The viewer's own upload info (Base), reporting back to us, without the modal "Uploading..." box
// or the inventory floater popping up.
template <class Base>
class IbashoUploadInfo : public Base
{
public:
    template <typename... Args>
    IbashoUploadInfo(upload_state_t state, Args&&... args)
        : Base(std::forward<Args>(args)...), mState(std::move(state))
    {
    }

    LLUUID finishUpload(LLSD& result) override
    {
        LLUUID item_id = Base::finishUpload(result);
        mState->item_id = item_id;
        mState->asset_id = result["new_asset"].asUUID();
        mState->charged = result.has("upload_price") ? result["upload_price"].asInteger()
                                                     : (item_id.notNull() ? mState->cost : 0);
        ibasho::recordSpend(mState->charged);
        if (item_id.isNull())
            mState->error = "upload ended without an inventory item";
        mState->done = true;
        return item_id;
    }

    bool failedUpload(LLSD& result, std::string& reason) override
    {
        mState->error = reason.empty() ? std::string("upload failed") : reason;
        mState->done = true;
        return Base::failedUpload(result, reason);
    }

    bool showUploadDialog() const override { return false; }
    bool showInventoryPanel() const override { return false; }

private:
    upload_state_t mState;
};

// Decode an image file and encode it as the viewer does for upload: powers of two within max_size.
LLPointer<LLImageJ2C> encodeTexture(const std::string& path, S32 max_size, bool lossless,
                                    S32& width, S32& height, std::string& err)
{
    std::string ext = gDirUtilp->getExtension(path);
    LLPointer<LLImageFormatted> image = LLImageFormatted::createFromType(LLImageBase::getCodecFromExtension(ext));
    if (image.isNull())
    {
        err = "not an image type the viewer reads: ." + ext;
        return nullptr;
    }
    if (!image->load(path))
    {
        err = "couldn't load the image: " + LLImage::getLastThreadError();
        return nullptr;
    }
    image->setDiscardLevel(0);
    LLPointer<LLImageRaw> raw = new LLImageRaw;
    if (!image->decode(raw, 0.0f))
    {
        err = "couldn't decode the image";
        return nullptr;
    }
    if (raw->getComponents() != 3 && raw->getComponents() != 4)
    {
        err = "image must be RGB or RGBA";
        return nullptr;
    }
    LLPointer<LLImageJ2C> j2c = LLViewerTextureList::convertToUploadFile(raw, max_size, false, lossless);
    if (j2c.isNull())
    {
        err = "JPEG2000 encoding failed";
        return nullptr;
    }
    width = raw->getWidth(); // convertToUploadFile scaled it in place
    height = raw->getHeight();
    return j2c;
}

U32 permMask(const std::string& letters)
{
    U32 mask = PERM_MOVE;
    for (char c : letters)
        mask |= c == 'M' ? PERM_MODIFY : c == 'C' ? PERM_COPY : c == 'T' ? PERM_TRANSFER : 0;
    return mask;
}

// ---------- wearables ----------

// The local texture slots of a wearable type, as (dictionary name, index), e.g. ("upper_tattoo", TEX_UPPER_TATTOO).
std::vector<std::pair<std::string, ETextureIndex>> textureSlots(LLWearableType::EType type)
{
    std::vector<std::pair<std::string, ETextureIndex>> slots;
    for (const auto& [te, entry] : LLAvatarAppearance::getDictionary()->getTextures())
        if (entry && entry->mIsLocalTexture && entry->mWearableType == type)
            slots.emplace_back(entry->mName, te);
    return slots;
}

// A slot by full name ("upper_tattoo"), by the part before the first '_' ("upper"), or by index.
ETextureIndex findTextureSlot(LLWearableType::EType type, const std::string& key)
{
    std::string want = key;
    LLStringUtil::toLower(want);
    auto slots = textureSlots(type);
    for (const auto& [name, te] : slots)
        if (name == want || std::to_string((S32)te) == want)
            return te;
    for (const auto& [name, te] : slots)
        if (name.substr(0, name.find('_')) == want)
            return te;
    return TEX_INVALID;
}

std::string slotNames(LLWearableType::EType type)
{
    std::string names;
    for (const auto& [name, te] : textureSlots(type))
        names += (names.empty() ? "" : ", ") + name;
    return names.empty() ? std::string("none") : names;
}

std::string wearableTypeNames()
{
    std::string names;
    for (S32 i = 0; i < LLWearableType::WT_COUNT; ++i)
        names += (i ? ", " : "") + LLWearableType::getInstance()->getTypeName((LLWearableType::EType)i);
    return names;
}

// The wearable's tint, if its type has one (all its slots share the colour params).
bool colorSlot(LLWearableType::EType type, ETextureIndex& te)
{
    U32 params[3];
    for (const auto& [name, slot] : textureSlots(type))
        if (LLAvatarAppearance::teToColorParams(slot, params))
        {
            te = slot;
            return true;
        }
    return false;
}

LLSD describeWearable(LLViewerWearable* wearable, U32 index)
{
    LLWearableType::EType type = wearable->getType();
    LLViewerInventoryItem* item = gInventory.getItem(wearable->getItemID());
    LLSD out = llsd::map("type", LLWearableType::getInstance()->getTypeName(type), "index", (S32)index,
                         "item_id", wearable->getItemID(), "asset_id", wearable->getAssetID(),
                         "name", item ? item->getName() : wearable->getName(),
                         "modifiable", gAgentWearables.isWearableModifiable(wearable->getItemID()),
                         "unsaved", wearable->isDirty());
    LLSD textures = LLSD::emptyMap();
    for (const auto& [name, te] : textureSlots(type))
    {
        const LLLocalTextureObject* lto = wearable->getLocalTextureObject(te);
        if (lto)
            textures[name] = lto->getID();
    }
    if (textures.size())
        out["textures"] = textures;
    ETextureIndex tint;
    if (colorSlot(type, tint))
    {
        LLColor4 c = wearable->getClothesColor(tint);
        out["color"] = llsd::array(c.mV[0], c.mV[1], c.mV[2]);
    }
    return out;
}

// What the appearance editor's Save does: write the wearable as a new asset, point its item at it,
// and refresh its outfit (COF) link so the bake service rebakes once all of that has gone through.
void saveWornWearable(LLViewerWearable* wearable, U32 index, const std::string& new_name)
{
    const LLUUID item_id = wearable->getItemID();
    LLPointer<LLInventoryCallback> cb = gAgentAvatarp->mEndCustomizeCallback;
    bool lend = cb.isNull(); // null unless the appearance editor is open
    if (lend)
        cb = new LLUpdateAppearanceOnDestroy; // rebakes when the last operation holding it is done
    LLInventoryModel::item_array_t links = LLAppearanceMgr::instance().findCOFItemLinks(item_id);
    if (!links.empty() && links[0]->getIsLinkType())
    {
        LLInventoryObject::const_object_list_t objs;
        objs.push_back(LLConstPointer<LLInventoryObject>(links[0].get()));
        link_inventory_array(LLAppearanceMgr::instance().getCOF(), objs, cb);
        remove_inventory_item(links[0]->getUUID(), cb);
    }
    // saveWearable hands mEndCustomizeCallback to its item update, so the rebake also waits for that.
    if (lend)
        gAgentAvatarp->mEndCustomizeCallback = cb;
    gAgentWearables.saveWearable(wearable->getType(), index, new_name);
    if (lend)
        gAgentAvatarp->mEndCustomizeCallback = nullptr;
}

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

// ---- streamUI (2026-10-10, Claude Opus 5.5) ----
// While on, the UI draws as usual except for anything that puts other people's words on screen: chat and IM
// floaters, nearby chat and IM toasts, and notifications from or about people (offers, friendship, group
// notices...). The gate is LLFloater::setVisible (toasts are floaters too), plus a per-frame sweep for
// anything that got visible some other way. Held-back floaters are re-shown when it goes off.
namespace streamui
{
using LLNotificationsUI::LLToast;

bool sOn = false;
bool sOwnChat = true; // my own nearby chat lines may show
std::optional<bool> sPrevBubbles;
std::set<std::string> sExtraHide, sExtraShow, sExtraFloaters;
std::map<LLFloater*, LLHandle<LLFloater>> sHeld;

const std::set<std::string> FLOATERS = {
    "im_container", "impanel", "nearby_chat", "conversation", "preview_conversation", "im_well_window",
    "notification_well_window", "inspect_toast", "incoming_call", "notifications_console"};
// Offers, group notices and the IM toast are excluded by type; anything that logs to an IM session by
// canLogToIM(). These name or quote people otherwise.
const std::set<std::string> NOTIFICATIONS = {
    "FriendOnlineOffline", "PaymentReceived", "PaymentSent", "ObjectMessage", "ServerObjectMessage",
    "OfferCallingCard", "CallingCardAccepted", "CallingCardDeclined", "OfferFriendshipNoMessage",
    "VoiceInviteP2P", "VoiceInviteGroup", "VoiceInviteAdHoc", "InviteAdHoc", "P2PCallDeclined", "P2PCallNoAnswer",
    "AutoUnmuteByIM", "AutoUnmuteByMoney", "AutoUnmuteByInventory", "EjectAvatarFromGroup",
    "AvatarFrozen", "AvatarFrozenDuration", "AvatarHasUnFrozenYou", "GroupNotice", "JoinGroup"};
const std::set<std::string> TYPES = {"offer", "groupnotify", "notifytoast"};

bool notificationHides(const LLNotificationPtr& n)
{
    const std::string& name = n->getName();
    if (sExtraShow.count(name))
        return false;
    if (sExtraHide.count(name) || NOTIFICATIONS.count(name) || TYPES.count(n->getType()) || n->canLogToIM())
        return true;
    // "<name> gave you <item>." after accepting an offer
    static const std::string gave = LLTrans::getString("InvOfferGaveYou");
    return name == "SystemMessageTip" && n->getMessage().find(gave) != std::string::npos;
}

std::string describe(LLFloater* f)
{
    if (auto* toast = dynamic_cast<LLToast*>(f))
    {
        if (dynamic_cast<LLFloaterIMNearbyChatToastPanel*>(toast->getPanel()))
            return "toast:nearby chat";
        LLNotificationPtr n = LLNotifications::instance().find(toast->getNotificationID());
        return "toast:" + (n ? n->getName() : std::string("?"));
    }
    return f->getInstanceName();
}

bool hides(LLFloater* f)
{
    if (auto* modal = dynamic_cast<LLModalDialog*>(f); modal && modal->isModal())
        return false; // my own confirmations; an invisible modal would also swallow all input
    if (auto* toast = dynamic_cast<LLToast*>(f))
    {
        if (auto* chat = dynamic_cast<LLFloaterIMNearbyChatToastPanel*>(toast->getPanel()))
            return !(sOwnChat && chat->getFromID() == gAgentID);
        LLNotificationPtr n = LLNotifications::instance().find(toast->getNotificationID());
        return !n || notificationHides(n);
    }
    if (f->getHost())
        return false; // hosted floaters go with their host (im_container)
    const std::string& name = f->getInstanceName();
    return FLOATERS.count(name) || sExtraFloaters.count(name);
}

void hold(LLFloater* f)
{
    sHeld[f] = f->getHandle();
    gFocusMgr.releaseFocusIfNeeded(f);
}

bool filter(LLFloater* f, bool visible)
{
    if (!visible)
    {
        sHeld.erase(f); // the viewer itself hides it: nothing to restore
        return false;
    }
    if (!sOn || !hides(f))
        return true;
    hold(f);
    return false;
}

void sweep()
{
    std::vector<LLFloater*> found;
    if (gFloaterView)
        for (LLView* v : *gFloaterView->getChildList())
            if (auto* f = dynamic_cast<LLFloater*>(v); f && f->getVisible() && hides(f))
                found.push_back(f);
    for (LLFloater* f : found)
    {
        f->setVisible(false);
        hold(f);
    }
    for (auto it = sHeld.begin(); it != sHeld.end();)
    {
        LLFloater* f = it->second.get();
        if (!f)
        {
            it = sHeld.erase(it);
            continue;
        }
        gFocusMgr.releaseFocusIfNeeded(f); // openFloater focuses after setVisible
        ++it;
    }
    if (gSavedSettings.getBOOL("UseChatBubbles"))
        gSavedSettings.setBOOL("UseChatBubbles", false);
}

void setOn(bool on)
{
    static bool hooked = false;
    if (!hooked)
    {
        hooked = true;
        gIbashoFloaterVisibility = filter;
        ibasho::onIdle([]() { if (sOn) sweep(); });
    }
    if (on == sOn)
        return;
    sOn = on;
    if (on)
    {
        sPrevBubbles = gSavedSettings.getBOOL("UseChatBubbles");
        sweep();
        return;
    }
    auto held = std::move(sHeld);
    sHeld.clear();
    for (auto& [ptr, handle] : held)
        if (LLFloater* f = handle.get(); f && !f->isDead())
            f->setVisible(true);
    if (sPrevBubbles)
        gSavedSettings.setBOOL("UseChatBubbles", *sPrevBubbles);
    sPrevBubbles.reset();
}

std::set<std::string> nameSet(const LLSD& list)
{
    std::set<std::string> out;
    for (const auto& v : llsd::inArray(list))
        out.insert(v.asString());
    return out;
}
} // namespace streamui

// ---- findAvatars and profile (2026-10-23, Claude Opus 5.5) ----
// A name search as the avatar picker makes it (the region's AvatarPickerSearch capability, else the
// AvatarPickerRequest message), and a profile as the profile window gets it (the AgentProfile capability, else
// the older AvatarPropertiesRequest message, with groups and picks in replies of their own). Each request's
// state is shared by its deferred reply and whatever fills it in; anything arriving after the reply has gone
// finds nobody waiting, and the message routes stop listening once their request has replied.
namespace lookup
{
constexpr F64 SETTLE = 0.5;     // replies by message can come in several parts: wait this long after the last
constexpr F64 EXTRA_WAIT = 3.0; // and at most this long after the properties for groups and picks

F64 now()
{
    return (F64)LLTimer::getTotalSeconds();
}

// A GET as LLFloaterAvatarPicker and LLAvatarPropertiesProcessor make it, from inside a coroutine. Failures
// leave a short reason in `error`; the body, if any, is still returned.
LLSD httpGet(const std::string& name, const std::string& url, F64 timeout, std::string& error)
{
    LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>(name, policy);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
    options->setTimeout((unsigned int)llmax(1, llceil((F32)timeout)));
    options->setFollowRedirects(true);
    LLSD result = adapter->getAndSuspend(request, url, options);
    LLCore::HttpStatus status =
        LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    if (!status)
        error = status.isHttpStatus() ? llformat("HTTP %d", (S32)status.getType()) : status.toString();
    result.erase(LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS);
    return result;
}

// A PUT as the profile window makes it (put_avatar_properties_coro), from inside a coroutine.
bool httpPut(const std::string& name, const std::string& url, const LLSD& body, F64 timeout, std::string& error)
{
    LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter =
        std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>(name, policy);
    LLCore::HttpRequest::ptr_t request = std::make_shared<LLCore::HttpRequest>();
    LLCore::HttpOptions::ptr_t options = std::make_shared<LLCore::HttpOptions>();
    options->setTimeout((unsigned int)llmax(1, llceil((F32)timeout)));
    options->setFollowRedirects(true);
    LLSD result = adapter->putAndSuspend(request, url, body, options);
    LLCore::HttpStatus status =
        LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS]);
    if (!status)
        error = status.isHttpStatus() ? llformat("HTTP %d", (S32)status.getType()) : status.toString();
    return (bool)status;
}

struct Search
{
    LLUUID query_id; // set when searching by message
    bool done = false;
    F64 last_at = 0.0;
    LLSD avatars = LLSD::emptyArray();
    std::string error;
};
std::map<LLUUID, std::shared_ptr<Search>> sSearches; // searches by message, by query id

void searchCoro(std::string url, F64 timeout, std::shared_ptr<Search> search)
{
    std::string error;
    LLSD result = httpGet("IbashoAgent::findAvatars", url, timeout, error);
    if (!error.empty() && !result["agents"].isArray()) // the picker reads a 400's body as results too
        search->error = "the grid's avatar search failed (" + error + ")";
    for (const auto& row : llsd::inArray(result["agents"]))
        if (row["id"].asUUID().notNull())
            search->avatars.append(llsd::map("id", row["id"].asUUID(), "username", row["username"].asString(),
                                             "display_name", row["display_name"].asString()));
    search->done = true;
}

struct Profile
{
    LLUUID id;
    bool done = false;                                 // capability: the answer (or failure) is in
    bool props = false, groups = false, picks = false; // by message: what has arrived
    bool mature_publish = false;                       // by message: kept for an update, not replied
    F64 props_at = 0.0, last_at = 0.0;
    std::string error;
    LLSD out = llsd::map("groups", LLSD::emptyArray(), "picks", LLSD::emptyArray()); // the reply's fields
};
std::multimap<LLUUID, std::shared_ptr<Profile>> sProfiles; // profiles by message, by avatar id

// "YYYY-MM-DD"; just "MM-DD" when the avatar hides their age (as the profile window shows it).
void setBorn(LLSD& out, const LLDate& born, bool hide_year)
{
    S32 year = 0, month = 0, day = 0;
    if (born.secondsSinceEpoch() <= 0.0 || !born.split(&year, &month, &day))
        return;
    out["born"] = hide_year ? llformat("%02d-%02d", month, day) : llformat("%04d-%02d-%02d", year, month, day);
    if (hide_year)
        out["age_hidden"] = true;
}

// As LLAvatarPropertiesProcessor::accountType, untranslated.
std::string accountType(S32 index, const std::string& caption)
{
    if (!caption.empty())
        return caption; // special accounts
    static const char* const TYPES[] = {"Resident", "Trial", "Charter Member", "Linden Lab Employee"};
    return TYPES[llclamp(index, 0, 3)];
}

void addEntry(LLSD& list, const LLUUID& id, const std::string& name)
{
    if (id.isNull())
        return;
    for (const auto& entry : llsd::inArray(list))
        if (entry["id"].asUUID() == id)
            return;
    list.append(llsd::map("id", id, "name", name));
}

// The AgentProfile capability's answer, read as LLAvatarPropertiesProcessor::requestAvatarPropertiesCoro does.
void fillFromCapability(Profile& p, const LLSD& r)
{
    LLSD& out = p.out;
    out["about"] = r["sl_about_text"].asString();
    out["image_id"] = r["sl_image_id"].asUUID();
    out["partner_id"] = r["partner_id"].asUUID();
    out["first_life"] = r["fl_about_text"].asString();
    out["first_life_image_id"] = r["fl_image_id"].asUUID();
    setBorn(out, r["member_since"].asDate(), r["hide_age"].asBoolean() && p.id != gAgentID);
    out["account"] = r.has("charter_member") ? accountType(r["charter_member"].asInteger(), "")
                                             : accountType(0, r["caption"].asString());
    out["online"] = r["online"].asBoolean();
    out["allow_publish"] = r["allow_publish"].asBoolean();
    out["identified"] = r["identified"].asBoolean();
    out["transacted"] = r["transacted"].asBoolean();
    if (!r["customer_type"].asString().empty())
        out["customer_type"] = r["customer_type"].asString();
    if (!r["notes"].asString().empty())
        out["notes"] = r["notes"].asString(); // my own private notes about them
    for (const auto& group : llsd::inArray(r["groups"]))
        addEntry(out["groups"], group["id"].asUUID(), group["name"].asString());
    for (const auto& pick : llsd::inArray(r["picks"]))
        addEntry(out["picks"], pick["id"].asUUID(), pick["name"].asString());
}

void profileCoro(std::string url, F64 timeout, std::shared_ptr<Profile> p)
{
    std::string error;
    LLSD result = httpGet("IbashoAgent::profile", url, timeout, error);
    if (!error.empty())
        p->error = "the grid gave no profile for that id (" + error + ")";
    else if (result["id"].asUUID() != p->id)
        p->error = "the grid gave no profile for that id";
    else
        fillFromCapability(*p, result);
    p->done = true;
}

// The older properties reply, for every profile waiting on that avatar.
class PropertiesObserver : public LLAvatarPropertiesObserver
{
public:
    void processProperties(void* data, EAvatarProcessorType type) override
    {
        if (type != APT_PROPERTIES_LEGACY || !data)
            return;
        const LLAvatarLegacyData& d = *static_cast<const LLAvatarLegacyData*>(data);
        auto range = sProfiles.equal_range(d.avatar_id);
        for (auto it = range.first; it != range.second; ++it)
        {
            Profile& p = *it->second;
            LLSD& out = p.out;
            out["about"] = d.about_text;
            out["image_id"] = d.image_id;
            out["partner_id"] = d.partner_id;
            out["first_life"] = d.fl_about_text;
            out["first_life_image_id"] = d.fl_image_id;
            setBorn(out, d.born_on, false);
            out["account"] = accountType(d.caption_index, d.caption_text);
            out["online"] = (d.flags & AVATAR_ONLINE) != 0;
            out["allow_publish"] = (d.flags & AVATAR_ALLOW_PUBLISH) != 0;
            out["identified"] = (d.flags & AVATAR_IDENTIFIED) != 0;
            out["transacted"] = (d.flags & AVATAR_TRANSACTED) != 0;
            if (!d.profile_url.empty())
                out["web_url"] = d.profile_url;
            p.mature_publish = (d.flags & AVATAR_MATURE_PUBLISH) != 0;
            p.props = true;
            p.props_at = p.last_at = now();
        }
    }
};

// Never destroyed: the processor keeps a plain pointer to it while any profile waits, so it must outlive them all.
PropertiesObserver* observer()
{
    static PropertiesObserver* sObserver = new PropertiesObserver;
    return sObserver;
}

void watch(const std::shared_ptr<Profile>& p)
{
    if (!sProfiles.count(p->id))
        LLAvatarPropertiesProcessor::getInstance()->addObserver(p->id, observer());
    sProfiles.emplace(p->id, p);
}

void unwatch(const std::shared_ptr<Profile>& p)
{
    auto range = sProfiles.equal_range(p->id);
    for (auto it = range.first; it != range.second; ++it)
    {
        if (it->second == p)
        {
            sProfiles.erase(it);
            break;
        }
    }
    if (!sProfiles.count(p->id))
        LLAvatarPropertiesProcessor::getInstance()->removeObserver(p->id, observer());
}

// ---- setProfile and setDisplayName (2026-11-03, Claude Opus 5.5) ----
// My own profile as the profile window saves it: each field a PUT to the AgentProfile capability. Grids without
// it (OpenSim) take the older AvatarPropertiesUpdate message, which carries every field, so my current profile is
// read first and only the fields asked for change. The display name as the display name window sets it
// (LLViewerDisplayName::set); the region answers later, by event.
struct ProfileSave
{
    bool done = false;
    std::string error;
};

void saveProfileCoro(std::string url, F64 timeout, LLSD fields, std::shared_ptr<ProfileSave> save)
{
    for (const auto& [key, value] : llsd::inMap(fields))
    {
        std::string error;
        if (!httpPut("IbashoAgent::setProfile", url, llsd::map(key, value), timeout, error))
        {
            save->error = "the grid didn't take " + key + " (" + error + ")";
            break;
        }
    }
    save->done = true;
}

void sendLegacyProfile(const Profile& p, const LLSD& fields)
{
    const LLSD& out = p.out;
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_AvatarPropertiesUpdate);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgentID);
    msg->addUUIDFast(_PREHASH_SessionID, gAgentSessionID);
    msg->nextBlockFast(_PREHASH_PropertiesData);
    msg->addUUIDFast(_PREHASH_ImageID,
                     fields.has("sl_image_id") ? fields["sl_image_id"].asUUID() : out["image_id"].asUUID());
    msg->addUUIDFast(_PREHASH_FLImageID, out["first_life_image_id"].asUUID());
    msg->addStringFast(_PREHASH_AboutText,
                       fields.has("sl_about_text") ? fields["sl_about_text"].asString() : out["about"].asString());
    msg->addStringFast(_PREHASH_FLAboutText, out["first_life"].asString());
    msg->addBOOLFast(_PREHASH_AllowPublish, out["allow_publish"].asBoolean());
    msg->addBOOLFast(_PREHASH_MaturePublish, p.mature_publish);
    msg->addStringFast(_PREHASH_ProfileURL, out["web_url"].asString());
    gAgent.sendReliableMessage();
}

struct NameSet
{
    bool done = false;
    std::string error;
    LLSD content;
};

// When my display name may change next (the grid allows about one change a week), as an ISO date; "" if now.
std::string nextNameChange()
{
    LLAvatarName av;
    if (!LLAvatarNameCache::get(gAgentID, &av) || av.mNextUpdate <= LLDate::now().secondsSinceEpoch())
        return "";
    return LLDate(av.mNextUpdate).asString();
}
} // namespace lookup

// ---- readNotecard (2026-10-23, Claude Opus 5.5) ----
// A notecard in my inventory, read as the notecard preview reads it (LLPreviewNotecard::loadAsset and
// onLoadComplete): only with copy permission, the asset through the asset storage (the cache, else the grid), then
// the Linden text parsed as LLViewerTextEditor::importBuffer does. Items embedded in the text become [type: name].
namespace notecards
{
constexpr S32 MAX_FILE = 1024 * 1024; // a notecard's asset: its text (64 KB at most) and its embedded items
constexpr S32 MAX_TEXT = LLNotecard::MAX_SIZE;

struct Read
{
    bool done = false;
    std::string error;
    LLSD out;
};

std::string itemLabel(const std::vector<LLPointer<LLInventoryItem>>& items, size_t index)
{
    if (index >= items.size() || items[index].isNull())
        return "[an item]";
    const char* type = LLAssetType::lookup(items[index]->getType());
    return llformat("[%s: %s]", type ? type : "item", items[index]->getName().c_str());
}

// The text an asset holds: Linden text (version 2: UTF-8 with embedded items as FIRST_EMBEDDED_CHAR + index;
// version 1: bytes 0x80 + index), or plain text (version 0).
void parse(const std::vector<char>& buf, LLSD& out, std::string& error)
{
    if (buf.size() > 19 && !strncmp(buf.data(), "Linden text version", 19))
    {
        LLNotecard nc(MAX_TEXT);
        std::istringstream str(std::string(buf.data(), buf.size()));
        if (!nc.importStream(str))
        {
            error = "its contents couldn't be read as a notecard";
            return;
        }
        const auto& items = nc.getItems();
        const std::string& raw = nc.getText();
        std::string text;
        if (nc.getVersion() == 1)
        {
            for (char c : raw)
                text += (U8)c >= 0x80 ? itemLabel(items, (U8)c - 0x80) : std::string(1, c);
        }
        else
        {
            LLWString shown;
            for (llwchar c : utf8str_to_wstring(raw))
            {
                if (c >= LLTextEditor::FIRST_EMBEDDED_CHAR && c <= LLTextEditor::LAST_EMBEDDED_CHAR)
                    shown += utf8str_to_wstring(itemLabel(items, c - LLTextEditor::FIRST_EMBEDDED_CHAR));
                else
                    shown += c;
            }
            text = wstring_to_utf8str(shown);
        }
        LLSD listed = LLSD::emptyArray();
        for (const auto& item : items)
        {
            if (item.notNull())
            {
                const char* type = LLAssetType::lookup(item->getType());
                listed.append(llsd::map("name", item->getName(), "type", type ? type : "item"));
            }
        }
        out["text"] = text;
        out["items"] = listed;
        return;
    }
    std::string text(buf.data(), strnlen(buf.data(), buf.size()));
    if (text.size() > (size_t)MAX_TEXT)
    {
        text = utf8str_truncate(text, MAX_TEXT); // never half a character
        out["truncated"] = true;
    }
    out["text"] = text;
    out["items"] = LLSD::emptyArray();
}

void loaded(Read& r, const LLUUID& asset_id, LLAssetType::EType type, S32 status)
{
    r.done = true;
    if (status != 0)
    {
        if (status == LL_ERR_INSUFFICIENT_PERMISSIONS)
            r.error = "the grid refused to send it (no permission)";
        else if (status == LL_ERR_ASSET_REQUEST_NOT_IN_DATABASE || status == LL_ERR_ASSET_REQUEST_NONEXISTENT_FILE)
            r.error = "the grid has no contents for this notecard (its asset is missing)";
        else
            r.error = llformat("couldn't load it (asset status %d)", status);
        return;
    }
    LLFileSystem file(asset_id, type, LLFileSystem::READ);
    S32 len = file.getSize();
    if (len > MAX_FILE)
    {
        r.error = llformat("it's too big to be a notecard (%d bytes)", len);
        return;
    }
    std::vector<char> buf(llmax(len, 0));
    if (len > 0 && !file.read((U8*)buf.data(), len))
    {
        r.error = "couldn't read it from the cache";
        return;
    }
    parse(buf, r.out, r.error);
}
} // namespace notecards

// ---- findRegion (2026-10-23, Claude Opus 5.5) ----
// A region by name, as the world map's search finds it (MapNameRequest). The answer comes as map blocks: the
// regions whose names match, then an empty block that ends the search (which the map's search list waits for too).
// A hook in LLWorldMapMessage::processMapBlockReply hands each block over.
namespace regions
{
struct Search
{
    std::string want;
    bool ended = false;
    LLSD found;
    LLSD similar = LLSD::emptyArray();
};
std::vector<std::shared_ptr<Search>> sSearches;

void forget(const std::shared_ptr<Search>& s)
{
    sSearches.erase(std::remove(sSearches.begin(), sSearches.end(), s), sSearches.end());
}
} // namespace regions

// ---- findPlaces (2026-11-04, Claude Opus 5.5) ----
// Places by words and category, as the directory's Places tab finds them (LLPanelDirPlaces::queryCore: a
// DirPlacesQuery, which searches with my account's own maturity access; Second Life's web search gives a visitor
// General places only). The grid answers in several DirPlacesReply messages, at most 101 results from the query's
// start, the 101st saying there are more; a hook in LLPanelDirBrowser::processDirPlacesReply hands ours over. Then
// each place on the page is asked for as its place profile asks (ParcelInfoRequest, through
// LLRemoteParcelInfoProcessor): its region, position, rating and description.
namespace places
{
constexpr S32 BATCH = 100;
constexpr F64 SETTLE = 1.0;        // after the last reply, wait this long for more
constexpr F64 DETAILS_WAIT = 6.0;  // then at most this long for the places' details
const char* const LEVELS[] = { "general", "moderate", "adult" };

struct Hit
{
    LLUUID id;
    std::string name;
    bool for_sale = false;
    F32 dwell = 0.f;
};

struct Search : public LLRemoteParcelInfoObserver
{
    LLUUID query_id;
    bool answered = false;
    U32 status = 0;
    F64 last_at = 0.0;
    std::vector<Hit> hits;
    // Once the search has settled: the places on the page, and their details as they come.
    bool paged = false;
    bool more = false;
    F64 paged_at = 0.0;
    std::vector<LLUUID> page;
    std::map<LLUUID, LLParcelData> details;

    void processParcelInfo(const LLParcelData& data) override
    {
        if (std::find(page.begin(), page.end(), data.parcel_id) != page.end())
            details[data.parcel_id] = data;
    }
    void setParcelID(const LLUUID&) override {}
    void setErrorStatus(S32, const std::string&) override {}

    void forget()
    {
        for (const LLUUID& id : page)
            LLRemoteParcelInfoProcessor::getInstance()->removeObserver(id, this);
    }
};
std::map<LLUUID, std::shared_ptr<Search>> sSearches;

// 0, 1, 2; -1 for anything else
S32 levelOf(const std::string& maturity)
{
    for (S32 i = 0; i < 3; ++i)
        if (maturity == LEVELS[i])
            return i;
    return -1;
}

// As LLPanelPlaceProfile::processParcelInfo reads ParcelInfoReply's flags.
S32 levelOfFlags(U8 flags)
{
    return (flags & 0x2) ? 2 : (flags & 0x1) ? 1 : 0;
}

LLSD describe(const Hit& hit, const LLParcelData* d)
{
    LLSD p = llsd::map("id", hit.id, "name", hit.name, "traffic", (S32)ll_round(hit.dwell), "for_sale", hit.for_sale);
    if (!d)
        return p;
    if (hit.dwell <= 0.f)
        p["traffic"] = (S32)ll_round(d->dwell); // sorted by name, the directory sends none
    LLVector3d global(d->global_x, d->global_y, d->global_z);
    LLVector3d origin(floor(d->global_x / REGION_WIDTH_METERS) * REGION_WIDTH_METERS,
                      floor(d->global_y / REGION_WIDTH_METERS) * REGION_WIDTH_METERS, 0.0);
    if (LLSimInfo* sim = LLWorldMap::getInstance()->simInfoFromName(d->sim_name))
        origin = sim->getGlobalOrigin(); // a larger region (OpenSim's var regions), if the map has it
    static const char* const RATINGS[] = { "General", "Moderate", "Adult" };
    p["region"] = d->sim_name;
    p["pos"] = llsd::array(global.mdV[VX] - origin.mdV[VX], global.mdV[VY] - origin.mdV[VY], global.mdV[VZ]);
    p["global"] = ll_sd_from_vector3d(global);
    p["rating"] = RATINGS[levelOfFlags(d->flags)];
    p["description"] = d->desc;
    p["area"] = d->actual_area;
    return p;
}
} // namespace places

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
            "Reply with the current parcel and region: name, desc, owner, area, flags, music/media, rights;\n"
            "my_voice: whether my own voice chat is on, not that it has connected here (the parcel's \"voice\" right says if it's allowed).",
            &IbashoAgentListener::parcel,
            llsd::map("reply", LLSD()));
        add("people",
            "Reply [\"people\"]: avatars within [\"dist\"] m (default 64; region-wide coarse positions),\n"
            "nearest first: id, display_name, username, pos (region), dist, friend; in_voice and speaking\n"
            "(true only) while my own voice chat is connected.",
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
        add("findAvatars",
            "Search avatars by [\"name\"] as the avatar picker does: \"first.last\", \"first last\", \"first\" (a\n"
            "Resident), or part of a display name; the grid's search decides what matches. Replies\n"
            "{avatars: [{id, username, display_name}]} (at most [\"limit\"], default 20, max 100; empty if nobody\n"
            "matched) once the grid answers, or {error} if it hasn't within [\"timeout\"] s (default 10).",
            &IbashoAgentListener::findAvatars,
            llsd::map("name", LLSD(), "reply", LLSD()));
        add("profile",
            "Avatar [\"id\"]'s profile, as the profile window gets it: replies {id, username?, display_name?, about,\n"
            "born? (YYYY-MM-DD; MM-DD with age_hidden: true), image_id, partner_id, first_life, first_life_image_id,\n"
            "account, online, allow_publish, identified, transacted, groups: [{id, name}], picks: [{id, name}],\n"
            "customer_type?, notes? (my own notes on them), web_url?}. Grids without the AgentProfile capability\n"
            "(OpenSim) answer with older messages, where groups or picks may not come: then partial: true.\n"
            "{error} if no profile comes within [\"timeout\"] s (default 10).",
            &IbashoAgentListener::profile,
            llsd::map("id", LLSD(), "reply", LLSD()));
        add("setProfile",
            "Set my own profile as the profile window saves it: [\"about\"] (the about text) and/or\n"
            "[\"image_id\"] (the picture, a texture's asset id). Replies {} once the grid has taken them, or\n"
            "{error}. Grids without the AgentProfile capability (OpenSim) get the older update message, which has\n"
            "no answer: there the reply comes once it's sent ({by_message: true}), and only the fields given change.",
            &IbashoAgentListener::setProfile,
            llsd::map("reply", LLSD()));
        add("setDisplayName",
            "Set my display name to [\"name\"] (at most 31 characters; \"\" or my username goes back to the\n"
            "username), as the display name window does. Second Life allows a change about once a week: replies\n"
            "{display_name, next_change?} once the grid has answered, or {error} (with next_change when it's too\n"
            "soon). Others see the new name within a few minutes; grids without display names say so.",
            &IbashoAgentListener::setDisplayName,
            llsd::map("name", LLSD(), "reply", LLSD()));
        add("readNotecard",
            "Read notecard [\"item_id\"] in my inventory as the notecard preview does (only with copy permission):\n"
            "replies {item_id, name, text, items: [{name, type}], truncated?} once it has loaded, or {error} if it\n"
            "can't be read or hasn't come within [\"timeout\"] s (default 20). Items embedded in the text show in\n"
            "it as [type: name]; text is at most 64 KB.",
            &IbashoAgentListener::readNotecard,
            llsd::map("item_id", LLSD(), "reply", LLSD()));
        add("findRegion",
            "Look a region up by [\"name\"] on the world map, as the map's search does: replies {found: true,\n"
            "region: {name, global: [x, y] (its south-west corner, metres), access, maturity}} or {found: false,\n"
            "similar: [names]} once the map has answered, or {error} if it hasn't within [\"timeout\"] s\n"
            "(default 10).",
            &IbashoAgentListener::findRegion,
            llsd::map("name", LLSD(), "reply", LLSD()));
        add("findPlaces",
            "Search places as the directory's Places tab does, with my account's maturity access: [\"query\"]\n"
            "(words to find; \"\" for any, which the grid may refuse without a category), [\"category\"] (a parcel\n"
            "category: linden, adult, arts, store, educational, game, gather, newcomer, park, home, shopping, stage,\n"
            "other, rental; default any), [\"maturity\"] (the most to include: general, moderate or adult; never more\n"
            "than my account may enter), [\"sort\"] (traffic, the default, or name), [\"start\"] (results to skip)\n"
            "and [\"limit\"] (default 20; one reply stays within a hundred from a multiple of 100). Replies {places:\n"
            "[{id, name, traffic, for_sale, region?, pos? (in the region), global?, rating?, description?, area?}],\n"
            "more, maturity (the most the search included)} once the grid has answered and the places' details have\n"
            "come (those that haven't within a few seconds come without them), or {error} if the grid's search hasn't\n"
            "answered within [\"timeout\"] s (default 20).",
            &IbashoAgentListener::findPlaces,
            llsd::map("reply", LLSD()));
        add("upload",
            "Upload file [\"path\"] (png/tga/bmp/jpg texture, wav sound, anim animation) as a new inventory\n"
            "item, paying the normal L$ upload fee. Optional [\"name\"] (default: file name), [\"desc\"],\n"
            "[\"folder_id\"], [\"type\"] (texture|sound|animation, checked against the file), [\"next_perms\"]\n"
            "(e.g. \"MCT\"; default: the Uploads permission preferences), [\"timeout\"] (s, default 120).\n"
            "Textures: [\"max_size\"] (default 1024, the bake resolution; 2048 may cost more), [\"lossless\"].\n"
            "Refused if the fee is over [\"max_cost\"] (negative counts as 0), IbashoMaxUploadCost, or the pay\n"
            "caps. [\"quote\"]: just reply the fee. Replies {item_id, asset_id, cost, charged, width?,\n"
            "height?} once the item exists.",
            &IbashoAgentListener::upload,
            llsd::map("path", LLSD(), "reply", LLSD()));
        add("wearables",
            "Reply [\"wearables\"]: what's worn, innermost first per type: type, index, item_id, asset_id,\n"
            "name, modifiable, unsaved, textures {slot: texture id}, color. Also [\"bakes_on_mesh\"] (region).",
            &IbashoAgentListener::wearables,
            llsd::map("reply", LLSD()));
        add("createWearable",
            "Create a new wearable of [\"type\"] (tattoo, universal, alpha, skin, shirt, ...) in [\"folder_id\"]\n"
            "(default: Clothing/Body Parts) named [\"name\"], with optional [\"desc\"]. Wears it unless\n"
            "[\"wear\"] is false ([\"replace\"]: take off others of the type). Replies {item_id, worn} once\n"
            "it's created and (if wearing) worn, waiting up to [\"timeout\"] s (default 30).",
            &IbashoAgentListener::createWearable,
            llsd::map("type", LLSD(), "reply", LLSD()));
        add("editWearable",
            "Edit the worn wearable [\"item_id\"] (needs modify rights), then save it and rebake, as the\n"
            "appearance editor's Save does. [\"textures\"] {slot: texture id}, or one [\"slot\"] + [\"texture_id\"];\n"
            "slots by name (\"upper_tattoo\", or just \"upper\"; see wearables) or TEX index. Texture asset ids,\n"
            "or a texture's inventory item id; a null id clears the slot.\n"
            "[\"color\"] [r,g,b] 0-1 (the tint, for types that have one), [\"params\"] {visual param name or id:\n"
            "weight}, [\"name\"] renames. [\"save\"] false keeps the changes local and unsaved (batching).\n"
            "Replies with the wearable as wearables lists it, plus [\"saved\"].",
            &IbashoAgentListener::editWearable,
            llsd::map("item_id", LLSD()));
        add("rebake",
            "Force a rebake of my avatar's textures (Advanced > Rebake Textures).",
            &IbashoAgentListener::rebake);
        add("renderUI",
            "Whether the viewer draws its 2D UI at all (chat console, toasts, floaters, menus; Ctrl+Alt+F1).\n"
            "Optional [\"visible\"] bool sets it. While hidden, mouse clicks don't reach the UI.\n"
            "Replies {visible} (the state after any change).",
            &IbashoAgentListener::renderUI,
            llsd::map("reply", LLSD()));
        add("streamUI",
            "For showing the window to others: the UI draws as usual, minus anything that shows other people's\n"
            "words: chat and IM floaters, nearby chat and IM toasts, chat bubbles, and notifications from or\n"
            "about people (offers, friendship, teleports, group notices, payments, online/offline...).\n"
            "Notifications still arrive and can be answered over LEAP; floaters held back reappear when it goes\n"
            "off. Not saved: a restart starts with it off. Optional [\"on\"] bool sets it; [\"own_chat\"] (default\n"
            "true) lets toasts of my own nearby chat lines show; [\"hide\"] / [\"show\"]: extra notification\n"
            "names to hide / always show; [\"floaters\"]: extra floater names to hide (each list replaces the\n"
            "previous one).\n"
            "Replies {on, visible (the UI is drawn at all, see renderUI), own_chat, held: [what's held back]}.",
            &IbashoAgentListener::streamUI,
            llsd::map("reply", LLSD()));
        add("spectator",
            "Spectator mode: the real mouse and keyboard don't reach the viewer, clicks don't focus its window,\n"
            "and the viewer never moves, hides or confines the real cursor; LEAP input works as usual.\n"
            "[\"on\"] sets it (setting IbashoSpectator: on at every launch, not saved). Replies {on}.",
            &IbashoAgentListener::spectator);
        add("move",
            "Hold movement controls for [\"secs\"] (default 1, max 600), as held keys do but without needing\n"
            "keyboard focus; scripts that took controls (vehicles) see them like keys. [\"forward\"] -1..1\n"
            "(back/forward), [\"strafe\"] -1..1 (+ right), [\"up\"] -1..1 (jump or fly up / crouch or fly down),\n"
            "each held at full while |v| >= 0.5, as a key tap's slower nudge below that (OpenSim hands\n"
            "nudges to scripts as the full control); [\"turn\"] deg/s (+ right/clockwise; a held turn key\n"
            "is 90). [\"run\"] runs meanwhile,\n"
            "[\"fly\"] true/false sets flying first, [\"face\"] a compass bearing (0 N, 90 E) to face first.\n"
            "The camera is left alone. A new move replaces the current one. Replies at the start {id, pos,\n"
            "region, heading, sitting, flying}, or with [\"wait\"] at the end. The end also posts an IbashoEvents\n"
            "{event: \"move_done\", id, reason (time|stop|replaced|locked), secs, moved (m), pos, ...}.",
            [](const LLSD& data) { ibasho::move(data); });
        add("stop",
            "Stop the current move now. Replies {was_moving, pos, ...} (the move_done info if it was moving).",
            [](const LLSD& data) { ibasho::stopMove(data); });
        add("probeGround",
            "What's underfoot at [\"points\"] [[x, y], ...] (region coordinates, at most 256): replies {feet (my\n"
            "feet's height), points: [{terrain, water, hit}]}. hit: the highest object surface under the point\n"
            "(objects the viewer has loaded, invisible ones included), casting down from [\"top\"] (default\n"
            "feet + 10 m) for [\"depth\"] m (default 40); avatars, attachments, phantom objects and grass are\n"
            "passed through; null if no object. Terrain is its own field. Heights are region z.",
            [](const LLSD& data) { ibasho::probeGround(data); });
        add("messages",
            "Today's outgoing chat lines and IMs against IbashoMaxMessagesPerDay (counted by Second Life time\n"
            "date, kept across restarts): replies {date, count, max, left}. Past the limit, chat and IMs are\n"
            "refused with a notice and an IbashoEvents {event: \"refused\"}.",
            &IbashoAgentListener::messages,
            llsd::map("reply", LLSD()));
        add("uploadMesh",
            "Upload a model (.dae, .gltf, .glb) through the model upload floater, as picking the file for\n"
            "the high LOD does: lower LODs and physics as the floater generates them by default, then\n"
            "\"Calculate weights & fee\", then \"Upload\". Optional [\"name\"] (default: from the file),\n"
            "[\"scale\"] (import scale), [\"textures\"] (also upload its textures; costs more), [\"folder_id\"]\n"
            "(default Objects), [\"timeout\"] (s, default 180), [\"keep_open\"] (leave the floater open).\n"
            "[\"lod_med\"], [\"lod_low\"], [\"lod_lowest\"], [\"physics\"]: model files for those LODs (absolute\n"
            "paths), loaded after the high LOD as the floater's \"Load from file\" does; each file's objects must\n"
            "have the high LOD's object names (optionally with _LOD2/_LOD1/_LOD0/_PHYS).\n"
            "Refused if the fee is over [\"max_cost\"] (negative counts as 0), IbashoMaxUploadCost, or the pay\n"
            "caps. [\"quote\"]: stop after calculating. Replies {name, cost, weights {land_impact, streaming,\n"
            "physics, server, price_breakdown}, lods {high|med|low|lowest|physics: {tris, from}}, refusal?} for a quote, else\n"
            "adds {item_id, asset_id, charged} once the item exists; errors carry the floater's [\"log\"].",
            [](const LLSD& data) { ibasho::uploadMesh(data); },
            llsd::map("path", LLSD()));
        add("hideAvatars",
            "Don't draw chosen avatars in the window (so not on a screen capture of it): body, attachments (rigged\n"
            "too), their attached lights and particles, name tag and hover text. Snapshots still show them unless\n"
            "[\"in_snapshots\"] is true. [\"mode\"]: \"off\", \"list\" (hide [\"ids\"]) or \"all_except\" (hide\n"
            "everyone but me and [\"except\"], newcomers included). Each list given replaces the last; both are\n"
            "kept across mode changes. Not saved: a restart starts with it off. Sounds are untouched.\n"
            "Replies as getHidden, plus [\"grew\"]: whether anyone may have become hidden by this call.",
            [](const LLSD& data) { ibasho::hideAvatars(data); });
        add("textureStats",
            "Where texture memory goes: the viewer's totals and discard bias, the fetched textures by boost level\n"
            "(count, MB at their current discard level, raw copies, faceless, on screen, by size), and the\n"
            "[\"top\"] (15, at most 200) largest. MB are real bytes, not the budget's doubled count.",
            [](const LLSD& data) { Response response(ibasho::textureStats(data), data); });
        add("getHidden",
            "Avatar hiding state: {mode, ids, except, in_snapshots, hidden_now: [avatar ids hidden at the\n"
            "moment], shown_now: [the other avatars the viewer knows of, not me]}.",
            [](const LLSD& data) { ibasho::getHidden(data); });
    }

private:
    void renderUI(const LLSD& data)
    {
        Response response(LLSD(), data);
        constexpr U32 ui = LLPipeline::RENDER_DEBUG_FEATURE_UI;
        if (data.has("visible") && data["visible"].asBoolean() != gPipeline.hasRenderDebugFeatureMask(ui))
            LLPipeline::toggleRenderDebugFeature(ui);
        response["visible"] = gPipeline.hasRenderDebugFeatureMask(ui);
    }

    void streamUI(const LLSD& data)
    {
        Response response(LLSD(), data);
        if (data.has("own_chat"))
            streamui::sOwnChat = data["own_chat"].asBoolean();
        if (data.has("hide"))
            streamui::sExtraHide = streamui::nameSet(data["hide"]);
        if (data.has("show"))
            streamui::sExtraShow = streamui::nameSet(data["show"]);
        if (data.has("floaters"))
            streamui::sExtraFloaters = streamui::nameSet(data["floaters"]);
        if (data.has("on"))
            streamui::setOn(data["on"].asBoolean());
        else if (streamui::sOn)
            streamui::sweep(); // options may have changed what should be held
        response["on"] = streamui::sOn;
        response["visible"] = gPipeline.hasRenderDebugFeatureMask(LLPipeline::RENDER_DEBUG_FEATURE_UI);
        response["own_chat"] = streamui::sOwnChat;
        LLSD held = LLSD::emptyArray();
        for (auto& [ptr, handle] : streamui::sHeld)
            if (LLFloater* f = handle.get())
                held.append(streamui::describe(f));
        response["held"] = held;
    }

    void messages(const LLSD& data)
    {
        Response response(ibasho::messageCount(), data);
    }

    void spectator(const LLSD& data)
    {
        Response response(LLSD(), data);
        if (data.has("on"))
            gSavedSettings.setBOOL("IbashoSpectator", data["on"].asBoolean()); // initSpectator's signal applies it
        response["on"] = gSavedSettings.getBOOL("IbashoSpectator");
    }

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
        // give_money checked the caps again and counted the payment (ibasho::allowSpend)
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
        response["my_voice"] = LLVoiceClient::instanceExists() && LLVoiceClient::getInstance()->isVoiceWorking();
    }

    void people(const LLSD& data)
    {
        Response response(LLSD(), data);
        F32 radius = data.has("dist") ? (F32)data["dist"].asReal() : 64.f;
        uuid_vec_t ids;
        std::vector<LLVector3d> positions;
        LLVector3d me = gAgent.getPositionGlobal();
        LLWorld::getInstance()->getAvatars(&ids, &positions, me, radius);
        LLVoiceClient* voice = LLVoiceClient::instanceExists() ? LLVoiceClient::getInstance() : nullptr;
        if (voice && !voice->isVoiceWorking())
            voice = nullptr;
        std::vector<std::pair<F64, LLSD>> found;
        for (size_t i = 0; i < ids.size(); ++i)
        {
            if (ids[i] == gAgentID)
                continue;
            F64 d = dist_vec(positions[i], me);
            LLSD entry = llsd::map("id", ids[i], "dist", d,
                                   "pos", ll_sd_from_vector3(gAgent.getPosAgentFromGlobal(positions[i])),
                                   "friend", LLAvatarTracker::instance().isBuddy(ids[i]));
            if (voice && voice->isParticipant(ids[i]))
            {
                entry["in_voice"] = true;
                if (voice->getIsSpeaking(ids[i]))
                    entry["speaking"] = true;
            }
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

    void findAvatars(const LLSD& data)
    {
        // Read as the avatar picker reads it: "first.last" is "first last", and "Resident" isn't part of a name.
        std::string name = data["name"].asString();
        std::replace(name.begin(), name.end(), '.', ' ');
        LLStringUtil::trim(name);
        size_t sep = name.find_first_of(" _");
        if (sep != std::string::npos)
        {
            std::string last = name.substr(sep + 1);
            LLStringUtil::trim(last);
            LLStringUtil::toLower(last);
            if (last == "resident")
                name = name.substr(0, sep);
        }
        if (name.empty())
            return replyError(data, "need a name");
        LLViewerRegion* region = gAgent.getRegion();
        if (!region || LLStartUp::getStartupState() < STATE_STARTED)
            return replyError(data, "not in world yet");
        S32 limit = data.has("limit") ? llclamp(data["limit"].asInteger(), 1, 100) : 20;
        F64 timeout = data.has("timeout") ? llclamp(data["timeout"].asReal(), 1.0, 120.0) : 10.0;

        auto search = std::make_shared<lookup::Search>();
        std::string url = region->getCapability("AvatarPickerSearch");
        if (!url.empty())
        {
            if (url.back() != '/')
                url += "/"; // as the picker does, so the query parses
            url += "?page_size=100&names=" + LLURI::escape(name);
            LLCoros::instance().launch("IbashoAgent::findAvatars",
                                       [url, timeout, search]() { lookup::searchCoro(url, timeout, search); });
        }
        else
        {
            search->query_id.generate();
            lookup::sSearches[search->query_id] = search;
            LLMessageSystem* msg = gMessageSystem;
            msg->newMessage("AvatarPickerRequest");
            msg->nextBlock("AgentData");
            msg->addUUID("AgentID", gAgent.getID());
            msg->addUUID("SessionID", gAgent.getSessionID());
            msg->addUUID("QueryID", search->query_id);
            msg->nextBlock("Data");
            msg->addString("Name", name);
            gAgent.sendReliableMessage();
        }

        ibasho::deferReply(data, timeout, [search, limit](LLSD& reply, bool expired)
        {
            bool by_message = search->query_id.notNull();
            bool ready = search->done && (!by_message || lookup::now() - search->last_at >= lookup::SETTLE);
            if (!ready && !expired)
                return false;
            if (by_message)
                lookup::sSearches.erase(search->query_id);
            if (!search->error.empty())
            {
                reply["error"] = search->error;
            }
            else if (!search->done)
            {
                reply["error"] = "no answer from the grid's avatar search in time";
            }
            else
            {
                LLSD avatars = LLSD::emptyArray();
                for (const auto& avatar : llsd::inArray(search->avatars))
                    if (avatars.size() < limit)
                        avatars.append(avatar);
                reply["avatars"] = avatars;
            }
            return true;
        });
    }

    void profile(const LLSD& data)
    {
        LLUUID id = data["id"].asUUID();
        if (id.isNull())
            return replyError(data, "need an avatar id");
        if (!gAgent.getRegion() || LLStartUp::getStartupState() < STATE_STARTED)
            return replyError(data, "not in world yet");
        F64 timeout = data.has("timeout") ? llclamp(data["timeout"].asReal(), 1.0, 120.0) : 10.0;
        LLAvatarNameCache::get(id, [](const LLUUID&, const LLAvatarName&) {}); // for the names in the reply

        auto p = std::make_shared<lookup::Profile>();
        p->id = id;
        std::string url = gAgent.getRegionCapability("AgentProfile");
        bool by_message = url.empty();
        if (!by_message)
        {
            url += "/" + id.asString();
            LLCoros::instance().launch("IbashoAgent::profile",
                                       [url, timeout, p]() { lookup::profileCoro(url, timeout, p); });
        }
        else
        {
            // The properties request brings the groups reply too; picks are asked for on their own.
            lookup::watch(p);
            LLAvatarPropertiesProcessor::getInstance()->sendAvatarLegacyPropertiesRequest(id);
            std::vector<std::string> strings{ id.asString() };
            send_generic_message("avatarpicksrequest", strings);
        }

        ibasho::deferReply(data, timeout, [p, by_message](LLSD& reply, bool expired)
        {
            F64 now = lookup::now();
            bool ready = by_message
                ? (p->props && p->groups && p->picks && now - p->last_at >= lookup::SETTLE) ||
                  (p->props && now - p->props_at >= lookup::EXTRA_WAIT)
                : p->done;
            if (!ready && !expired)
                return false;
            if (by_message)
                lookup::unwatch(p);
            if (!p->error.empty())
            {
                reply["error"] = p->error;
                return true;
            }
            if (!(by_message ? p->props : p->done))
            {
                reply["error"] = "no profile came in time (an unknown id?)";
                return true;
            }
            for (const auto& [key, value] : llsd::inMap(p->out))
                reply[key] = value;
            reply["id"] = p->id;
            LLSD name = nameSD(p->id);
            if (name.isDefined())
            {
                reply["display_name"] = name["display_name"];
                reply["username"] = name["username"];
            }
            if (by_message && !(p->groups && p->picks))
                reply["partial"] = true;
            if (p->id == gAgentID && !lookup::nextNameChange().empty())
                reply["display_name_next_change"] = lookup::nextNameChange();
            return true;
        });
    }

    void setProfile(const LLSD& data)
    {
        if (!gAgent.getRegion() || LLStartUp::getStartupState() < STATE_STARTED)
            return replyError(data, "not in world yet");
        LLSD fields = LLSD::emptyMap();
        if (data.has("about"))
            fields["sl_about_text"] = data["about"].asString();
        if (data.has("image_id"))
            fields["sl_image_id"] = data["image_id"].asUUID();
        if (fields.size() == 0)
            return replyError(data, "nothing to set: give about and/or image_id");
        F64 timeout = data.has("timeout") ? llclamp(data["timeout"].asReal(), 1.0, 120.0) : 20.0;

        std::string url = gAgent.getRegionCapability("AgentProfile");
        if (!url.empty())
        {
            auto save = std::make_shared<lookup::ProfileSave>();
            url += "/" + gAgentID.asString();
            LLCoros::instance().launch("IbashoAgent::setProfile",
                                       [url, timeout, fields, save]() { lookup::saveProfileCoro(url, timeout, fields, save); });
            ibasho::deferReply(data, timeout * fields.size() + 1.0, [save](LLSD& reply, bool expired)
            {
                if (!save->done && !expired)
                    return false;
                if (!save->error.empty())
                    reply["error"] = save->error;
                else if (!save->done)
                    reply["error"] = "the grid hasn't answered yet (the change may still go through)";
                return true;
            });
            return;
        }

        auto p = std::make_shared<lookup::Profile>();
        p->id = gAgentID;
        lookup::watch(p);
        LLAvatarPropertiesProcessor::getInstance()->sendAvatarLegacyPropertiesRequest(gAgentID);
        ibasho::deferReply(data, timeout, [p, fields](LLSD& reply, bool expired)
        {
            if (!p->props && !expired)
                return false;
            lookup::unwatch(p);
            if (!p->props)
            {
                reply["error"] = "the grid didn't send my current profile in time, so nothing was changed";
                return true;
            }
            lookup::sendLegacyProfile(*p, fields);
            reply["by_message"] = true;
            return true;
        });
    }

    void setDisplayName(const LLSD& data)
    {
        if (!gAgent.getRegion() || LLStartUp::getStartupState() < STATE_STARTED)
            return replyError(data, "not in world yet");
        if (!data["name"].isString())
            return replyError(data, "need a name (\"\" for my username)");
        std::string name = data["name"].asString();
        LLStringUtil::trim(name);
        if (gAgent.getRegionCapability("SetDisplayName").empty()
            || !LLAvatarNameCache::getInstance()->hasNameLookupURL())
            return replyError(data, "this grid has no display names: only the account name shows");
        LLAvatarName av;
        if (!LLAvatarNameCache::get(gAgentID, &av))
            return replyError(data, "my own name hasn't loaded yet: try again in a moment");
        std::string next = lookup::nextNameChange();
        if (!next.empty())
        {
            LLEventAPI::Response response(llsd::map("next_change", next), data);
            response.error("the display name can change about once a week; the next change can be made from " +
                           next + " (UTC)");
            return;
        }
        if (name == av.getUserName())
            name.clear(); // a reset, as the window sends it
        constexpr size_t MAX_CHARS = 31;
        if (utf8str_to_wstring(name).size() > MAX_CHARS)
            return replyError(data, llformat("a display name is at most %d characters", (S32)MAX_CHARS));

        auto set = std::make_shared<lookup::NameSet>();
        LLViewerDisplayName::set(name, [set](bool success, const std::string& reason, const LLSD& content)
        {
            set->done = true;
            set->content = content;
            if (success)
                return;
            LLSD desc = content["error_description"];
            std::string lang = LLUI::getLanguage();
            set->error = desc.has(lang) ? desc[lang].asString()
                       : desc.has("en") ? desc["en"].asString()
                       : !content["error_tag"].asString().empty() ? content["error_tag"].asString()
                       : !reason.empty() ? reason : std::string("the grid refused it");
        });
        F64 timeout = data.has("timeout") ? llclamp(data["timeout"].asReal(), 1.0, 120.0) : 30.0;
        ibasho::deferReply(data, timeout, [set, name](LLSD& reply, bool expired)
        {
            if (!set->done && !expired)
                return false;
            if (!set->done)
            {
                reply["error"] = "the grid hasn't answered yet (the change may still go through)";
                return true;
            }
            if (!set->error.empty())
            {
                reply["error"] = set->error;
                return true;
            }
            reply["display_name"] = set->content.has("display_name") ? set->content["display_name"].asString() : name;
            std::string next = lookup::nextNameChange();
            if (!next.empty())
                reply["next_change"] = next;
            else if (set->content.has("display_name_next_update"))
                reply["next_change"] = set->content["display_name_next_update"].asDate().asString();
            return true;
        });
    }

    void readNotecard(const LLSD& data)
    {
        LLUUID item_id = data["item_id"].asUUID();
        if (item_id.isNull())
            return replyError(data, "need an item_id: a notecard in my inventory");
        if (!gAgent.getRegion() || LLStartUp::getStartupState() < STATE_STARTED || !gAssetStorage)
            return replyError(data, "not in world yet");
        LLViewerInventoryItem* item = gInventory.getItem(item_id);
        if (item && item->getIsLinkType())
            item = item->getLinkedItem();
        if (!item)
            return replyError(data, "no such item in my inventory");
        if (item->getType() != LLAssetType::AT_NOTECARD)
            return replyError(data, "\"" + item->getName() + "\" isn't a notecard");
        if (!gAgent.allowOperation(PERM_COPY, item->getPermissions(), GP_OBJECT_MANIPULATE))
            return replyError(data, "\"" + item->getName() + "\" is no-copy: its text can't be read (the viewer's "
                                    "notecard window won't show it either)");
        F64 timeout = data.has("timeout") ? llclamp(data["timeout"].asReal(), 1.0, 120.0) : 20.0;

        auto r = std::make_shared<notecards::Read>();
        r->out = llsd::map("item_id", item->getUUID(), "name", item->getName());
        if (item->getAssetUUID().isNull())
        {
            r->out["text"] = ""; // a notecard nobody has written in yet
            r->out["items"] = LLSD::emptyArray();
            r->done = true;
        }
        else
        {
            gAssetStorage->getInvItemAsset(LLHost(), gAgent.getID(), gAgent.getSessionID(),
                                           item->getPermissions().getOwner(), LLUUID::null, item->getUUID(),
                                           item->getAssetUUID(), item->getType(),
                                           [r](const LLUUID& asset_id, LLAssetType::EType type, void*, S32 status,
                                               LLExtStat) { notecards::loaded(*r, asset_id, type, status); },
                                           nullptr, true);
        }
        ibasho::deferReply(data, timeout, [r](LLSD& reply, bool expired)
        {
            if (!r->done && !expired)
                return false;
            if (!r->done)
                reply["error"] = "the notecard didn't arrive in time";
            else if (!r->error.empty())
                reply["error"] = r->error;
            else
                for (const auto& [key, value] : llsd::inMap(r->out))
                    reply[key] = value;
            return true;
        });
    }

    void findRegion(const LLSD& data)
    {
        std::string name = data["name"].asString();
        LLStringUtil::trim(name);
        if (name.empty())
            return replyError(data, "need a region's name");
        if (!gAgent.getRegion() || LLStartUp::getStartupState() < STATE_STARTED)
            return replyError(data, "not in world yet");
        F64 timeout = data.has("timeout") ? llclamp(data["timeout"].asReal(), 1.0, 60.0) : 10.0;
        auto s = std::make_shared<regions::Search>();
        s->want = name;
        regions::sSearches.push_back(s);
        LLWorldMapMessage::getInstance()->sendNamedRegionRequest(name);
        ibasho::deferReply(data, timeout, [s](LLSD& reply, bool expired)
        {
            bool found = s->found.isDefined();
            if (!found && !s->ended && !expired)
                return false;
            regions::forget(s);
            if (found)
            {
                reply["found"] = true;
                reply["region"] = s->found;
            }
            else if (s->ended)
            {
                reply["found"] = false;
                reply["similar"] = s->similar;
            }
            else
            {
                reply["error"] = "the world map didn't answer in time";
            }
            return true;
        });
    }

    void findPlaces(const LLSD& data)
    {
        if (!gAgent.getRegion() || LLStartUp::getStartupState() < STATE_STARTED)
            return replyError(data, "not in world yet");
        std::string query = data["query"].asString();
        LLStringUtil::trim(query);
        if (query.size() > 200)
            return replyError(data, "query: at most 200 bytes");
        S32 category = LLParcel::C_ANY;
        const std::string cat = data["category"].asString();
        if (!cat.empty() && cat != "any")
        {
            category = LLParcel::C_NONE;
            for (S32 i = LLParcel::C_NONE + 1; i < LLParcel::C_COUNT; ++i)
                if (cat == LLParcel::getCategoryString((LLParcel::ECategory)i))
                    category = i;
            if (category == LLParcel::C_NONE)
                return replyError(data, "no category " + cat + " (linden, adult, arts, store, educational, game, "
                                        "gather, newcomer, park, home, shopping, stage, other, rental, any)");
        }
        S32 want = data.has("maturity") ? places::levelOf(data["maturity"].asString()) : 2;
        if (want < 0)
            return replyError(data, "maturity: general, moderate or adult");
        U32 flags = DFQ_INC_PG;
        S32 most = 0;
        if (want >= 1 && gAgent.canAccessMature())
        {
            flags |= DFQ_INC_MATURE;
            most = 1;
        }
        if (want >= 2 && gAgent.canAccessAdult())
        {
            flags |= DFQ_INC_ADULT;
            most = 2;
        }
        const std::string sort = data.has("sort") ? data["sort"].asString() : "traffic";
        if (sort == "traffic")
            flags |= DFQ_DWELL_SORT;
        else if (sort == "name")
            flags |= DFQ_NAME_SORT | DFQ_SORT_ASC;
        else
            return replyError(data, "sort: traffic or name"); // Second Life's sends the busiest whatever the order
        const S32 start = llclamp(data["start"].asInteger(), 0, 100000);
        const S32 skip = start % places::BATCH;
        const S32 limit = llmin(data.has("limit") ? llclamp(data["limit"].asInteger(), 1, places::BATCH) : 20,
                                places::BATCH - skip);
        F64 timeout = data.has("timeout") ? llclamp(data["timeout"].asReal(), 2.0, 120.0) : 20.0;

        auto s = std::make_shared<places::Search>();
        s->query_id.generate();
        places::sSearches[s->query_id] = s;
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessage("DirPlacesQuery");
        msg->nextBlock("AgentData");
        msg->addUUID("AgentID", gAgent.getID());
        msg->addUUID("SessionID", gAgent.getSessionID());
        msg->nextBlock("QueryData");
        msg->addUUID("QueryID", s->query_id);
        msg->addString("QueryText", query);
        msg->addU32("QueryFlags", flags);
        msg->addS8("Category", (S8)category);
        msg->addString("SimName", "");
        msg->addS32Fast(_PREHASH_QueryStart, start - skip);
        gAgent.sendReliableMessage();

        ibasho::deferReply(data, timeout, [s, skip, limit, most, sort](LLSD& reply, bool expired)
        {
            const F64 now = lookup::now();
            if (!s->paged)
            {
                bool settled = s->answered && (now - s->last_at >= places::SETTLE || (S32)s->hits.size() > places::BATCH);
                if (!settled && !expired)
                    return false;
                if (!s->answered)
                {
                    places::sSearches.erase(s->query_id);
                    reply["error"] = "no answer from the grid's place search in time (on OpenSim, only grids with a "
                                     "search module answer)";
                    return true;
                }
                const U32 status = s->status;
                const char* refusal =
                    (status & STATUS_SEARCH_PLACES_BANNEDWORD) ? "the grid's search refuses a word in it" :
                    (status & STATUS_SEARCH_PLACES_SHORTSTRING) ?
                        "the grid's search wants longer words (Second Life's, 3 letters or more)" :
                    (status & STATUS_SEARCH_PLACES_SEARCHDISABLED) ? "the grid's place search is turned off" : nullptr;
                if (refusal && s->hits.empty())
                {
                    places::sSearches.erase(s->query_id);
                    reply["error"] = refusal;
                    return true;
                }
                // The 101st, the last to arrive, only says there are more (as LLPanelDirBrowser::showNextButton).
                s->paged = true;
                s->paged_at = now;
                s->more = (S32)s->hits.size() > skip + limit;
                if ((S32)s->hits.size() > places::BATCH)
                    s->hits.resize(places::BATCH);
                // the grid sorts each message; across messages, sort as the directory's list does
                std::stable_sort(s->hits.begin(), s->hits.end(), [&sort](const places::Hit& a, const places::Hit& b)
                {
                    if (sort == "name")
                        return LLStringUtil::compareDictInsensitive(a.name, b.name) < 0;
                    return a.dwell > b.dwell;
                });
                for (S32 i = skip; i < skip + limit && i < (S32)s->hits.size(); ++i)
                {
                    const LLUUID& id = s->hits[i].id;
                    s->page.push_back(id);
                    LLRemoteParcelInfoProcessor::getInstance()->addObserver(id, s.get());
                    LLRemoteParcelInfoProcessor::getInstance()->sendParcelInfoRequest(id);
                }
                if (!expired && !s->page.empty())
                    return false;
            }
            if (!expired && s->details.size() < s->page.size() && now - s->paged_at < places::DETAILS_WAIT)
                return false;
            s->forget();
            places::sSearches.erase(s->query_id);
            LLSD found = LLSD::emptyArray();
            for (S32 i = skip; i < skip + limit && i < (S32)s->hits.size(); ++i)
            {
                const places::Hit& hit = s->hits[i];
                auto d = s->details.find(hit.id);
                const LLParcelData* details = d == s->details.end() ? nullptr : &d->second;
                if (details && places::levelOfFlags(details->flags) > most)
                    continue; // above what was asked for: the grid shouldn't send it
                found.append(places::describe(hit, details));
            }
            reply["places"] = found;
            reply["more"] = s->more;
            reply["maturity"] = places::LEVELS[most];
            return true;
        });
    }

    void upload(const LLSD& data)
    {
        const std::string path = data["path"].asString();
        if (!gDirUtilp->fileExists(path))
            return replyError(data, "no such file: " + path);
        const std::string ext = gDirUtilp->getExtension(path);
        LLAssetType::EType asset_type = LLAssetType::AT_NONE;
        if (ext == "bvh")
            return replyError(data, "BVH needs the animation preview to convert it; upload an .anim");
        if (!LLResourceUploadInfo::findAssetTypeOfExtension(ext, asset_type))
            return replyError(data, "can't upload ." + ext + " files (textures: png tga bmp jpg; wav; anim)");
        const char* type_name = asset_type == LLAssetType::AT_TEXTURE ? "texture"
                              : asset_type == LLAssetType::AT_SOUND   ? "sound" : "animation";
        if (data.has("type") && data["type"].asString() != type_name)
            return replyError(data, std::string("that file is a ") + type_name + ", not a " + data["type"].asString());

        LLUUID folder_id = data["folder_id"].asUUID();
        if (folder_id.notNull() && !gInventory.getCategory(folder_id))
            return replyError(data, "no such folder");
        std::string native_path = path; // so the default name is the file's own, not its folders too
        LLStringUtil::replaceChar(native_path, '/', gDirUtilp->getDirDelimiter()[0]);
        std::string name = inventoryText(data.has("name") ? data["name"] : LLSD(gDirUtilp->getBaseFileName(native_path, true)),
                                         DB_INV_ITEM_NAME_STR_LEN);
        std::string desc = inventoryText(data["desc"], DB_INV_ITEM_DESC_STR_LEN);
        U32 next_perms = data.has("next_perms") ? permMask(data["next_perms"].asString())
                                                : LLFloaterPerms::getNextOwnerPerms("Uploads");
        U32 group_perms = LLFloaterPerms::getGroupPerms("Uploads");
        U32 everyone_perms = LLFloaterPerms::getEveryonePerms("Uploads");
        const LLAgentBenefits& benefits = LLAgentBenefitsMgr::current();

        // Textures are encoded here (as the image preview floater does) so the fee can follow the
        // final size; sounds and animations go through LLNewFileResourceUploadInfo's own conversion.
        LLPointer<LLImageJ2C> j2c;
        S32 width = 0, height = 0, cost = 0;
        if (asset_type == LLAssetType::AT_TEXTURE)
        {
            S32 max_size = data.has("max_size") ? data["max_size"].asInteger() : 1024;
            max_size = llclamp(max_size, 64, llmin(gSavedSettings.getS32("max_texture_dimension_X"),
                                                   gSavedSettings.getS32("max_texture_dimension_Y")));
            std::string err;
            j2c = encodeTexture(path, max_size, data["lossless"].asBoolean(), width, height, err);
            if (j2c.isNull())
                return replyError(data, err);
            cost = benefits.getTextureUploadCost(width, height);
        }
        else if (!benefits.findUploadCost(asset_type, cost))
        {
            return replyError(data, "no upload price known for that type");
        }

        S32 cap = gSavedSettings.getS32("IbashoMaxUploadCost");
        std::string refusal;
        if (cost < 0)
            refusal = "the grid hasn't said what this upload costs";
        else if (data.has("max_cost") && cost > llmax(0, data["max_cost"].asInteger()))   // negative: nothing
            refusal = llformat("the fee is L$%d, over max_cost L$%d", cost, llmax(0, data["max_cost"].asInteger()));
        else if (cost > cap)
            refusal = llformat("the fee is L$%d, over the upload cap of L$%d (IbashoMaxUploadCost)", cost, cap);
        else
            refusal = ibasho::spendRefusal(cost);
        LLSD info = llsd::map("cost", cost, "type", type_name, "name", name);
        if (width)
        {
            info["width"] = width;
            info["height"] = height;
        }
        if (data["quote"].asBoolean())
        {
            if (!refusal.empty())
                info["refusal"] = refusal;
            Response response(info, data);
            return;
        }
        if (!refusal.empty())
            return replyError(data, refusal);
        if (gAgent.getRegionCapability("NewFileAgentInventory").empty())
            return replyError(data, "this region has no NewFileAgentInventory capability");

        auto state = std::make_shared<UploadState>();
        state->cost = cost;
        LLResourceUploadInfo::ptr_t upload_info;
        if (j2c.notNull())
        {
            LLTransactionID tid;
            tid.generate();
            {
                LLFileSystem file(tid.makeAssetID(gAgent.getSecureSessionID()), LLAssetType::AT_TEXTURE, LLFileSystem::WRITE);
                file.write(j2c->getData(), j2c->getDataSize());
            }
            upload_info = std::make_shared<IbashoUploadInfo<LLResourceUploadInfo>>(
                state, tid, LLAssetType::AT_TEXTURE, name, desc, 0, LLFolderType::FT_NONE, LLInventoryType::IT_NONE,
                next_perms, group_perms, everyone_perms, cost, folder_id, false);
        }
        else
        {
            upload_info = std::make_shared<IbashoUploadInfo<LLNewFileResourceUploadInfo>>(
                state, path, name, desc, 0, LLFolderType::FT_NONE, LLInventoryType::IT_NONE,
                next_perms, group_perms, everyone_perms, cost, folder_id, false);
        }
        upload_new_resource(upload_info);

        F64 timeout = data.has("timeout") ? data["timeout"].asReal() : 120.0;
        ibasho::deferReply(data, timeout, [state, info](LLSD& reply, bool expired)
        {
            if (!state->done)
            {
                if (expired)
                    reply["error"] = "timed out; the upload may still finish (watch inventory and money events)";
                return false;
            }
            for (const auto& [k, v] : llsd::inMap(info))
                reply[k] = v;
            if (!state->error.empty())
                reply["error"] = state->error;
            reply["item_id"] = state->item_id;
            reply["asset_id"] = state->asset_id;
            reply["charged"] = state->charged;
            return true;
        });
    }

    void wearables(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLViewerRegion* region = gAgent.getRegion();
        response["bakes_on_mesh"] = region && region->bakesOnMeshEnabled();
        response["wearables"] = LLSD::emptyArray();
        for (S32 t = 0; t < LLWearableType::WT_COUNT; ++t)
        {
            LLWearableType::EType type = (LLWearableType::EType)t;
            for (U32 i = 0; i < gAgentWearables.getWearableCount(type); ++i)
            {
                LLViewerWearable* wearable = gAgentWearables.getViewerWearable(type, i);
                if (wearable)
                    response["wearables"].append(describeWearable(wearable, i));
            }
        }
    }

    void createWearable(const LLSD& data)
    {
        std::string type_name = data["type"].asString();
        LLStringUtil::toLower(type_name);
        LLWearableType::EType type = LLWearableType::getInstance()->typeNameToType(type_name);
        if ((S32)type < 0 || type >= LLWearableType::WT_COUNT)
            return replyError(data, "unknown wearable type " + type_name + "; one of " + wearableTypeNames());
        if (!isAgentAvatarValid() || !gAgent.getRegion())
            return replyError(data, "no avatar");
        if (type == LLWearableType::WT_UNIVERSAL && !gAgent.getRegion()->bakesOnMeshEnabled())
            return replyError(data, "this region doesn't support bakes on mesh, so no universal wearables");
        LLUUID folder_id = data["folder_id"].asUUID();
        if (folder_id.notNull() && !gInventory.getCategory(folder_id))
            return replyError(data, "no such folder");
        LLSD updates = LLSD::emptyMap();
        if (data.has("name"))
            updates["name"] = inventoryText(data["name"], DB_INV_ITEM_NAME_STR_LEN);
        if (data.has("desc"))
            updates["desc"] = inventoryText(data["desc"], DB_INV_ITEM_DESC_STR_LEN);
        bool wear = !data.has("wear") || data["wear"].asBoolean();
        bool replace = data["replace"].asBoolean();

        auto created = std::make_shared<LLUUID>();
        LLAgentWearables::createWearable(type, false, folder_id, [created, updates, wear, replace](const LLUUID& item_id)
        {
            *created = item_id;
            if (item_id.isNull())
                return;
            if (updates.size())
                update_inventory_item(item_id, updates, nullptr);
            if (wear)
                LLAppearanceMgr::instance().wearItemOnAvatar(item_id, true, replace);
        });

        F64 timeout = data.has("timeout") ? data["timeout"].asReal() : 30.0;
        ibasho::deferReply(data, timeout, [created, wear](LLSD& reply, bool expired)
        {
            if (created->isNull())
            {
                if (expired)
                    reply["error"] = "timed out waiting for the server to create the item";
                return false;
            }
            reply["item_id"] = *created;
            bool worn = gAgentWearables.getWearableFromItemID(*created) != nullptr;
            reply["worn"] = worn;
            return worn || !wear;
        });
    }

    void editWearable(const LLSD& data)
    {
        Response response(LLSD(), data);
        if (!isAgentAvatarValid())
            return response.error("no avatar");
        LLUUID item_id = data["item_id"].asUUID();
        LLViewerWearable* wearable = gAgentWearables.getWearableFromItemID(item_id);
        if (!wearable)
            return response.error("not worn (only worn wearables can be edited; wear it first)");
        if (!gAgentWearables.isWearableModifiable(item_id))
            return response.error("no modify permission");
        U32 index = 0;
        if (!gAgentWearables.getWearableIndex(wearable, index))
            return response.error("wearable not found among worn ones");
        LLWearableType::EType type = wearable->getType();

        // Check everything before changing anything.
        LLSD textures_in = data.has("textures") ? data["textures"] : LLSD::emptyMap();
        if (data.has("slot"))
            textures_in[data["slot"].asString()] = data["texture_id"];
        std::vector<std::pair<ETextureIndex, LLUUID>> textures;
        for (const auto& [slot, id] : llsd::inMap(textures_in))
        {
            ETextureIndex te = findTextureSlot(type, slot);
            if (te == TEX_INVALID)
                return response.error("no texture slot " + slot + " on a " +
                                      LLWearableType::getInstance()->getTypeName(type) + "; slots: " + slotNames(type));
            LLUUID tex = id.asUUID();
            LLViewerInventoryItem* item = gInventory.getItem(tex); // a texture's item id works too
            if (item && item->getType() == LLAssetType::AT_TEXTURE)
                tex = item->getAssetUUID();
            textures.emplace_back(te, tex);
        }
        ETextureIndex tint = TEX_INVALID;
        if (data.has("color") && !colorSlot(type, tint))
            return response.error("this wearable type has no colour");
        std::vector<std::pair<S32, F32>> params;
        for (const auto& [key, weight] : llsd::inMap(data.has("params") ? data["params"] : LLSD::emptyMap()))
        {
            LLVisualParam* param = nullptr;
            if (!key.empty() && std::all_of(key.begin(), key.end(), ::isdigit))
                param = wearable->getVisualParam(std::stoi(key));
            else if (LLVisualParam* avatar_param = gAgentAvatarp->getVisualParam(key.c_str()))
                param = wearable->getVisualParam(avatar_param->getID());
            if (!param)
                return response.error("no visual param " + key + " on this wearable");
            params.emplace_back(param->getID(), (F32)weight.asReal());
        }
        std::string new_name = data.has("name") ? inventoryText(data["name"], DB_INV_ITEM_NAME_STR_LEN) : std::string();

        // Apply, the way the appearance editor's controls do.
        for (const auto& [te, id] : textures)
        {
            LLViewerFetchedTexture* image =
                LLViewerTextureManager::getFetchedTexture(id.isNull() || id == IMG_DEFAULT ? IMG_DEFAULT_AVATAR : id);
            gAgentAvatarp->setLocalTexture(te, image, false, index);
        }
        if (tint != TEX_INVALID)
        {
            const LLSD& c = data["color"];
            wearable->setClothesColor(tint, LLColor4((F32)c[0].asReal(), (F32)c[1].asReal(), (F32)c[2].asReal(), 1.f));
        }
        for (const auto& [id, weight] : params)
            wearable->setVisualParamWeight(id, weight);
        if (!params.empty())
        {
            wearable->writeToAvatar(gAgentAvatarp);
            gAgentAvatarp->updateVisualParams();
        }
        gAgentAvatarp->wearableUpdated(type);

        bool save = !data.has("save") || data["save"].asBoolean();
        bool renamed = false;
        if (!new_name.empty())
        {
            LLViewerInventoryItem* item = gInventory.getItem(wearable->getItemID());
            renamed = !item || item->getName() != new_name;
        }
        if (save && (wearable->isDirty() || renamed))
            saveWornWearable(wearable, index, new_name);
        // Saving swaps in a copy with a new asset (the old wearable is reverted).
        LLViewerWearable* now = gAgentWearables.getViewerWearable(type, index);
        response.setResponse(describeWearable(now ? now : wearable, index));
        response["saved"] = now && now != wearable;
    }

    void rebake(const LLSD& data)
    {
        Response response(LLSD(), data);
        if (!isAgentAvatarValid())
            return response.error("no avatar");
        handle_rebake_textures();
    }
};

IbashoAgentListener sIbashoAgentListener;
} // namespace

// findAvatars and profile answers that come by message (see lookup above).
bool ibasho::onAvatarPickerReply(LLMessageSystem* msg, const LLUUID& query_id)
{
    auto found = lookup::sSearches.find(query_id);
    if (found == lookup::sSearches.end())
        return false;
    lookup::Search& search = *found->second;
    S32 count = msg->getNumberOfBlocksFast(_PREHASH_Data);
    for (S32 i = 0; i < count; ++i)
    {
        LLUUID id;
        std::string first, last;
        msg->getUUIDFast(_PREHASH_Data, _PREHASH_AvatarID, id, i);
        msg->getStringFast(_PREHASH_Data, _PREHASH_FirstName, first, i);
        msg->getStringFast(_PREHASH_Data, _PREHASH_LastName, last, i);
        if (id.isNull())
            continue; // the "nobody found" line
        std::string legacy = first + " " + (last.empty() ? std::string("Resident") : last);
        search.avatars.append(llsd::map("id", id, "username", LLCacheName::buildUsername(legacy),
                                        "display_name", LLCacheName::buildFullName(first, last)));
    }
    search.done = true;
    search.last_at = lookup::now();
    return true;
}

void ibasho::onAvatarGroupsReply(LLMessageSystem* msg)
{
    LLUUID avatar_id;
    msg->getUUIDFast(_PREHASH_AgentData, _PREHASH_AvatarID, avatar_id);
    auto range = lookup::sProfiles.equal_range(avatar_id);
    for (auto it = range.first; it != range.second; ++it)
    {
        lookup::Profile& p = *it->second;
        S32 count = msg->getNumberOfBlocksFast(_PREHASH_GroupData);
        for (S32 i = 0; i < count; ++i)
        {
            LLUUID group_id;
            std::string name;
            msg->getUUIDFast(_PREHASH_GroupData, _PREHASH_GroupID, group_id, i);
            msg->getStringFast(_PREHASH_GroupData, _PREHASH_GroupName, name, i);
            lookup::addEntry(p.out["groups"], group_id, name);
        }
        p.groups = true;
        p.last_at = lookup::now();
    }
}

void ibasho::onAvatarPicksReply(LLMessageSystem* msg)
{
    LLUUID target_id;
    msg->getUUIDFast(_PREHASH_AgentData, _PREHASH_TargetID, target_id);
    auto range = lookup::sProfiles.equal_range(target_id);
    for (auto it = range.first; it != range.second; ++it)
    {
        lookup::Profile& p = *it->second;
        S32 count = msg->getNumberOfBlocksFast(_PREHASH_Data);
        for (S32 i = 0; i < count; ++i)
        {
            LLUUID pick_id;
            std::string name;
            msg->getUUIDFast(_PREHASH_Data, _PREHASH_PickID, pick_id, i);
            msg->getStringFast(_PREHASH_Data, _PREHASH_PickName, name, i);
            lookup::addEntry(p.out["picks"], pick_id, name);
        }
        p.picks = true;
        p.last_at = lookup::now();
    }
}

// findRegion's answers (see regions above). Only blocks of the map's own layer: a region-existence check (a
// request for one block with null regions included) also sends empty blocks, which would end a search early.
void ibasho::onMapBlock(bool map_layer, const std::string& name, U32 x_world, U32 y_world, U8 access)
{
    if (!map_layer || regions::sSearches.empty())
        return;
    // The block that ends a search names no region: an empty name, or (OpenSim) the searched-for name with the
    // access code of a region that doesn't exist (255, which LLWorldMap::insertRegion refuses too).
    const bool end = name.empty() || access == 255;
    for (const auto& s : regions::sSearches)
    {
        if (end)
            s->ended = true;
        else if (s->found.isDefined())
            continue;
        else if (LLStringUtil::compareInsensitive(name, s->want) == 0)
            s->found = llsd::map("name", name, "global", llsd::array((F64)x_world, (F64)y_world),
                                 "access", (S32)access, "maturity", LLViewerRegion::accessToString(access));
        else if (s->similar.size() < 20)
            s->similar.append(name);
    }
}

// findPlaces' answers (see places above): a DirPlacesReply to one of our searches, in as many parts as the grid sends.
bool ibasho::onDirPlacesReply(LLMessageSystem* msg, const LLUUID& query_id)
{
    auto found = places::sSearches.find(query_id);
    if (found == places::sSearches.end())
        return false;
    places::Search& s = *found->second;
    if (s.paged)
        return true; // late: the page has been made
    if (msg->getNumberOfBlocks("StatusData"))
    {
        U32 status = 0;
        msg->getU32("StatusData", "Status", status);
        s.status |= status;
    }
    S32 count = msg->getNumberOfBlocks("QueryReplies");
    for (S32 i = 0; i < count; ++i)
    {
        places::Hit hit;
        msg->getUUID("QueryReplies", "ParcelID", hit.id, i);
        msg->getString("QueryReplies", "Name", hit.name, i);
        msg->getBOOL("QueryReplies", "ForSale", hit.for_sale, i);
        msg->getF32("QueryReplies", "Dwell", hit.dwell, i);
        if (hit.id.notNull()) // the "nothing found" line has none
            s.hits.push_back(hit);
    }
    s.answered = true;
    s.last_at = lookup::now();
    return true;
}

// Spectator mode (2026-10-11, Claude Opus 5.5): the window layer drops real input (llwindowwin32.cpp); this keeps
// its flag in step with the setting, which LEAP can change (IbashoAgent spectator, LLViewerControl). The window's
// idea of where the cursor is (hover, tooltips) then follows LEAP's mouse events instead of the real pointer.
void ibasho::initSpectator()
{
#if LL_WINDOWS
    LLControlVariable* control = gSavedSettings.getControl("IbashoSpectator");
    if (!control)
        return;
    gIbashoSpectator = control->getValue().asBoolean();
    control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&)
    {
        bool on = value.asBoolean();
        if (on == gIbashoSpectator)
            return;
        gIbashoSpectator = on;
        // Wherever the real pointer last was is stale now: move the window's cursor off it, which ends any
        // hover or tooltip there. Not the real pointer (the flag is already set).
        if (on && gViewerWindow && gViewerWindow->getWindow())
            gViewerWindow->getWindow()->setCursorPosition(LLCoordWindow(-1, -1));
    });
#endif
}

// No activation (2026-10-23, Claude Opus 5.5): while IbashoNoActivate is on, the window layer never activates or
// raises the viewer's window (llwindowwin32.cpp); this keeps its flag in step with the setting.
void ibasho::initNoActivate()
{
#if LL_WINDOWS
    LLControlVariable* control = gSavedSettings.getControl("IbashoNoActivate");
    if (!control)
        return;
    gIbashoNoActivate = control->getValue().asBoolean();
    control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&)
    {
        gIbashoNoActivate = value.asBoolean();
    });
#endif
}

void ibasho::onInjectedMouse(const LLCoordGL& pos)
{
#if LL_WINDOWS
    if (gIbashoSpectator && gViewerWindow && gViewerWindow->getWindow())
        gViewerWindow->getWindow()->setCursorPosition(LLCoordWindow(pos.convert()));
#endif
}

// hideAvatars (2026-10-12, Claude Opus 5.5): avatar hiding in the window only; see ibashohide.h for where it hooks in.
bool ibasho::gHideActive = false;

namespace
{
namespace hide
{
enum class Mode { off, list, all_except };
Mode sMode = Mode::off;
std::unordered_set<LLUUID> sIds;    // list mode: hide these
std::unordered_set<LLUUID> sExcept; // all_except mode: hide everyone but me and these
bool sInSnapshots = false;          // hide in snapshots too (default: snapshots show everyone)
std::unordered_set<const LLDrawInfo*> sParticleInfos;

const char* modeName(Mode m)
{
    return m == Mode::list ? "list" : m == Mode::all_except ? "all_except" : "off";
}

bool hidden(const LLVOAvatar* av)
{
    if (!av || av->isSelf() || av->isUIAvatar() || av->isDead())
        return false;
    if (av->isControlAvatar())
    { // animesh goes with the avatar wearing it; animesh rezzed in-world stays
        const LLVOAvatar* wearer = av->getAttachedAvatar();
        return wearer && hidden(wearer);
    }
    const LLUUID& id = av->getID();
    if (id == gAgentID)
        return false;
    if (sMode == Mode::list)
        return sIds.count(id) > 0;
    return sMode == Mode::all_except && sExcept.count(id) == 0;
}

// The avatar itself, or the avatar a prim is attached to: an attachment's root has its avatar as parent. (An
// avatar sitting on an object is the object's child, not the other way round, so seats don't count.)
const LLVOAvatar* wearer(const LLViewerObject* obj)
{
    for (int depth = 0; obj && depth < 256; ++depth)
    {
        if (obj->isAvatar())
            return (const LLVOAvatar*)obj;
        obj = (const LLViewerObject*)obj->getParent();
    }
    return nullptr;
}

// Renders for the window (incl. its shadows, reflections, mirrors) skip hidden avatars; snapshots and the
// impostor renders (which make a texture, not the frame) draw them.
bool thisRender()
{
    return (sInSnapshots || !gSnapshot) && !LLPipeline::sImpostorRender;
}

std::unordered_set<LLUUID> idSet(const LLSD& list)
{
    std::unordered_set<LLUUID> out;
    for (const auto& v : llsd::inArray(list))
        if (v.asUUID().notNull())
            out.insert(v.asUUID());
    return out;
}

LLSD idArray(const std::unordered_set<LLUUID>& ids)
{
    LLSD out = LLSD::emptyArray();
    for (const auto& id : ids)
        out.append(id);
    return out;
}

LLSD describe()
{
    LLSD out = llsd::map("mode", modeName(sMode), "ids", idArray(sIds), "except", idArray(sExcept),
                         "in_snapshots", sInSnapshots);
    LLSD hidden_now = LLSD::emptyArray(), shown_now = LLSD::emptyArray();
    for (LLCharacter* c : LLCharacter::sInstances)
    {
        LLVOAvatar* av = (LLVOAvatar*)c;
        if (av->isDead() || av->isSelf() || av->isUIAvatar() || av->isControlAvatar())
            continue;
        (ibasho::gHideActive && hidden(av) ? hidden_now : shown_now).append(av->getID());
    }
    out["hidden_now"] = hidden_now; // avatars the viewer knows of, other than me
    out["shown_now"] = shown_now;
    return out;
}
} // namespace hide
} // namespace

bool ibasho::hideAvatarSlow(const LLVOAvatar* av)
{
    return hide::thisRender() && hide::hidden(av);
}

bool ibasho::hideObjectSlow(LLViewerObject* obj)
{
    return obj && hide::thisRender() && hide::hidden(hide::wearer(obj));
}

bool ibasho::hideGroupSlow(LLSpatialGroup* group)
{
    if (!group || !hide::thisRender())
        return false;
    LLSpatialPartition* part = group->getSpatialPartition();
    LLSpatialBridge* bridge = part ? part->asBridge() : nullptr;
    if (!bridge || !bridge->mDrawable)
        return false;
    LLViewerObject* root = bridge->mDrawable->getVObj().get();
    return root && root->isAttachment() && hide::hidden(hide::wearer(root));
}

bool ibasho::hideParticleSlow(LLViewerObject* part_group, S32 idx)
{
    LLVOPartGroup* vo = dynamic_cast<LLVOPartGroup*>(part_group);
    LLViewerPartGroup* group = vo ? vo->getViewerPartGroup() : nullptr;
    if (!group || idx < 0 || idx >= group->getCount())
        return false;
    LLViewerPart* part = group->mParticles[idx];
    if (!part || part->mPartSourcep.isNull())
        return false;
    return hide::hidden(hide::wearer(part->mPartSourcep->mSourceObjectp.get()));
}

bool ibasho::isHiddenDrawInfoSlow(const LLDrawInfo* info)
{
    return !hide::sParticleInfos.empty() && hide::sParticleInfos.count(info) > 0;
}

bool ibasho::hideDrawInfoSlow(const LLDrawInfo* info)
{
    return isHiddenDrawInfoSlow(info) && hide::thisRender();
}

void ibasho::markHiddenDrawInfo(const LLDrawInfo* info)
{
    hide::sParticleInfos.insert(info);
}

void ibasho::forgetDrawInfoSlow(const LLDrawInfo* info)
{
    if (!hide::sParticleInfos.empty())
        hide::sParticleInfos.erase(info);
}

void ibasho::hideAvatars(const LLSD& request)
{
    const hide::Mode old_mode = hide::sMode;
    const auto old_ids = hide::sIds;
    const auto old_except = hide::sExcept;
    if (request.has("mode"))
    {
        std::string mode = request["mode"].asString();
        if (mode == "off")
            hide::sMode = hide::Mode::off;
        else if (mode == "list")
            hide::sMode = hide::Mode::list;
        else if (mode == "all_except")
            hide::sMode = hide::Mode::all_except;
        else
        {
            sendReply(llsd::map("error", "mode must be off, list or all_except"), request);
            return;
        }
    }
    if (request.has("ids"))
        hide::sIds = hide::idSet(request["ids"]);
    if (request.has("except"))
        hide::sExcept = hide::idSet(request["except"]);
    if (request.has("in_snapshots"))
        hide::sInSnapshots = request["in_snapshots"].asBoolean();
    gHideActive = hide::sMode != hide::Mode::off;
    if (!gHideActive)
        hide::sParticleInfos.clear(); // unmarked infos get no forget calls while off

    // Whether anyone may have become hidden: a frame from before this call may still be on its way to a
    // capture, so a caller about to show the window waits a moment.
    bool grew = false;
    if (hide::sMode == hide::Mode::list)
    {
        grew = old_mode != hide::Mode::list;
        for (const auto& id : hide::sIds)
            grew = grew || !old_ids.count(id);
    }
    else if (hide::sMode == hide::Mode::all_except)
    {
        grew = old_mode != hide::Mode::all_except;
        for (const auto& id : old_except)
            grew = grew || !hide::sExcept.count(id);
    }
    LLSD reply = hide::describe();
    reply["grew"] = grew;
    sendReply(reply, request);
}

void ibasho::getHidden(const LLSD& request)
{
    sendReply(hide::describe(), request);
}
