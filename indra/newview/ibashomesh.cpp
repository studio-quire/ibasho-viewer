/**
 * @file   ibashomesh.cpp
 * @brief  IbashoAgent uploadMesh: the model upload floater, driven without its file dialog.
 *         Written by Claude Opus 5.5, 2026-10-10.
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

#include "llbutton.h"
#include "llfloatermodelpreview.h"
#include "llfloaterreg.h"
#include "llinventorymodel.h"
#include "lllineeditor.h"
#include "llmodelloader.h"
#include "llmodelpreview.h"
#include "llevents.h"
#include "llsdutil.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewertexteditor.h"

#include <algorithm>
#include <deque>

// Does what a resident does in the floater: pick the file for the high LOD, then (if given) a file for
// each lower LOD and the physics, one at a time as "Load from file" does (the rest as the floater
// generates them by default), press "Calculate weights & fee", then "Upload". It's a friend of
// LLFloaterModelPreview so it can call the same handlers the buttons do (LOD files added 2026-10-17).
class IbashoMeshAccess
{
public:
    enum Stage { LOADING, CALCULATING, UPLOADING, FINDING_ITEM };

    struct State
    {
        LLHandle<LLFloater> floater;
        Stage stage = LOADING;
        std::deque<std::pair<S32, std::string>> lod_files; // still to load, after the high LOD
        std::vector<S32> from_file;                        // LODs given as files
        S32 loading_lod = LLModel::LOD_HIGH;
        S32 high_tris = -1;
        bool quote = false, keep_open = false;
        S32 max_cost = -1;
        S32 ready_frames = 0;
        time_t start = 0;
        std::string name;
        LLUUID folder_id;
        LLSD fee, lods;
    };
    typedef std::shared_ptr<State> state_t;

    static const char* lodName(S32 lod)
    {
        static const char* names[] = { "lowest", "low", "med", "high", "physics" };
        return lod >= 0 && lod < LLModel::NUM_LODS ? names[lod] : "?";
    }

    static bool modelFile(const std::string& path)
    {
        std::string ext = gDirUtilp->getExtension(path);
        return ext == "dae" || ext == "gltf" || ext == "glb";
    }

    // The floater pairs each high-LOD object with a model of the same name in each LOD's file. If none
    // matches, it quietly falls back to a model from a higher LOD (or no physics), so check that every
    // LOD given as a file is really what will be uploaded.
    static std::string checkFileLods(const state_t& s, LLModelPreview* mp)
    {
        for (S32 lod : s->from_file)
        {
            const auto& loaded = mp->mModel[lod];
            for (const LLModelInstance& inst : mp->mUploadData)
            {
                const LLModel* used = inst.mLOD[lod].get();
                bool ok = used && std::any_of(loaded.begin(), loaded.end(),
                                              [used](const LLPointer<LLModel>& m) { return m.get() == used; });
                if (!ok)
                    return "the " + std::string(lodName(lod)) + " file has no object matching \"" + inst.mLabel +
                           "\" in the high LOD (give it the same object name); the floater would have used " +
                           (used ? "\"" + used->mLabel + "\"" : std::string("nothing"));
            }
        }
        return std::string();
    }

    static S32 baseTris(LLModelPreview* mp)
    {
        S32 tris = 0;
        for (const auto& m : mp->mBaseModel)
            if (m.notNull())
                tris += m->getNumTriangles();
        return tris;
    }

    static LLSD lodSummary(const state_t& s, LLModelPreview* mp)
    {
        LLSD lods = LLSD::emptyMap();
        for (S32 lod = 0; lod < LLModel::NUM_LODS; ++lod)
        {
            S32 tris = 0;
            for (const LLModelInstance& inst : mp->mUploadData)
                if (inst.mLOD[lod].notNull())
                    tris += inst.mLOD[lod]->getNumTriangles();
            bool file = lod == LLModel::LOD_HIGH ||
                        std::find(s->from_file.begin(), s->from_file.end(), lod) != s->from_file.end();
            lods[lodName(lod)] = llsd::map("tris", tris, "from", file ? "file" : "floater");
        }
        return lods;
    }

    static LLFloaterModelPreview* floater(const state_t& s)
    {
        return dynamic_cast<LLFloaterModelPreview*>(s->floater.get());
    }

    static std::string log(LLFloaterModelPreview* fmp)
    {
        std::string text = fmp && fmp->mUploadLogText ? fmp->mUploadLogText->getText() : std::string();
        LLStringUtil::trim(text);
        return text.size() > 1500 ? text.substr(text.size() - 1500) : text;
    }

    static void close(const state_t& s)
    {
        // Closing deletes the preview; a loader still running would then call back into it and crash.
        LLFloaterModelPreview* fmp = floater(s);
        if (fmp && !s->keep_open && !fmp->isModelLoading())
            fmp->closeFloater(false);
    }

    static bool fail(LLSD& reply, const state_t& s, const std::string& error)
    {
        reply["error"] = error;
        std::string text = log(floater(s));
        if (!text.empty())
            reply["log"] = text;
        close(s);
        return true;
    }

    static LLSD weights(const LLSD& fee)
    {
        return llsd::map("land_impact", fee["resource_cost"], "streaming", fee["model_streaming_cost"],
                         "physics", fee["physics_cost"], "server", fee["simulation_cost"],
                         "price_breakdown", fee["upload_price_breakdown"]);
    }

    static std::string refusal(const state_t& s, S32 cost)
    {
        S32 cap = gSavedSettings.getS32("IbashoMaxUploadCost");
        if (s->max_cost >= 0 && cost > s->max_cost)
            return llformat("the fee is L$%d, over max_cost L$%d", cost, s->max_cost);
        if (cost > cap)
            return llformat("the fee is L$%d, over the upload cap of L$%d (IbashoMaxUploadCost)", cost, cap);
        return ibasho::spendRefusal(cost);
    }

    static LLViewerInventoryItem* findItem(const state_t& s)
    {
        // The new item lands straight in the destination folder (default: Objects).
        LLUUID folder = s->folder_id.notNull() ? s->folder_id
                                               : gInventory.findUserDefinedCategoryUUIDForType(LLFolderType::FT_OBJECT);
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder, cats, items);
        LLViewerInventoryItem* best = nullptr;
        if (!items)
            return nullptr;
        for (const auto& item : *items)
        {
            if (item && item->getType() == LLAssetType::AT_OBJECT && item->getName() == s->name &&
                item->getCreationDate() >= s->start - 5 && (!best || item->getCreationDate() > best->getCreationDate()))
                best = item.get();
        }
        return best;
    }

    static void start(const LLSD& data)
    {
        auto reply_error = [&data](const std::string& e) { sendReply(llsd::map("error", e), data); };
        const std::string path = data["path"].asString();
        if (!gDirUtilp->fileExists(path))
            return reply_error("no such file: " + path);
        if (!modelFile(path))
            return reply_error("the model floater reads .dae, .gltf and .glb");
        std::deque<std::pair<S32, std::string>> lod_files;
        static const std::pair<const char*, S32> lod_keys[] = { { "lod_med", LLModel::LOD_MEDIUM },
                                                                 { "lod_low", LLModel::LOD_LOW },
                                                                 { "lod_lowest", LLModel::LOD_IMPOSTOR },
                                                                 { "physics", LLModel::LOD_PHYSICS } };
        for (const auto& [key, lod] : lod_keys)
        {
            if (!data.has(key) || data[key].asString().empty())
                continue;
            std::string file = data[key].asString();
            if (!gDirUtilp->fileExists(file))
                return reply_error(std::string(key) + ": no such file: " + file);
            if (!modelFile(file))
                return reply_error(std::string(key) + ": the model floater reads .dae, .gltf and .glb");
            lod_files.emplace_back(lod, file);
        }
        LLUUID folder_id = data["folder_id"].asUUID();
        if (folder_id.notNull() && !gInventory.getCategory(folder_id))
            return reply_error("no such folder");
        LLFloaterModelPreview* open = LLFloaterReg::findTypedInstance<LLFloaterModelPreview>("upload_model");
        if (open && (open->getVisible() || open->isModelLoading()))
            return reply_error("the model upload floater is already open; close it first "
                               "(LLFloaterReg hideInstance {\"name\":\"upload_model\"})");

        LLFloaterModelPreview* fmp = LLFloaterReg::getTypedInstance<LLFloaterModelPreview>("upload_model");
        if (!fmp || !fmp->mModelPreview)
            return reply_error("couldn't make the model upload floater");
        auto s = std::make_shared<State>();
        s->floater = fmp->getHandle();
        s->quote = data["quote"].asBoolean();
        s->keep_open = data["keep_open"].asBoolean();
        s->max_cost = data.has("max_cost") ? llmax(0, data["max_cost"].asInteger()) : -1;   // negative: nothing
        s->start = time_corrected();
        s->folder_id = folder_id;
        s->lod_files = lod_files;
        for (const auto& lf : lod_files)
            s->from_file.push_back(lf.first);

        fmp->setUploadDestination(folder_id);
        if (data.has("name"))
            fmp->getChild<LLLineEditor>("description_form")->setText(data["name"].asString());
        if (data.has("scale"))
            fmp->getChild<LLUICtrl>("import_scale")->setValue(data["scale"].asReal());
        if (data.has("textures"))
            fmp->getChild<LLUICtrl>("upload_textures")->setValue(data["textures"].asBoolean());
        fmp->loadModel(LLModel::LOD_HIGH, path); // opens the floater, as picking a file would

        F64 timeout = data.has("timeout") ? data["timeout"].asReal() : 180.0;
        ibasho::deferReply(data, timeout, [s](LLSD& reply, bool expired) { return poll(s, reply, expired); });
    }

    static bool poll(const state_t& s, LLSD& reply, bool expired)
    {
        LLFloaterModelPreview* fmp = floater(s);
        if (expired)
        {
            static const char* stages[] = { "loading the model", "calculating the fee", "uploading", "waiting for the item" };
            return fail(reply, s, std::string("timed out ") + stages[s->stage] +
                        (s->stage >= UPLOADING ? "; the upload may still finish (watch inventory and money events)" : ""));
        }
        switch (s->stage)
        {
        case LOADING:
        {
            if (!fmp)
                return fail(reply, s, "the model floater closed while loading");
            LLModelPreview* mp = fmp->mModelPreview;
            mp->update(); // the floater's draw() does this, but it isn't drawn while the UI is hidden
            if (fmp->isModelLoading())
            {
                s->ready_frames = 0;
                return false;
            }
            if (mp->getLoadState() >= LLModelLoader::ERROR_PARSING)
                return fail(reply, s, std::string(mp->getLoadState() == LLModelLoader::ERROR_MATERIALS
                                                      ? "the LODs don't fit together (materials or object names; see log)"
                                                      : "the model didn't load") +
                                          " after loading the " + lodName(s->loading_lod) + " file (loader state " +
                                          std::to_string(mp->getLoadState()) + ")");
            // LOD generation continues over idle frames after the load; wait until it's been quiet a while.
            s->ready_frames = mp->lodsReady() ? s->ready_frames + 1 : 0;
            if (s->ready_frames < 10)
                return false;
            if (s->high_tris < 0)
                s->high_tris = baseTris(mp);
            else if (baseTris(mp) != s->high_tris) // a file with a different object count replaces the high LOD
                return fail(reply, s, std::string("the ") + lodName(s->loading_lod) +
                                          " file doesn't have the same objects as the high LOD, so the floater "
                                          "replaced the high LOD with it");
            if (!s->lod_files.empty())
            {
                // Like picking "Load from file" for that LOD and then the file; one load at a time.
                auto [lod, file] = s->lod_files.front();
                s->lod_files.pop_front();
                s->loading_lod = lod;
                s->ready_frames = 0;
                fmp->setCtrlLoadFromFile(lod);
                fmp->loadModel(lod, file);
                return false;
            }
            mp->rebuildUploadData(); // the calculate button does this too; check the result first
            if (mp->getLoadState() >= LLModelLoader::ERROR_PARSING)
                return fail(reply, s, "the floater rejected the model (loader state " +
                                          std::to_string(mp->getLoadState()) + "; see log)");
            std::string mismatch = checkFileLods(s, mp);
            if (!mismatch.empty())
                return fail(reply, s, mismatch);
            s->lods = lodSummary(s, mp);
            s->name = fmp->getChild<LLLineEditor>("description_form")->getText();
            fmp->onClickCalculateBtn();
            s->stage = CALCULATING;
            return false;
        }
        case CALCULATING:
        {
            if (!fmp)
                return fail(reply, s, "the model floater closed while calculating");
            if (fmp->mCalculateBtn->getVisible()) // the fee request failed, or the model changed
                return fail(reply, s, "calculating the fee failed");
            if (!fmp->mModelPhysicsFee.has("upload_price") || fmp->mUploadModelUrl.empty())
                return false;
            s->fee = fmp->mModelPhysicsFee;
            S32 cost = s->fee["upload_price"].asInteger();
            reply["name"] = s->name;
            reply["cost"] = cost;
            reply["weights"] = weights(s->fee);
            reply["lods"] = s->lods;
            std::string why_not = refusal(s, cost);
            if (!fmp->mHasUploadPerm)
                why_not = "this account may not upload mesh (the region's MeshUploadFlag check said no)";
            else if (!fmp->isModelUploadAllowed())
                why_not = "the floater won't allow this model (see log)";
            if (s->quote)
            {
                if (!why_not.empty())
                {
                    reply["refusal"] = why_not;
                    reply["log"] = log(fmp);
                }
                close(s);
                return true;
            }
            if (!why_not.empty())
                return fail(reply, s, why_not);
            LLFloaterModelPreview::onUpload(fmp);
            s->stage = UPLOADING;
            return false;
        }
        case UPLOADING:
            // Success closes the floater; failure re-enables Upload.
            if (fmp && fmp->getVisible())
            {
                if (fmp->mUploadBtn->getEnabled() || fmp->mCalculateBtn->getVisible())
                    return fail(reply, s, "the upload failed");
                return false;
            }
            ibasho::recordSpend(s->fee["upload_price"].asInteger());
            s->stage = FINDING_ITEM;
            [[fallthrough]];
        case FINDING_ITEM:
        {
            LLViewerInventoryItem* item = findItem(s);
            if (!item)
                return false;
            reply["name"] = s->name;
            reply["item_id"] = item->getUUID();
            reply["asset_id"] = item->getAssetUUID();
            reply["cost"] = s->fee["upload_price"];
            reply["charged"] = s->fee["upload_price"];
            reply["weights"] = weights(s->fee);
            reply["lods"] = s->lods;
            return true;
        }
        }
        return true;
    }
};

namespace ibasho
{
void uploadMesh(const LLSD& data)
{
    IbashoMeshAccess::start(data);
}
}
