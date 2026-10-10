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
#include "llagentbenefits.h"
#include "llagentui.h"
#include "llagentwearables.h"
#include "llappearancemgr.h"
#include "llavatarappearancedefines.h"
#include "llavatarnamecache.h"
#include "llcallingcard.h"
#include "lldbstrings.h"
#include "lleventapi.h"
#include "llfilesystem.h"
#include "llfloaterperms.h"
#include "llgiveinventory.h"
#include "llgroupactions.h"
#include "llimagej2c.h"
#include "llinventorymodel.h"
#include "lllocaltextureobject.h"
#include "llnotifications.h"
#include "llpermissionsflags.h"
#include "llviewerassetupload.h"
#include "llviewermenufile.h"
#include "llviewertexturelist.h"
#include "llviewerwearable.h"
#include "llwearabletype.h"
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
#include "pipeline.h"
#include "message.h"

void send_lures(const LLSD& notification, const LLSD& response); // llviewermessage.cpp
void handle_rebake_textures();                                  // llviewermenu.cpp

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
        add("upload",
            "Upload file [\"path\"] (png/tga/bmp/jpg texture, wav sound, anim animation) as a new inventory\n"
            "item, paying the normal L$ upload fee. Optional [\"name\"] (default: file name), [\"desc\"],\n"
            "[\"folder_id\"], [\"type\"] (texture|sound|animation, checked against the file), [\"next_perms\"]\n"
            "(e.g. \"MCT\"; default: the Uploads permission preferences), [\"timeout\"] (s, default 120).\n"
            "Textures: [\"max_size\"] (default 1024, the bake resolution; 2048 may cost more), [\"lossless\"].\n"
            "Refused if the fee is over [\"max_cost\"], IbashoMaxUploadCost, or the pay caps. [\"quote\"]: just\n"
            "reply the fee. Replies {item_id, asset_id, cost, charged, width?, height?} once the item exists.",
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
        add("uploadMesh",
            "Upload a model (.dae, .gltf, .glb) through the model upload floater, as picking the file for\n"
            "the high LOD does: lower LODs and physics as the floater generates them by default, then\n"
            "\"Calculate weights & fee\", then \"Upload\". Optional [\"name\"] (default: from the file),\n"
            "[\"scale\"] (import scale), [\"textures\"] (also upload its textures; costs more), [\"folder_id\"]\n"
            "(default Objects), [\"timeout\"] (s, default 180), [\"keep_open\"] (leave the floater open).\n"
            "Refused if the fee is over [\"max_cost\"], IbashoMaxUploadCost, or the pay caps. [\"quote\"]: stop\n"
            "after calculating. Replies {name, cost, weights {land_impact, streaming, physics, server,\n"
            "price_breakdown}, refusal?} for a quote, else adds {item_id, asset_id, charged} once the item\n"
            "exists; errors carry the floater's [\"log\"].",
            [](const LLSD& data) { ibasho::uploadMesh(data); },
            llsd::map("path", LLSD()));
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
        else if (data.has("max_cost") && cost > data["max_cost"].asInteger())
            refusal = llformat("the fee is L$%d, over max_cost L$%d", cost, data["max_cost"].asInteger());
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
