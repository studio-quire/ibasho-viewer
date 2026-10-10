/**
 * @file   ibashomove.cpp
 * @brief  IbashoAgent move/stop: hold the avatar's movement controls for a while, as held keys do,
 *         without needing keyboard focus. Written by Claude Opus 5.5, 2026-10-10.
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
#include "llagentcamera.h"
#include "lleventapi.h"
#include "llevents.h"
#include "llsdutil.h"
#include "llsdutil_math.h"
#include "lltimer.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"

// Keys set control flags while held; the main loop sends them in AgentUpdate and clears them every
// frame (LLAgent::resetControlFlags), so we set them again from an idle callback each frame. Those
// run after the clear and before the next send. The flags reach the region exactly as keys' do, so
// scripts that took controls (vehicles) see them too.
namespace ibasho
{
namespace
{
    void replyTo(const LLSD& request, const LLSD& reply) { sendReply(reply, request); }

    constexpr F32 KEY_YAW_RATE = 90.f; // deg/s, what a held turn key gives (LLAgent::propagate)

    struct Move
    {
        U32 id = 0;
        F32 forward = 0.f, strafe = 0.f, turn = 0.f, up = 0.f; // strafe and turn: + is right
        bool run = false;
        F64 start = 0.0, end = 0.0;
        LLVector3d start_pos;
        std::shared_ptr<LLSD> done; // filled when it ends
    };

    Move sMove;
    bool sActive = false;
    U32 sNextId = 1;
    bool sHooked = false;

    S32 sign(F32 v) { return v > 0.f ? 1 : v < 0.f ? -1 : 0; }

    F32 compassHeading()
    {
        LLVector3 at = gAgent.getAtAxis();
        F32 deg = atan2f(at.mV[VX], at.mV[VY]) * RAD_TO_DEG; // 0 = north (+y), 90 = east (+x)
        return deg < 0.f ? deg + 360.f : deg;
    }

    LLSD where()
    {
        LLSD out = llsd::map("pos", ll_sd_from_vector3(gAgent.getPositionAgent()),
                             "heading", ll_round(compassHeading()),
                             "sitting", isAgentAvatarValid() && gAgentAvatarp->isSitting(),
                             "flying", gAgent.getFlying());
        if (gAgent.getRegion())
            out["region"] = gAgent.getRegion()->getName();
        return out;
    }

    void finish(const std::string& reason)
    {
        if (!sActive)
            return;
        sActive = false;
        if (sMove.run && !gAgent.getAlwaysRun())
        {
            gAgent.clearRunning();
            gAgent.sendWalkRun(false);
        }
        LLSD info = where();
        info["id"] = (S32)sMove.id;
        info["reason"] = reason;
        info["secs"] = LLTimer::getTotalSeconds().value() - sMove.start;
        info["moved"] = dist_vec(gAgent.getPositionGlobal(), sMove.start_pos);
        *sMove.done = info;
        postEvent("move_done", info);
    }

    void hold()
    {
        if (!sActive)
            return;
        if (LLTimer::getTotalSeconds() >= sMove.end)
            return finish("time");
        if (gAgent.isMovementLocked())
            return finish("locked");

        // As the key handlers do (agent_push_forward etc.), minus their camera reset, so the camera
        // stays wherever it is. Under half strength uses the nudge flags of a tapped key: slower
        // (OpenSim hands nudges to llTakeControls scripts as the full control).
        if (S32 d = sign(sMove.forward))
        {
            if (fabsf(sMove.forward) >= 0.5f)
            {
                gAgent.setControlFlags((d > 0 ? AGENT_CONTROL_AT_POS : AGENT_CONTROL_AT_NEG) | AGENT_CONTROL_FAST_AT);
                gAgentCamera.setAtKey(d);
            }
            else
            {
                gAgent.setControlFlags(d > 0 ? AGENT_CONTROL_NUDGE_AT_POS : AGENT_CONTROL_NUDGE_AT_NEG);
                gAgentCamera.setWalkKey(d);
            }
        }
        if (S32 d = -sign(sMove.strafe)) // the viewer's left is positive
        {
            if (fabsf(sMove.strafe) >= 0.5f)
                gAgent.setControlFlags((d > 0 ? AGENT_CONTROL_LEFT_POS : AGENT_CONTROL_LEFT_NEG) | AGENT_CONTROL_FAST_LEFT);
            else
                gAgent.setControlFlags(d > 0 ? AGENT_CONTROL_NUDGE_LEFT_POS : AGENT_CONTROL_NUDGE_LEFT_NEG);
            gAgentCamera.setLeftKey(d);
        }
        if (S32 d = sign(sMove.up))
        {
            gAgent.setControlFlags((d > 0 ? AGENT_CONTROL_UP_POS : AGENT_CONTROL_UP_NEG) | AGENT_CONTROL_FAST_UP);
            gAgentCamera.setUpKey(d);
        }
        if (sMove.turn != 0.f)
            gAgent.moveYaw(-sMove.turn / KEY_YAW_RATE, false); // sets the yaw flag; propagate() turns us
    }
}

void move(const LLSD& data)
{
    if (!isAgentAvatarValid() || !gAgent.getRegion())
        return replyTo(data, llsd::map("error", "no avatar"));
    if (gAgent.isMovementLocked())
        return replyTo(data, llsd::map("error", "movement is locked"));
    F64 secs = data.has("secs") ? data["secs"].asReal() : 1.0;
    if (!(secs > 0.0 && secs <= 600.0))
        return replyTo(data, llsd::map("error", "secs must be in (0, 600]"));
    Move m;
    m.forward = llclamp((F32)data["forward"].asReal(), -1.f, 1.f);
    m.strafe = llclamp((F32)data["strafe"].asReal(), -1.f, 1.f);
    m.up = llclamp((F32)data["up"].asReal(), -1.f, 1.f);
    m.turn = llclamp((F32)data["turn"].asReal(), -720.f, 720.f);

    if (sActive)
        finish("replaced");
    if (!sHooked)
    {
        sHooked = true;
        onIdle(hold);
    }

    gAgent.clearAFK();
    if (data.has("fly") && data["fly"].asBoolean() != gAgent.getFlying())
        gAgent.setFlying(data["fly"].asBoolean());
    if (data.has("face"))
    {
        F32 b = (F32)data["face"].asReal() * DEG_TO_RAD; // compass bearing
        gAgent.resetAxes(LLVector3(sinf(b), cosf(b), 0.f));
    }
    m.run = data["run"].asBoolean() && !gAgent.getRunning();
    if (m.run)
    {
        gAgent.setRunning();
        gAgent.sendWalkRun(true);
    }

    m.id = sNextId++;
    m.start = LLTimer::getTotalSeconds();
    m.end = m.start + secs;
    m.start_pos = gAgent.getPositionGlobal();
    m.done = std::make_shared<LLSD>();
    sMove = m;
    sActive = true;

    LLSD started = where();
    started["id"] = (S32)m.id;
    started["secs"] = secs;
    if (!data["wait"].asBoolean())
        return replyTo(data, started);
    std::shared_ptr<LLSD> done = m.done;
    deferReply(data, secs + 5.0, [done](LLSD& reply, bool expired)
    {
        if (done->isUndefined())
            return false;
        for (const auto& [k, v] : llsd::inMap(*done))
            reply[k] = v;
        return true;
    });
}

void stopMove(const LLSD& data)
{
    bool was = sActive;
    finish("stop");
    LLSD out = was ? *sMove.done : where();
    out["was_moving"] = was;
    sendReply(out, data);
}
} // namespace ibasho
