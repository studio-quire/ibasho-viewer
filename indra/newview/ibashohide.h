/**
 * @file   ibashohide.h
 * @brief  Window-only avatar hiding (IbashoAgent hideAvatars). Hooks for the render code: hidden avatars, their
 *         attachments (rigged too), attached lights and particles, name tags and hover text are skipped in renders
 *         for the window, so they never reach a screen capture. Snapshots (gSnapshot) and impostor renders still
 *         draw them, so the agent's own snapshots see everyone. Culling and texture loading are left alone, so a
 *         snapshot finds them loaded. Written by Claude Opus 5.5, 2026-10-12.
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

#ifndef LL_IBASHOHIDE_H
#define LL_IBASHOHIDE_H

#include "stdtypes.h"

class LLDrawInfo;
class LLSD;
class LLSpatialGroup;
class LLViewerObject;
class LLVOAvatar;

namespace ibasho
{
    extern bool gHideActive; // hiding is on at all; every hook checks this first

    bool hideAvatarSlow(const LLVOAvatar* av);
    bool hideObjectSlow(LLViewerObject* obj);
    bool hideGroupSlow(LLSpatialGroup* group);
    bool hideParticleSlow(LLViewerObject* part_group, S32 idx);
    bool hideDrawInfoSlow(const LLDrawInfo* info);
    bool isHiddenDrawInfoSlow(const LLDrawInfo* info);
    void markHiddenDrawInfo(const LLDrawInfo* info);
    void forgetDrawInfoSlow(const LLDrawInfo* info);

    // Skip drawing this in the current render? (The avatar body, its impostor and shadow.)
    inline bool hideAvatarNow(const LLVOAvatar* av) { return gHideActive && hideAvatarSlow(av); }
    // An avatar, or any prim attached to one (name tags, hover text, attached lights).
    inline bool hideObjectNow(LLViewerObject* obj) { return gHideActive && hideObjectSlow(obj); }
    // A spatial group in an attachment's bridge (everything attachments draw through the render maps).
    inline bool hideGroupNow(LLSpatialGroup* group) { return gHideActive && hideGroupSlow(group); }

    // Particles share vertex buffers across sources, so LLParticlePartition::getGeometry gives particles from
    // hidden avatars (their attachments, or the avatar itself) their own draw infos and marks them; the draw
    // loops then skip marked infos in renders for the window.
    inline bool hideParticle(LLViewerObject* part_group, S32 idx) { return gHideActive && hideParticleSlow(part_group, idx); }
    inline bool isHiddenDrawInfo(const LLDrawInfo* info) { return gHideActive && isHiddenDrawInfoSlow(info); }
    inline bool hideDrawInfoNow(const LLDrawInfo* info) { return gHideActive && hideDrawInfoSlow(info); }
    inline void forgetDrawInfo(const LLDrawInfo* info) { if (gHideActive) forgetDrawInfoSlow(info); }

    void hideAvatars(const LLSD& request);
    void getHidden(const LLSD& request);
}

#endif
