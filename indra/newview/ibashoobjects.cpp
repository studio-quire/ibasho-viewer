/**
 * @file   ibashoobjects.cpp
 * @brief  IbashoObjects (perceive in-world objects) and IbashoBuild (create/modify/link/take) LEAP APIs.
 *
 * Names, descriptions, owners and permissions aren't in object updates; the viewer has to ask
 * the region (RequestObjectPropertiesFamily, or ObjectProperties for selected objects). We
 * cache every reply, throttle our own requests, and answer LEAP requests once the replies are
 * in (or a timeout passes).
 *
 * Building works on individual objects through the same messages the build tools send
 * (ObjectAdd, MultipleObjectUpdate, ObjectShape, ObjectImage, ObjectLink, DeRezObject...),
 * without going through the selection.
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
#include "lleventapi.h"
#include "llinventorymodel.h"
#include "material_codes.h"
#include "message.h"
#include "object_flags.h"
#include "llpermissionsflags.h"
#include "llsaleinfo.h"
#include "llsdutil.h"
#include "llsdutil_math.h"
#include "llselectmgr.h"
#include "lltooldraganddrop.h" // pack_permissions_slam
#include "llviewerinventory.h"
#include "llviewerjointattachment.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "llvolume.h"
#include "llvolumemessage.h"

namespace
{
using ibasho::findObject;
using ibasho::idList;

// ---------------------------------------------------------------- properties cache

std::map<LLUUID, LLSD> sProps;          // object id -> properties (see readCommon)
std::deque<LLUUID> sFetchQueue;         // ids waiting for a RequestObjectPropertiesFamily
std::set<LLUUID> sFetchQueued;
std::map<LLUUID, F64> sFetchSent;       // id -> when we last asked
std::set<LLUUID> sEmitProps;            // post an object_properties event when these arrive
const F64 PROPS_FRESH = 300.0;          // re-ask after this many seconds
const F64 FETCH_RETRY = 5.0;            // don't re-send a request sooner than this
const S32 FETCH_PER_SECOND = 20;
F64 sFetchBudget = 0.0;
F64 sFetchLastTick = 0.0;

void drainFetchQueue();
void processCreated();
void ensureIdle()
{
    static bool started = false;
    if (!started)
    {
        started = true;
        ibasho::onIdle([]() { drainFetchQueue(); processCreated(); });
    }
}

LLSD perms(U32 base, U32 owner, U32 group, U32 everyone, U32 next)
{
    using ibasho::permString;
    return llsd::map("base", permString(base), "owner", permString(owner), "group", permString(group),
                     "everyone", permString(everyone), "next", permString(next));
}

const char* const SALE_TYPES[] = { "not", "original", "copy", "contents" }; // EForSale order

LLSaleInfo::EForSale saleTypeFromName(const std::string& name)
{
    for (S32 i = 0; i < 4; ++i)
        if (name == SALE_TYPES[i])
            return (LLSaleInfo::EForSale)i;
    return LLSaleInfo::lookup(name.c_str()); // LL's short names: orig, cntn
}

LLSD saleSD(const LLSaleInfo& sale)
{
    S32 type = sale.getSaleType();
    if (type <= LLSaleInfo::FS_NOT || type > LLSaleInfo::FS_CONTENTS)
        return LLSD();
    return llsd::map("type", SALE_TYPES[type], "price", sale.getSalePrice());
}

bool propsFresh(const LLUUID& id, bool need_full = false)
{
    auto it = sProps.find(id);
    if (it == sProps.end())
        return false;
    if (need_full && !it->second["full"].asBoolean())
        return false;
    return (F64)LLTimer::getTotalSeconds() - it->second["fetched"].asReal() < PROPS_FRESH;
}

void storeProps(const LLUUID& id, LLSD props)
{
    if (sProps.size() > 20000)
        sProps.clear();
    LLSD& entry = sProps[id];
    bool was_full = entry["full"].asBoolean();
    for (const auto& [k, v] : llsd::inMap(props))
        entry[k] = v;
    if (was_full && !props["full"].asBoolean())
    {
        entry["full"] = true; // family replies don't carry creator etc.; keep what selection gave us
    }
    entry["fetched"] = (F64)LLTimer::getTotalSeconds();
    if (sEmitProps.erase(id))
    {
        LLSD ev = entry;
        ev["id"] = id;
        ev.erase("fetched");
        ibasho::postEvent("object_properties", ev);
    }
}

void queueFetch(const LLUUID& id, bool force = false)
{
    if (!force && propsFresh(id))
        return;
    auto sent = sFetchSent.find(id);
    if (sent != sFetchSent.end() && (F64)LLTimer::getTotalSeconds() - sent->second < FETCH_RETRY)
        return;
    if (sFetchQueued.insert(id).second)
        sFetchQueue.push_back(id);
    ensureIdle();
}

void drainFetchQueue()
{
    F64 now = (F64)LLTimer::getTotalSeconds();
    sFetchBudget = llmin((F64)FETCH_PER_SECOND, sFetchBudget + (now - sFetchLastTick) * FETCH_PER_SECOND);
    sFetchLastTick = now;
    while (!sFetchQueue.empty() && sFetchBudget >= 1.0)
    {
        LLUUID id = sFetchQueue.front();
        sFetchQueue.pop_front();
        sFetchQueued.erase(id);
        LLViewerObject* obj = gObjectList.findObject(id);
        if (!obj || obj->isDead() || !obj->getRegion())
            continue;
        LLSelectMgr::getInstance()->requestObjectPropertiesFamily(obj);
        sFetchSent[id] = now;
        sFetchBudget -= 1.0;
    }
    if (sFetchSent.size() > 5000)
        sFetchSent.clear();
}

// ---------------------------------------------------------------- describing objects

const char* clickActionName(U8 action)
{
    switch (action)
    {
    case CLICK_ACTION_SIT:        return "sit";
    case CLICK_ACTION_BUY:        return "buy";
    case CLICK_ACTION_PAY:        return "pay";
    case CLICK_ACTION_OPEN:       return "open";
    case CLICK_ACTION_PLAY:       return "play";
    case CLICK_ACTION_OPEN_MEDIA: return "open_media";
    case CLICK_ACTION_ZOOM:       return "zoom";
    case CLICK_ACTION_DISABLED:   return "disabled";
    default:                      return "touch";
    }
}

std::string attachPointName(S32 id)
{
    if (isAgentAvatarValid())
    {
        auto it = gAgentAvatarp->mAttachmentPoints.find(id);
        if (it != gAgentAvatarp->mAttachmentPoints.end() && it->second)
            return it->second->getName();
    }
    return llformat("%d", id);
}

S32 countPrims(const LLViewerObject* root, LLSD* seated)
{
    S32 n = 1;
    for (const auto& child : root->getChildren())
    {
        if (child->isAvatar())
        {
            if (seated)
                seated->append(child->getID());
        }
        else
        {
            n += countPrims(child, seated);
        }
    }
    return n;
}

const char* shapeName(const LLVolumeParams& params)
{
    if (params.isMeshSculpt())
        return "mesh";
    if (params.isSculpt())
        return "sculpt";
    U8 profile = params.getProfileParams().getCurveType() & LL_PCODE_PROFILE_MASK;
    bool line = (params.getPathParams().getCurveType() & 0xf0) == LL_PCODE_PATH_LINE;
    switch (profile)
    {
    case LL_PCODE_PROFILE_SQUARE:      return line ? "box" : "tube";
    case LL_PCODE_PROFILE_CIRCLE:      return line ? "cylinder" : "torus";
    case LL_PCODE_PROFILE_EQUALTRI:    return line ? "prism" : "ring";
    case LL_PCODE_PROFILE_CIRCLE_HALF: return line ? "half-circle" : "sphere";
    default:                           return "other";
    }
}

LLSD describe(LLViewerObject* o, bool children = false)
{
    LLSD d;
    d["id"] = o->getID();
    d["local_id"] = (LLSD::Integer)o->getLocalID();
    LLViewerObject* root = o->getRootEdit();
    if (root != o)
        d["root_id"] = root->getID();
    switch (o->getPCode())
    {
    case LL_PCODE_VOLUME: break;
    case LL_PCODE_LEGACY_TREE:
    case LL_PCODE_TREE_NEW: d["kind"] = "tree"; break;
    case LL_PCODE_LEGACY_GRASS: d["kind"] = "grass"; break;
    default: d["kind"] = (LLSD::Integer)o->getPCode(); break;
    }
    d["pos"] = ll_sd_from_vector3(o->getPositionRegion());
    d["rot"] = ll_sd_from_quaternion(o->getRotationRegion());
    d["scale"] = ll_sd_from_vector3(o->getScale());
    if (o->getPCode() == LL_PCODE_VOLUME && o->getVolume())
        d["shape"] = shapeName(o->getVolume()->getParams());
    d["dist"] = (LLSD::Real)dist_vec(o->getPositionGlobal(), gAgent.getPositionGlobal());
    if (root == o)
    {
        LLSD seated = LLSD::emptyArray();
        d["prims"] = countPrims(o, &seated);
        if (seated.size())
            d["seated"] = seated;
    }
    if (!o->mHudText.empty())
    {
        d["text"] = o->mHudText;
        d["text_color"] = ll_sd_from_color4(o->mHudTextColor);
    }
    LLSD flags = LLSD::emptyArray();
    if (o->flagHandleTouch())    flags.append("touch");
    if (o->flagTakesMoney())     flags.append("pay");
    if (o->flagScripted())       flags.append("scripted");
    if (o->flagObjectYouOwner()) flags.append("mine");
    if (o->flagObjectModify())   flags.append("mod");
    if (o->flagObjectCopy())     flags.append("copy");
    if (o->flagObjectMove())     flags.append("move");
    if (o->flagPhantom())        flags.append("phantom");
    if (o->flagUsePhysics())     flags.append("physics");
    if (o->flagTemporaryOnRez()) flags.append("temp");
    if (o->flagAllowInventoryAdd()) flags.append("drop");
    d["flags"] = flags;
    if (o->getClickAction() != CLICK_ACTION_TOUCH)
        d["click_action"] = clickActionName(o->getClickAction());
    if (o->isAttachment() || root->isAttachment())
    {
        LLVOAvatar* av = o->getAvatar();
        if (av)
            d["attached_to"] = av->getID();
        d["attach_point"] = attachPointName(ATTACHMENT_ID_FROM_STATE(root->getAttachmentState()));
        if (o->isHUDAttachment())
            d["hud"] = true;
    }
    auto it = sProps.find(o->getID());
    if (it != sProps.end())
    {
        for (const auto& [k, v] : llsd::inMap(it->second))
        {
            if (k != "fetched")
                d[k] = v;
        }
    }
    else if (o->mOwnerID.notNull())
    {
        d["owner_id"] = o->mOwnerID;
    }
    if (children && root == o)
    {
        d["children"] = LLSD::emptyArray();
        for (const auto& child : o->getChildren())
        {
            if (!child->isAvatar())
                d["children"].append(describe(child));
        }
    }
    return d;
}

bool listable(LLViewerObject* o, bool attachments)
{
    if (!o || o->isDead() || o->isAvatar() || !o->getRegion())
        return false;
    if (o->getRootEdit() != o)
        return false;
    if (o->isAttachment())
        return attachments && !o->isHUDAttachment();
    return o->getPCode() == LL_PCODE_VOLUME || o->getPCode() == LL_PCODE_LEGACY_TREE
        || o->getPCode() == LL_PCODE_TREE_NEW || o->getPCode() == LL_PCODE_LEGACY_GRASS;
}

bool matches(const LLSD& d, const std::string& query)
{
    if (query.empty())
        return true;
    for (const char* key : { "name", "desc", "text" })
    {
        std::string v = d[key].asString();
        LLStringUtil::toLower(v);
        if (v.find(query) != std::string::npos)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------- IbashoObjects

class IbashoObjectsListener : public LLEventAPI
{
public:
    IbashoObjectsListener()
        : LLEventAPI("IbashoObjects",
                     "Perceive in-world objects: names, descriptions, hover text, owners, permissions.\n"
                     "Properties come from the region on demand (throttled) and are cached.")
    {
        add("nearby",
            "Reply [\"objects\"]: root objects within [\"dist\"] m (default 20), nearest first, at most\n"
            "[\"limit\"] (default 40). Each: id, local_id, pos, rot, scale, dist, prims, text, text_color,\n"
            "flags (touch pay scripted mine mod copy move phantom physics temp drop), click_action,\n"
            "and when known name, desc, owner_id, group_id, last_owner_id, perms, sale.\n"
            "[\"fetch\"] (default true) asks the region for missing properties and waits up to\n"
            "[\"timeout\"] s (default 4). [\"attachments\"]: include other avatars' attachments.\n"
            "[\"query\"]: keep only objects whose name/desc/hover text contains it (case-insensitive).",
            &IbashoObjectsListener::nearby,
            llsd::map("reply", LLSD()));
        add("get",
            "Reply [\"objects\"] for [\"ids\"] (any prim, not just roots), like nearby. Missing ids are\n"
            "listed in [\"missing\"]. [\"children\"]: include child prims. [\"full\"]: briefly select the\n"
            "objects to also learn creator_id, touch_name, sit_name, created (takes longer).",
            &IbashoObjectsListener::get,
            llsd::map("reply", LLSD()));
        add("find",
            "Reply [\"ids\"]: ids of objects in view (any prim, attachments included) starting with [\"prefix\"].",
            &IbashoObjectsListener::find,
            llsd::map("prefix", LLSD(), "reply", LLSD()));
        add("request",
            "Ask the region for properties of [\"ids\"]; each answer is posted to IbashoEvents as\n"
            "{event:\"object_properties\", id, name, desc, owner_id, ...}.",
            &IbashoObjectsListener::request);
        add("buy",
            "Buy object [\"id\"] that's for sale, at [\"price\"] (must match its current price; fetch it first\n"
            "with get). Spending caps as for IbashoAgent pay. Copies/originals go to Objects, contents to a new\n"
            "folder. The region answers with an inventory offer or an alert.",
            &IbashoObjectsListener::buy,
            llsd::map("id", LLSD(), "price", LLSD()));
        add("select",
            "Select [\"ids\"] (whole linksets) as the edit tool would; [\"add\"] keeps the current selection.",
            &IbashoObjectsListener::select);
        add("deselect",
            "Deselect [\"ids\"], or everything if none given.",
            &IbashoObjectsListener::deselect);
        add("selection",
            "Reply [\"objects\"]: the currently selected root objects.",
            &IbashoObjectsListener::selection,
            llsd::map("reply", LLSD()));
    }

private:
    static LLSD collect(const std::vector<LLUUID>& ids, bool children, const std::string& query, LLSD* missing)
    {
        LLSD out = LLSD::emptyArray();
        for (const LLUUID& id : ids)
        {
            LLViewerObject* o = gObjectList.findObject(id);
            if (!o || o->isDead())
            {
                if (missing)
                    missing->append(id);
                continue;
            }
            LLSD d = describe(o, children);
            if (matches(d, query))
                out.append(d);
        }
        return out;
    }

    void nearby(const LLSD& data)
    {
        F32 radius = data.has("dist") ? (F32)data["dist"].asReal() : 20.f;
        S32 limit = data.has("limit") ? data["limit"].asInteger() : 40;
        bool attachments = data["attachments"].asBoolean();
        bool fetch = !data.has("fetch") || data["fetch"].asBoolean();
        std::string query = data["query"].asString();
        LLStringUtil::toLower(query);
        // With a query, consider more candidates; names are only known after fetching.
        S32 candidates = query.empty() ? limit : llmax(limit, 200);

        LLVector3d me = gAgent.getPositionGlobal();
        std::vector<std::pair<F64, LLUUID>> found;
        S32 n = gObjectList.getNumObjects();
        for (S32 i = 0; i < n; ++i)
        {
            LLViewerObject* o = gObjectList.getObject(i);
            if (!listable(o, attachments) || (o->isAttachment() && o->getAvatar() == gAgentAvatarp.get()))
                continue;
            F64 d = dist_vec(o->getPositionGlobal(), me);
            if (d <= radius)
                found.emplace_back(d, o->getID());
        }
        std::sort(found.begin(), found.end());
        if ((S32)found.size() > candidates)
            found.resize(candidates);
        std::vector<LLUUID> ids;
        for (const auto& f : found)
            ids.push_back(f.second);
        answer(data, ids, fetch, false, false, query, limit);
    }

    void get(const LLSD& data)
    {
        std::vector<LLUUID> ids = idList(data);
        bool fetch = !data.has("fetch") || data["fetch"].asBoolean();
        answer(data, ids, fetch, data["children"].asBoolean(), data["full"].asBoolean(), "", 0);
    }

    // Reply once properties for ids (and children if asked) are in, or on timeout.
    void answer(const LLSD& data, const std::vector<LLUUID>& ids, bool fetch, bool children, bool full,
                const std::string& query, S32 limit)
    {
        F64 timeout = data.has("timeout") ? data["timeout"].asReal() : (full ? 6.0 : 4.0);
        std::vector<LLUUID> wanted;
        std::vector<LLUUID> selected_by_us;
        if (fetch)
        {
            for (const LLUUID& id : ids)
            {
                LLViewerObject* o = gObjectList.findObject(id);
                if (!o)
                    continue;
                wanted.push_back(id);
                if (children)
                {
                    for (const auto& c : o->getChildren())
                        if (!c->isAvatar())
                            wanted.push_back(c->getID());
                }
            }
            if (full)
            {
                for (const LLUUID& id : wanted)
                {
                    LLViewerObject* o = gObjectList.findObject(id);
                    if (o && !propsFresh(id, true) && !o->isSelected())
                    {
                        LLSelectMgr::getInstance()->selectObjectAndFamily(o->getRootEdit(), true);
                        selected_by_us.push_back(o->getRootEdit()->getID());
                    }
                }
            }
            else
            {
                for (const LLUUID& id : wanted)
                    queueFetch(id);
            }
        }
        ibasho::deferReply(data, timeout,
            [ids, wanted, selected_by_us, children, full, query, limit](LLSD& reply, bool expired)
            {
                if (!expired)
                {
                    for (const LLUUID& id : wanted)
                    {
                        if (!propsFresh(id, full) && gObjectList.findObject(id))
                            return false;
                    }
                }
                for (const LLUUID& id : selected_by_us)
                {
                    if (LLViewerObject* o = gObjectList.findObject(id))
                        LLSelectMgr::getInstance()->deselectObjectAndFamily(o);
                }
                LLSD missing = LLSD::emptyArray();
                LLSD objects = collect(ids, children, query, &missing);
                if (limit > 0 && objects.size() > limit)
                {
                    LLSD trimmed = LLSD::emptyArray();
                    for (S32 i = 0; i < limit; ++i)
                        trimmed.append(objects[i]);
                    objects = trimmed;
                }
                reply["objects"] = objects;
                if (missing.size())
                    reply["missing"] = missing;
                if (expired)
                    reply["timed_out"] = true;
                return true;
            });
    }

    void find(const LLSD& data)
    {
        Response response(LLSD(), data);
        std::string prefix = data["prefix"].asString();
        LLStringUtil::toLower(prefix);
        response["ids"] = LLSD::emptyArray();
        S32 n = gObjectList.getNumObjects();
        for (S32 i = 0; i < n && response["ids"].size() < 20; ++i)
        {
            LLViewerObject* o = gObjectList.getObject(i);
            if (o && !o->isDead() && !o->isAvatar() && o->getID().asString().compare(0, prefix.size(), prefix) == 0)
                response["ids"].append(o->getID());
        }
    }

    void request(const LLSD& data)
    {
        Response response(LLSD(), data);
        for (const LLUUID& id : idList(data))
        {
            sEmitProps.insert(id);
            queueFetch(id, true);
        }
    }

    void buy(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLViewerObject* o = findObject(data["id"]);
        if (!o || !o->getRegion())
            return response.error("object not in view");
        o = o->getRootEdit();
        auto it = sProps.find(o->getID());
        if (it == sProps.end())
            return response.error("properties unknown; get the object first");
        const LLSD& sale = it->second["sale"];
        if (!sale.isDefined())
            return response.error("not for sale");
        S32 price = sale["price"].asInteger();
        if (!data["price"].isInteger() && !data["price"].isReal())
            return response.error("give the expected price");
        if (data["price"].asInteger() != price)
            return response.error(llformat("price is L$%d, not L$%d", price, data["price"].asInteger()));
        std::string refusal = ibasho::spendRefusal(price);
        if (!refusal.empty())
            return response.error(refusal);
        LLSaleInfo::EForSale type = saleTypeFromName(sale["type"].asString());
        LLUUID category = gInventory.findCategoryUUIDForType(type == LLSaleInfo::FS_CONTENTS ? LLFolderType::FT_ROOT_INVENTORY
                                                                                            : LLFolderType::FT_OBJECT);
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ObjectBuy);
        msg->nextBlockFast(_PREHASH_AgentData);
        msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
        msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
        msg->addUUIDFast(_PREHASH_GroupID, gAgent.getGroupID());
        msg->addUUIDFast(_PREHASH_CategoryID, category);
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, o->getLocalID());
        msg->addU8Fast(_PREHASH_SaleType, (U8)type);
        msg->addS32Fast(_PREHASH_SalePrice, price);
        msg->sendReliable(o->getRegion()->getHost());
        ibasho::recordSpend(price);
        response["ok"] = true;
    }

    // LLSelectMgr drops selections nobody holds a handle to, so keep one while selected.
    static LLObjectSelectionHandle sHeld;

    void select(const LLSD& data)
    {
        Response response(LLSD(), data);
        if (!data["add"].asBoolean())
            LLSelectMgr::getInstance()->deselectAll();
        sHeld = LLSelectMgr::getInstance()->getSelection();
        S32 n = 0;
        for (const LLUUID& id : idList(data))
        {
            if (LLViewerObject* o = gObjectList.findObject(id))
            {
                LLSelectMgr::getInstance()->selectObjectAndFamily(o, true);
                ++n;
            }
        }
        response["selected"] = n;
    }

    void deselect(const LLSD& data)
    {
        Response response(LLSD(), data);
        std::vector<LLUUID> ids = idList(data);
        if (ids.empty())
        {
            LLSelectMgr::getInstance()->deselectAll();
            sHeld = NULL;
            return;
        }
        for (const LLUUID& id : ids)
        {
            if (LLViewerObject* o = gObjectList.findObject(id))
                LLSelectMgr::getInstance()->deselectObjectAndFamily(o);
        }
    }

    void selection(const LLSD& data)
    {
        Response response(LLSD(), data);
        response["objects"] = LLSD::emptyArray();
        LLObjectSelectionHandle sel = LLSelectMgr::getInstance()->getSelection();
        for (LLObjectSelection::root_iterator it = sel->root_begin(); it != sel->root_end(); ++it)
        {
            LLViewerObject* o = (*it)->getObject();
            if (o && !o->isDead())
                response["objects"].append(describe(o));
        }
    }
};

LLObjectSelectionHandle IbashoObjectsListener::sHeld;
IbashoObjectsListener sIbashoObjectsListener;

// ---------------------------------------------------------------- building helpers

void packAgent(LLMessageSystem* msg, bool with_group = false)
{
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    if (with_group)
        msg->addUUIDFast(_PREHASH_GroupID, gAgent.getGroupID());
}

struct ShapeType { const char* name; U8 profile; U8 path; };
const ShapeType SHAPES[] = {
    { "box",      LL_PCODE_PROFILE_SQUARE,      LL_PCODE_PATH_LINE },
    { "cylinder", LL_PCODE_PROFILE_CIRCLE,      LL_PCODE_PATH_LINE },
    { "prism",    LL_PCODE_PROFILE_EQUALTRI,    LL_PCODE_PATH_LINE },
    { "sphere",   LL_PCODE_PROFILE_CIRCLE_HALF, LL_PCODE_PATH_CIRCLE },
    { "torus",    LL_PCODE_PROFILE_CIRCLE,      LL_PCODE_PATH_CIRCLE },
    { "tube",     LL_PCODE_PROFILE_SQUARE,      LL_PCODE_PATH_CIRCLE },
    { "ring",     LL_PCODE_PROFILE_EQUALTRI,    LL_PCODE_PATH_CIRCLE },
    { "cone",     LL_PCODE_PROFILE_CIRCLE,      LL_PCODE_PATH_LINE },
    { "pyramid",  LL_PCODE_PROFILE_SQUARE,      LL_PCODE_PATH_LINE },
};

const ShapeType* findShape(const std::string& name)
{
    for (const auto& s : SHAPES)
        if (name == s.name)
            return &s;
    return nullptr;
}

// Fresh params for a basic shape, as the build tool makes them.
void resetShape(LLVolumeParams& p, const ShapeType& s)
{
    p.setType(s.profile, s.path);
    p.setBeginAndEndS(0.f, 1.f);
    p.setBeginAndEndT(0.f, 1.f);
    p.setHollow(0.f);
    p.setTwistBegin(0.f);
    p.setTwistEnd(0.f);
    p.setShear(0.f, 0.f);
    p.setTaper(0.f, 0.f);
    p.setRevolutions(1.f);
    p.setRadiusOffset(0.f);
    p.setSkew(0.f);
    bool circular = s.path == LL_PCODE_PATH_CIRCLE;
    if (circular && s.profile != LL_PCODE_PROFILE_CIRCLE_HALF)
        p.setRatio(1.f, 0.25f); // hole size
    else
        p.setRatio(1.f, 1.f);
    if (!strcmp(s.name, "cone") || !strcmp(s.name, "pyramid"))
        p.setRatio(0.f, 0.f);
}

LLVector2 vec2(const LLSD& v)
{
    if (v.isArray())
        return LLVector2((F32)v[0].asReal(), (F32)v[1].asReal());
    return LLVector2((F32)v.asReal(), (F32)v.asReal());
}

// Apply shape keys from p to params; returns whether anything changed. Error text in err.
bool applyShape(LLVolumeParams& params, const LLSD& p, std::string& err)
{
    bool changed = false;
    if (p.has("shape"))
    {
        const ShapeType* s = findShape(p["shape"].asString());
        if (!s)
        {
            err = "unknown shape " + p["shape"].asString();
            return false;
        }
        resetShape(params, *s);
        changed = true;
    }
    bool circular = (params.getPathParams().getCurveType() & 0xf0) != LL_PCODE_PATH_LINE;
    if (p.has("profile_cut")) { LLVector2 v = vec2(p["profile_cut"]); params.setBeginAndEndS(v.mV[0], v.mV[1]); changed = true; }
    if (p.has("path_cut"))    { LLVector2 v = vec2(p["path_cut"]);    params.setBeginAndEndT(v.mV[0], v.mV[1]); changed = true; }
    if (p.has("hollow"))      { params.setHollow((F32)p["hollow"].asReal()); changed = true; }
    if (p.has("twist"))       { LLVector2 v = vec2(p["twist"]); params.setTwistBegin(v.mV[0]); params.setTwistEnd(v.mV[1]); changed = true; }
    if (p.has("top_shear"))   { LLVector2 v = vec2(p["top_shear"]); params.setShear(v.mV[0], v.mV[1]); changed = true; }
    if (p.has("hole_size"))   { LLVector2 v = vec2(p["hole_size"]); params.setRatio(v.mV[0], v.mV[1]); changed = true; }
    if (p.has("taper"))
    {
        // Edit-panel/LSL semantics: on straight paths taper scales the top (1 - taper);
        // on circular paths it's the path taper.
        LLVector2 v = vec2(p["taper"]);
        if (circular)
            params.setTaper(v.mV[0], v.mV[1]);
        else
            params.setRatio(1.f - v.mV[0], 1.f - v.mV[1]);
        changed = true;
    }
    if (p.has("radius_offset")) { params.setRadiusOffset((F32)p["radius_offset"].asReal()); changed = true; }
    if (p.has("revolutions"))   { params.setRevolutions((F32)p["revolutions"].asReal()); changed = true; }
    if (p.has("skew"))          { params.setSkew((F32)p["skew"].asReal()); changed = true; }
    return changed;
}

LLQuaternion rotFrom(const LLSD& p, const char* quat_key, const char* euler_key, bool* found)
{
    *found = true;
    if (p.has(quat_key))
        return ll_quaternion_from_sd(p[quat_key]);
    if (p.has(euler_key))
    {
        LLVector3 e = ll_vector3_from_sd(p[euler_key]) * DEG_TO_RAD;
        LLQuaternion q;
        q.setEulerAngles(e.mV[0], e.mV[1], e.mV[2]);
        return q;
    }
    *found = false;
    return LLQuaternion();
}

void sendMultipleUpdate(LLViewerObject* o, U8 type, const LLVector3& pos, const LLQuaternion& rot, const LLVector3& scale)
{
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_MultipleObjectUpdate);
    packAgent(msg);
    msg->nextBlockFast(_PREHASH_ObjectData);
    msg->addU32Fast(_PREHASH_ObjectLocalID, o->getLocalID());
    msg->addU8Fast(_PREHASH_Type, type);
    U8 data[256];
    S32 offset = 0;
    // Order matters: the region unpacks position, rotation, scale.
    if (type & UPD_POSITION)
    {
        htolememcpy(&data[offset], pos.mV, MVT_LLVector3, 12);
        offset += 12;
    }
    if (type & UPD_ROTATION)
    {
        LLVector3 packed = rot.packToVector3();
        htolememcpy(&data[offset], packed.mV, MVT_LLQuaternion, 12);
        offset += 12;
    }
    if (type & UPD_SCALE)
    {
        htolememcpy(&data[offset], scale.mV, MVT_LLVector3, 12);
        offset += 12;
    }
    msg->addBinaryDataFast(_PREHASH_Data, data, offset);
    msg->sendReliable(o->getRegion()->getHost());
}

void sendObjectString(LLViewerObject* o, const char* message, const char* field, const std::string& value)
{
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessage(message);
    packAgent(msg);
    msg->nextBlock("ObjectData");
    msg->addU32("LocalID", o->getLocalID());
    msg->addString(field, value);
    msg->sendReliable(o->getRegion()->getHost());
}

// Modify one prim. Returns an error string, or "" on success.
std::string modifyObject(LLViewerObject* o, const LLSD& p)
{
    if (!o || o->isDead() || !o->getRegion())
        return "object not found";
    LLViewerObject* root = o->getRootEdit();
    bool is_root = root == o;

    // Position / rotation. Region frame by default; local_* relative to the root prim.
    U8 type = 0;
    LLVector3 pos = o->getPosition();
    LLQuaternion rot = o->getRotation();
    if (p.has("pos"))
    {
        LLVector3 region_pos = ll_vector3_from_sd(p["pos"]);
        pos = is_root ? region_pos : (region_pos - root->getPositionRegion()) * ~root->getRotationRegion();
        type |= UPD_POSITION;
    }
    else if (p.has("local_pos") && !is_root)
    {
        pos = ll_vector3_from_sd(p["local_pos"]);
        type |= UPD_POSITION;
    }
    bool have_rot = false;
    LLQuaternion region_rot = rotFrom(p, "rot", "rot_euler", &have_rot);
    if (have_rot)
    {
        rot = is_root ? region_rot : region_rot * ~root->getRotationRegion();
        type |= UPD_ROTATION;
    }
    else
    {
        LLQuaternion local_rot = rotFrom(p, "local_rot", "local_rot_euler", &have_rot);
        if (have_rot && !is_root)
        {
            rot = local_rot;
            type |= UPD_ROTATION;
        }
    }
    if (type)
    {
        // Moving/rotating a root moves the whole linkset; children move alone.
        if (is_root)
            type |= UPD_LINKED_SETS;
        sendMultipleUpdate(o, type, pos, rot, LLVector3());
    }
    if (p.has("scale"))
        sendMultipleUpdate(o, UPD_SCALE, LLVector3(), LLQuaternion(), ll_vector3_from_sd(p["scale"]));

    // Shape
    if (o->getVolume())
    {
        LLVolumeParams params = o->getVolume()->getParams();
        std::string err;
        if (applyShape(params, p, err))
        {
            LLMessageSystem* msg = gMessageSystem;
            msg->newMessageFast(_PREHASH_ObjectShape);
            packAgent(msg);
            msg->nextBlockFast(_PREHASH_ObjectData);
            msg->addU32Fast(_PREHASH_ObjectLocalID, o->getLocalID());
            LLVolumeMessage::packVolumeParams(&params, msg);
            msg->sendReliable(o->getRegion()->getHost());
        }
        else if (!err.empty())
        {
            return err;
        }
    }

    // Faces
    bool te_changed = false;
    S32 face = p.has("face") ? p["face"].asInteger() : -1;
    S32 first = face < 0 ? 0 : face, last = face < 0 ? o->getNumTEs() - 1 : face;
    if (face >= o->getNumTEs())
        return "no such face";
    for (S32 te = first; te <= last; ++te)
    {
        const LLTextureEntry* entry = o->getTE(te);
        if (!entry)
            continue;
        if (p.has("color"))
        {
            const LLSD& c = p["color"];
            LLColor4 col((F32)c[0].asReal(), (F32)c[1].asReal(), (F32)c[2].asReal(),
                         c.size() > 3 ? (F32)c[3].asReal() : entry->getColor().mV[VALPHA]);
            o->setTEColor(te, col);
            te_changed = true;
        }
        if (p.has("alpha"))
        {
            LLColor4 col = o->getTE(te)->getColor();
            col.mV[VALPHA] = (F32)p["alpha"].asReal();
            o->setTEColor(te, col);
            te_changed = true;
        }
        if (p.has("texture"))
        {
            o->setTETexture(te, p["texture"].asUUID());
            te_changed = true;
        }
        if (p.has("repeats"))
        {
            LLVector2 v = vec2(p["repeats"]);
            o->setTEScale(te, v.mV[0], v.mV[1]);
            te_changed = true;
        }
        if (p.has("glow"))
        {
            o->setTEGlow(te, (F32)p["glow"].asReal());
            te_changed = true;
        }
        if (p.has("fullbright"))
        {
            o->setTEFullbright(te, p["fullbright"].asBoolean() ? 1 : 0);
            te_changed = true;
        }
        if (p.has("shiny"))
        {
            o->setTEShiny(te, (U8)llclamp(p["shiny"].asInteger(), 0, 3));
            te_changed = true;
        }
    }
    if (te_changed)
        o->sendTEUpdate();

    if (p.has("name"))
        sendObjectString(o, "ObjectName", "Name", p["name"].asString());
    if (p.has("desc"))
        sendObjectString(o, "ObjectDescription", "Description", p["desc"].asString());

    bool flags_changed = false;
    const std::pair<const char*, U32> flag_keys[] = {
        { "phantom", FLAGS_PHANTOM }, { "physics", FLAGS_USE_PHYSICS }, { "temp", FLAGS_TEMPORARY_ON_REZ } };
    for (const auto& [key, flag] : flag_keys)
    {
        if (p.has(key))
        {
            o->setFlagsWithoutUpdate(flag, p[key].asBoolean());
            flags_changed = true;
        }
    }
    if (flags_changed)
        o->updateFlags();

    if (p.has("sale"))
    {
        // {"type": "original"|"copy"|"contents", "price": N}, or false/"not" to stop selling
        const LLSD& sale = p["sale"];
        LLSaleInfo::EForSale type = sale.isMap() ? saleTypeFromName(sale["type"].asString()) : LLSaleInfo::FS_NOT;
        if (sale.isMap() && type == LLSaleInfo::FS_NOT && sale["type"].asString() != "not")
            return "unknown sale type " + sale["type"].asString();
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ObjectSaleInfo);
        packAgent(msg);
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_LocalID, o->getLocalID());
        msg->addU8Fast(_PREHASH_SaleType, (U8)type);
        msg->addS32Fast(_PREHASH_SalePrice, sale.isMap() ? sale["price"].asInteger() : 0);
        msg->sendReliable(o->getRegion()->getHost());
    }

    if (p.has("next_perms"))
    {
        // e.g. "MC" = next owner may modify and copy but not transfer
        std::string want = p["next_perms"].asString();
        U32 mask = 0;
        for (char c : want)
            mask |= c == 'M' ? PERM_MODIFY : c == 'C' ? PERM_COPY : c == 'T' ? PERM_TRANSFER : 0;
        const U32 all = PERM_MODIFY | PERM_COPY | PERM_TRANSFER;
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ObjectPermissions);
        packAgent(msg);
        msg->nextBlockFast(_PREHASH_HeaderData);
        msg->addBOOLFast(_PREHASH_Override, false);
        for (bool set : { true, false })
        {
            msg->nextBlockFast(_PREHASH_ObjectData);
            msg->addU32Fast(_PREHASH_ObjectLocalID, o->getLocalID());
            msg->addU8Fast(_PREHASH_Field, PERM_NEXT_OWNER);
            msg->addBOOLFast(_PREHASH_Set, set);
            msg->addU32Fast(_PREHASH_Mask, set ? mask : (all & ~mask));
        }
        msg->sendReliable(o->getRegion()->getHost());
    }

    if (p.has("click_action"))
    {
        static const std::map<std::string, U8> actions = {
            { "touch", CLICK_ACTION_TOUCH }, { "sit", CLICK_ACTION_SIT }, { "buy", CLICK_ACTION_BUY },
            { "pay", CLICK_ACTION_PAY }, { "open", CLICK_ACTION_OPEN }, { "play", CLICK_ACTION_PLAY },
            { "open_media", CLICK_ACTION_OPEN_MEDIA }, { "zoom", CLICK_ACTION_ZOOM },
            { "disabled", CLICK_ACTION_DISABLED } };
        auto it = actions.find(p["click_action"].asString());
        if (it == actions.end())
            return "unknown click_action";
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ObjectClickAction);
        packAgent(msg);
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, o->getLocalID());
        msg->addU8("ClickAction", it->second);
        msg->sendReliable(o->getRegion()->getHost());
    }
    return "";
}

// Objects we asked the region to create (ObjectAdd, RezObject, ObjectDuplicate) and are waiting for.
struct PendingCreate
{
    LLSD modify;                // applied once it exists
    LLVector3 want_pos;         // where we expect it (region frame)
    bool move_to_pos = false;   // move it there if the region put it elsewhere
    F64 sent = 0.0;
    std::set<LLUUID> before;    // my objects already near want_pos when we asked
    LLUUID created;
};
std::deque<std::shared_ptr<PendingCreate>> sPendingCreates;
std::vector<LLPointer<LLViewerObject>> sJustCreated;
std::set<LLUUID> sClaimed;      // objects already matched to a request
const F64 CREATE_TIMEOUT = 15.0;
const F32 NEAR_RADIUS = 1.5f;
F64 sLastScan = 0.0;

std::set<LLUUID> myObjectsNear(const LLVector3& pos, F32 radius)
{
    std::set<LLUUID> ids;
    LLViewerRegion* region = gAgent.getRegion();
    S32 n = gObjectList.getNumObjects();
    for (S32 i = 0; i < n; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);
        if (o && !o->isDead() && o->getRegion() == region && o->getRootEdit() == o && !o->isAttachment()
            && !o->isAvatar() && o->flagObjectYouOwner() && dist_vec(o->getPositionRegion(), pos) < radius)
        {
            ids.insert(o->getID());
        }
    }
    return ids;
}

std::shared_ptr<PendingCreate> newPending(const LLSD& modify, const LLVector3& want_pos, bool move_to_pos)
{
    auto pending = std::make_shared<PendingCreate>();
    pending->modify = modify;
    pending->want_pos = want_pos;
    pending->move_to_pos = move_to_pos;
    pending->sent = (F64)LLTimer::getTotalSeconds();
    pending->before = myObjectsNear(want_pos, NEAR_RADIUS);
    sPendingCreates.push_back(pending);
    ensureIdle();
    return pending;
}

void claim(const std::shared_ptr<PendingCreate>& pending, LLViewerObject* o)
{
    pending->created = o->getID();
    sClaimed.insert(o->getID());
    LLSD ev = llsd::map("id", o->getID(), "local_id", (LLSD::Integer)o->getLocalID(),
                        "pos", ll_sd_from_vector3(o->getPositionRegion()), "requested", true);
    LLSD mod = pending->modify;
    if (pending->move_to_pos && dist_vec(o->getPositionRegion(), pending->want_pos) > 0.01f)
        mod["pos"] = ll_sd_from_vector3(pending->want_pos);
    if (mod.size())
    {
        std::string err = modifyObject(o, mod);
        if (!err.empty())
            ev["modify_error"] = err;
    }
    ibasho::postEvent("object_created", ev);
}

void processCreated()
{
    F64 now = (F64)LLTimer::getTotalSeconds();
    while (!sPendingCreates.empty() && now - sPendingCreates.front()->sent > CREATE_TIMEOUT)
        sPendingCreates.pop_front();

    std::vector<LLPointer<LLViewerObject>> objs;
    objs.swap(sJustCreated);
    for (auto& o : objs)
    {
        // Duplicated/rezzed linksets flag every prim; children are attached to their root by now.
        if (o->isDead() || o->getRootEdit() != o.get() || sClaimed.count(o->getID()))
            continue;
        auto it = std::find_if(sPendingCreates.begin(), sPendingCreates.end(),
                               [](const auto& p) { return p->created.isNull(); });
        if (it != sPendingCreates.end())
        {
            claim(*it, o);
        }
        else
        {
            sClaimed.insert(o->getID());
            ibasho::postEvent("object_created", llsd::map("id", o->getID(), "local_id", (LLSD::Integer)o->getLocalID(),
                                                          "pos", ll_sd_from_vector3(o->getPositionRegion())));
        }
    }

    // Fallback for regions that don't flag new objects as create-selected (OpenSim duplicates):
    // a new object of mine near where we expected it.
    if (now - sLastScan > 0.25)
    {
        sLastScan = now;
        for (const auto& pending : sPendingCreates)
        {
            if (pending->created.notNull() || now - pending->sent < 0.5)
                continue;
            for (const LLUUID& id : myObjectsNear(pending->want_pos, NEAR_RADIUS))
            {
                if (!pending->before.count(id) && !sClaimed.count(id))
                {
                    claim(pending, gObjectList.findObject(id));
                    break;
                }
            }
        }
    }
    sPendingCreates.erase(std::remove_if(sPendingCreates.begin(), sPendingCreates.end(),
                                         [](const auto& p) { return p->created.notNull(); }),
                          sPendingCreates.end());
    if (sClaimed.size() > 1000)
        sClaimed.clear();
}

// Reply to a create-like request once its object shows up.
void replyWhenCreated(const LLSD& data, std::shared_ptr<PendingCreate> pending)
{
    ibasho::deferReply(data, CREATE_TIMEOUT, [pending](LLSD& reply, bool expired)
    {
        if (pending->created.isNull())
        {
            if (expired)
                reply["error"] = "timed out waiting for the new object";
            return false;
        }
        reply["id"] = pending->created;
        if (LLViewerObject* o = gObjectList.findObject(pending->created))
            reply["local_id"] = (LLSD::Integer)o->getLocalID();
        return true;
    });
}

LLVector3 defaultRezPos()
{
    LLVector3 at = gAgent.getAtAxis();
    at.mV[VZ] = 0.f;
    at.normalize();
    return gAgent.getPositionAgent() + at * 2.f; // agent frame == current region frame
}

// Group root objects of ids by region, for derez/link messages. Returns an error or "".
std::string rootsByRegion(const std::vector<LLUUID>& ids, bool roots_only,
                          std::map<LLViewerRegion*, std::vector<LLViewerObject*>>& out)
{
    std::set<LLViewerObject*> seen;
    for (const LLUUID& id : ids)
    {
        LLViewerObject* o = gObjectList.findObject(id);
        if (!o || o->isDead() || !o->getRegion())
            return "object not found: " + id.asString();
        if (roots_only)
            o = o->getRootEdit();
        if (o->isAttachment())
            return "is an attachment: " + id.asString();
        if (seen.insert(o).second)
            out[o->getRegion()].push_back(o);
    }
    return "";
}

// ---------------------------------------------------------------- IbashoBuild

class IbashoBuildListener : public LLEventAPI
{
public:
    IbashoBuildListener()
        : LLEventAPI("IbashoBuild",
                     "Create, modify, link, take and delete objects without the build tools.\n"
                     "Positions are region coordinates of the agent's current region.\n"
                     "New objects are also announced on IbashoEvents as {event:\"object_created\"}.")
    {
        const char* modify_keys =
            "Modify keys: pos [x,y,z] (region; a root moves its linkset), local_pos (child, relative to root),\n"
            "rot [x,y,z,w] or rot_euler [deg x,y,z], local_rot / local_rot_euler, scale [x,y,z],\n"
            "shape (box cylinder prism sphere torus tube ring cone pyramid; resets shape params),\n"
            "profile_cut [b,e], path_cut [b,e], hollow, twist [b,e], taper [x,y], top_shear [x,y],\n"
            "hole_size [x,y], radius_offset, revolutions, skew,\n"
            "face (default all) with color [r,g,b(,a)] 0-1, alpha, texture (asset uuid), repeats [u,v],\n"
            "glow, fullbright, shiny 0-3; name, desc, phantom, physics, temp, click_action,\n"
            "sale {type: original|copy|contents, price} (false: not for sale), next_perms (e.g. \"MC\").";
        add("create",
            std::string("Create a prim: [\"shape\"] (default box) at [\"pos\"] (default 2 m ahead), with any\n"
                        "modify keys applied once it exists. Replies {id, local_id} when the region has made it.\n")
                + modify_keys,
            &IbashoBuildListener::create);
        add("modify",
            std::string("Modify prim [\"id\"] (or each of [\"ids\"]); needs modify rights.\n") + modify_keys,
            &IbashoBuildListener::modify);
        add("duplicate",
            "Copy object [\"id\"] (whole linkset) offset by [\"offset\"] (default [0,0,0.5]); optional modify\n"
            "keys for the copy. Replies {id} of the copy.",
            &IbashoBuildListener::duplicate,
            llsd::map("id", LLSD()));
        add("rez",
            "Rez inventory object [\"item_id\"] at [\"pos\"] (default 2 m ahead); optional modify keys.\n"
            "Replies {id} of the rezzed root.",
            &IbashoBuildListener::rez,
            llsd::map("item_id", LLSD()));
        add("link",
            "Link [\"ids\"] into one linkset; the first becomes the root.",
            &IbashoBuildListener::link,
            llsd::map("ids", LLSD()));
        add("unlink",
            "Unlink prims [\"ids\"] (a root id unlinks its whole linkset).",
            &IbashoBuildListener::unlink,
            llsd::map("ids", LLSD()));
        add("take",
            "Take objects [\"ids\"] (roots) into inventory ([\"folder_id\"], default Objects).\n"
            "[\"copy\"]: take a copy and leave the original.",
            &IbashoBuildListener::take,
            llsd::map("ids", LLSD()));
        add("delete",
            "Delete objects [\"ids\"] (roots) to Trash.",
            &IbashoBuildListener::deleteObjects,
            llsd::map("ids", LLSD()));
        add("returnObjects",
            "Return objects [\"ids\"] to their owners (needs land rights).",
            &IbashoBuildListener::returnObjects,
            llsd::map("ids", LLSD()));
    }

private:
    void create(const LLSD& data)
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (!region)
        {
            Response r(LLSD(), data);
            return r.error("no region");
        }
        std::string shape_name = data.has("shape") ? data["shape"].asString() : "box";
        const ShapeType* shape = findShape(shape_name);
        if (!shape)
        {
            Response r(LLSD(), data);
            return r.error("unknown shape " + shape_name);
        }
        LLVolumeParams params;
        resetShape(params, *shape);
        std::string err;
        applyShape(params, data, err);
        if (!err.empty())
        {
            Response r(LLSD(), data);
            return r.error(err);
        }
        LLVector3 pos = data.has("pos") ? ll_vector3_from_sd(data["pos"]) : defaultRezPos();
        LLVector3 scale = data.has("scale") ? ll_vector3_from_sd(data["scale"]) : LLVector3(0.5f, 0.5f, 0.5f);
        bool have_rot = false;
        LLQuaternion rot = rotFrom(data, "rot", "rot_euler", &have_rot);
        if (!have_rot && shape->path == LL_PCODE_PATH_CIRCLE)
            rot.setQuat(90.f * DEG_TO_RAD, LLVector3::y_axis); // as the build tool does

        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ObjectAdd);
        packAgent(msg, true);
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU8Fast(_PREHASH_Material, LL_MCODE_WOOD);
        msg->addU32Fast(_PREHASH_AddFlags, FLAGS_CREATE_SELECTED);
        LLVolumeMessage::packVolumeParams(&params, msg);
        msg->addU8Fast(_PREHASH_PCode, LL_PCODE_VOLUME);
        msg->addVector3Fast(_PREHASH_Scale, scale);
        msg->addQuatFast(_PREHASH_Rotation, rot);
        msg->addVector3Fast(_PREHASH_RayStart, pos);
        msg->addVector3Fast(_PREHASH_RayEnd, pos);
        msg->addU8Fast(_PREHASH_BypassRaycast, 1);
        msg->addU8Fast(_PREHASH_RayEndIsIntersection, 1);
        msg->addU8Fast(_PREHASH_State, 0);
        msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
        msg->sendReliable(region->getHost());

        auto pending = newPending(modifyKeysOnly(data, { "shape", "pos", "scale", "rot", "rot_euler", "profile_cut",
                                                         "path_cut", "hollow", "twist", "top_shear", "hole_size",
                                                         "taper", "radius_offset", "revolutions", "skew" }),
                                  pos, true);
        replyWhenCreated(data, pending);
    }

    static LLSD modifyKeysOnly(const LLSD& data, std::initializer_list<const char*> drop)
    {
        static const std::set<std::string> envelope = { "op", "reply", "reqid", "id", "ids", "item_id", "offset" };
        LLSD out = LLSD::emptyMap();
        for (const auto& [k, v] : llsd::inMap(data))
        {
            if (envelope.count(k))
                continue;
            bool skip = false;
            for (const char* d : drop)
                skip |= k == d;
            if (!skip)
                out[k] = v;
        }
        return out;
    }

    void modify(const LLSD& data)
    {
        Response response(LLSD(), data);
        LLSD mod = modifyKeysOnly(data, {});
        for (const LLUUID& id : idList(data))
        {
            std::string err = modifyObject(gObjectList.findObject(id), mod);
            if (!err.empty())
                return response.error(id.asString() + ": " + err);
        }
    }

    void duplicate(const LLSD& data)
    {
        LLViewerObject* o = findObject(data["id"]);
        if (!o || !o->getRegion())
        {
            Response r(LLSD(), data);
            return r.error("object not found");
        }
        o = o->getRootEdit();
        LLVector3 offset = data.has("offset") ? ll_vector3_from_sd(data["offset"]) : LLVector3(0.f, 0.f, 0.5f);
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_ObjectDuplicate);
        packAgent(msg, true);
        msg->nextBlockFast(_PREHASH_SharedData);
        msg->addVector3Fast(_PREHASH_Offset, offset);
        msg->addU32Fast(_PREHASH_DuplicateFlags, FLAGS_CREATE_SELECTED);
        msg->nextBlockFast(_PREHASH_ObjectData);
        msg->addU32Fast(_PREHASH_ObjectLocalID, o->getLocalID());
        msg->sendReliable(o->getRegion()->getHost());

        auto pending = newPending(modifyKeysOnly(data, {}), o->getPositionRegion() + offset, false);
        replyWhenCreated(data, pending);
    }

    void rez(const LLSD& data)
    {
        LLViewerInventoryItem* item = gInventory.getItem(data["item_id"].asUUID());
        LLViewerRegion* region = gAgent.getRegion();
        std::string err;
        if (!item)
            err = "item not in inventory (or inventory not loaded)";
        else if (item->getType() != LLAssetType::AT_OBJECT)
            err = "not an object item";
        else if (!region)
            err = "no region";
        else if (gInventory.isObjectDescendentOf(item->getUUID(), gInventory.findCategoryUUIDForType(LLFolderType::FT_CURRENT_OUTFIT)))
            err = "item is worn; detach it first";
        if (!err.empty())
        {
            Response r(LLSD(), data);
            return r.error(err);
        }
        LLVector3 pos = data.has("pos") ? ll_vector3_from_sd(data["pos"]) : defaultRezPos();
        bool copyable = item->getPermissions().allowCopyBy(gAgent.getID());

        LLMessageSystem* msg = gMessageSystem;
        msg->newMessageFast(_PREHASH_RezObject);
        packAgent(msg, true);
        msg->nextBlock("RezData");
        msg->addUUIDFast(_PREHASH_FromTaskID, LLUUID::null);
        msg->addU8Fast(_PREHASH_BypassRaycast, 1);
        msg->addVector3Fast(_PREHASH_RayStart, pos);
        msg->addVector3Fast(_PREHASH_RayEnd, pos);
        msg->addUUIDFast(_PREHASH_RayTargetID, LLUUID::null);
        msg->addBOOLFast(_PREHASH_RayEndIsIntersection, true);
        msg->addBOOLFast(_PREHASH_RezSelected, true);
        msg->addBOOLFast(_PREHASH_RemoveItem, !copyable);
        pack_permissions_slam(msg, item->getFlags(), item->getPermissions());
        msg->nextBlockFast(_PREHASH_InventoryData);
        item->packMessage(msg);
        msg->sendReliable(region->getHost());

        auto pending = newPending(modifyKeysOnly(data, { "pos" }), pos, true);
        replyWhenCreated(data, pending);
    }

    void sendLocalIDs(const char* message, LLViewerRegion* region, const std::vector<LLViewerObject*>& objs)
    {
        LLMessageSystem* msg = gMessageSystem;
        msg->newMessage(message);
        packAgent(msg);
        for (LLViewerObject* o : objs)
        {
            msg->nextBlockFast(_PREHASH_ObjectData);
            msg->addU32Fast(_PREHASH_ObjectLocalID, o->getLocalID());
        }
        msg->sendReliable(region->getHost());
    }

    void link(const LLSD& data)
    {
        Response response(LLSD(), data);
        std::map<LLViewerRegion*, std::vector<LLViewerObject*>> by_region;
        std::vector<LLUUID> ids = idList(data);
        std::string err = rootsByRegion(ids, true, by_region);
        if (!err.empty())
            return response.error(err);
        if (by_region.size() != 1)
            return response.error("objects must all be in one region");
        std::vector<LLViewerObject*>& objs = by_region.begin()->second;
        if (objs.size() < 2)
            return response.error("need at least two separate objects");
        // ObjectLink: the first local id becomes the new root.
        sendLocalIDs("ObjectLink", by_region.begin()->first, objs);
    }

    void unlink(const LLSD& data)
    {
        Response response(LLSD(), data);
        std::map<LLViewerRegion*, std::vector<LLViewerObject*>> by_region;
        std::string err = rootsByRegion(idList(data), false, by_region);
        if (!err.empty())
            return response.error(err);
        for (auto& [region, objs] : by_region)
        {
            std::vector<LLViewerObject*> prims;
            for (LLViewerObject* o : objs)
            {
                prims.push_back(o);
                if (o->getRootEdit() == o) // a root: delink the whole set
                    for (const auto& c : o->getChildren())
                        if (!c->isAvatar())
                            prims.push_back(c);
            }
            sendLocalIDs("ObjectDelink", region, prims);
        }
    }

    void derez(const LLSD& data, EDeRezDestination dest, const LLUUID& dest_id)
    {
        Response response(LLSD(), data);
        std::map<LLViewerRegion*, std::vector<LLViewerObject*>> by_region;
        std::string err = rootsByRegion(idList(data), true, by_region);
        if (!err.empty())
            return response.error(err);
        S32 n = 0;
        for (auto& [region, objs] : by_region)
        {
            LLUUID tid;
            tid.generate();
            LLMessageSystem* msg = gMessageSystem;
            msg->newMessageFast(_PREHASH_DeRezObject);
            packAgent(msg);
            msg->nextBlockFast(_PREHASH_AgentBlock);
            msg->addUUIDFast(_PREHASH_GroupID, gAgent.getGroupID());
            msg->addU8Fast(_PREHASH_Destination, (U8)dest);
            msg->addUUIDFast(_PREHASH_DestinationID, dest_id);
            msg->addUUIDFast(_PREHASH_TransactionID, tid);
            msg->addU8Fast(_PREHASH_PacketCount, 1);
            msg->addU8Fast(_PREHASH_PacketNumber, 0);
            for (LLViewerObject* o : objs)
            {
                msg->nextBlockFast(_PREHASH_ObjectData);
                msg->addU32Fast(_PREHASH_ObjectLocalID, o->getLocalID());
                ++n;
            }
            msg->sendReliable(region->getHost());
        }
        response["count"] = n;
    }

    void take(const LLSD& data)
    {
        LLUUID folder = data.has("folder_id") ? data["folder_id"].asUUID()
                                              : gInventory.findCategoryUUIDForType(LLFolderType::FT_OBJECT);
        derez(data, data["copy"].asBoolean() ? DRD_ACQUIRE_TO_AGENT_INVENTORY : DRD_TAKE_INTO_AGENT_INVENTORY, folder);
    }

    void deleteObjects(const LLSD& data)
    {
        derez(data, DRD_TRASH, gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH));
    }

    void returnObjects(const LLSD& data)
    {
        derez(data, DRD_RETURN_TO_OWNER, LLUUID::null);
    }
};

IbashoBuildListener sIbashoBuildListener;

LLSD readFamilyOrFull(LLMessageSystem* msg, S32 i, bool full)
{
    LLUUID owner_id, group_id, last_owner_id;
    U32 base_mask, owner_mask, group_mask, everyone_mask, next_owner_mask;
    LLSaleInfo sale_info;
    std::string name, desc;
    msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_OwnerID, owner_id, i);
    msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_GroupID, group_id, i);
    msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_LastOwnerID, last_owner_id, i);
    msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_BaseMask, base_mask, i);
    msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_OwnerMask, owner_mask, i);
    msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_GroupMask, group_mask, i);
    msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_EveryoneMask, everyone_mask, i);
    msg->getU32Fast(_PREHASH_ObjectData, _PREHASH_NextOwnerMask, next_owner_mask, i);
    if (full)
        sale_info.unpackMultiMessage(msg, _PREHASH_ObjectData, i);
    else
        sale_info.unpackMessage(msg, _PREHASH_ObjectData);
    msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Name, name, i);
    msg->getStringFast(_PREHASH_ObjectData, _PREHASH_Description, desc, i);

    LLSD p = llsd::map("name", name, "desc", desc, "owner_id", owner_id,
                       "perms", perms(base_mask, owner_mask, group_mask, everyone_mask, next_owner_mask));
    if (group_id.notNull())
        p["group_id"] = group_id;
    if (last_owner_id.notNull() && last_owner_id != owner_id)
        p["last_owner_id"] = last_owner_id;
    LLSD sale = saleSD(sale_info);
    if (sale.isDefined())
        p["sale"] = sale;
    if (full)
    {
        LLUUID creator_id;
        U64 creation_date = 0;
        std::string touch_name, sit_name;
        msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_CreatorID, creator_id, i);
        msg->getU64Fast(_PREHASH_ObjectData, _PREHASH_CreationDate, creation_date, i);
        msg->getStringFast(_PREHASH_ObjectData, _PREHASH_TouchName, touch_name, i);
        msg->getStringFast(_PREHASH_ObjectData, _PREHASH_SitName, sit_name, i);
        p["creator_id"] = creator_id;
        p["created"] = (LLSD::Integer)(creation_date / 1000000); // microseconds -> unix seconds
        if (!touch_name.empty())
            p["touch_name"] = touch_name;
        if (!sit_name.empty())
            p["sit_name"] = sit_name;
        p["full"] = true;
    }
    return p;
}
} // namespace

namespace ibasho
{
void onObjectPropertiesFamily(LLMessageSystem* msg)
{
    ensureIdle();
    LLUUID id;
    msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_ObjectID, id);
    storeProps(id, readFamilyOrFull(msg, 0, false));
}

void onObjectProperties(LLMessageSystem* msg, S32 block)
{
    ensureIdle();
    LLUUID id;
    msg->getUUIDFast(_PREHASH_ObjectData, _PREHASH_ObjectID, id, block);
    storeProps(id, readFamilyOrFull(msg, block, true));
}

void onObjectCreated(LLViewerObject* objectp)
{
    ensureIdle();
    if (objectp)
        sJustCreated.push_back(objectp);
}
} // namespace ibasho
