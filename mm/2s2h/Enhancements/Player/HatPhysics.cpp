#include <libultraship/bridge/consolevariablebridge.h>
#include <libultraship/bridge/resourcebridge.h>
#include "2s2h/GameInteractor/GameInteractor.h"
#include "2s2h/ShipInit.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

extern "C" {
#include "variables.h"
#include "sys_matrix.h"
#include "objects/object_link_child/object_link_child.h"
}

#include <fast/resource/type/DisplayList.h>

#define CVAR_NAME "gEnhancements.Player.HatPhysics"
#define CVAR CVarGetInteger(CVAR_NAME, 0)
#define CVAR_STIFFNESS CVarGetFloat("gEnhancements.Player.HatPhysicsStiffness", 0.15f)
#define CVAR_GRAVITY CVarGetFloat("gEnhancements.Player.HatPhysicsGravity", 1.0f)
#define CVAR_DAMPING CVarGetFloat("gEnhancements.Player.HatPhysicsDamping", 0.85f)

// Bendable hat for human Link. The hat is a chain of joints that lags behind Link's head, droops with gravity and
// springs back towards where the hat would normally be. The vanilla hat model is bent to follow the chain by moving
// its vertices, so no new model is needed.
//
// The hat model points along the X axis of its limb. The chain has NUM_JOINTS joints spread along that axis: joint 0
// is the base (fixed to the head) and the last one is the tip. A vertex is influenced by the two joints on either
// side of it, depending on where it is along the axis.

#define NUM_JOINTS 4
#define HAT_DL_PATH "objects/object_link_child/gLinkHumanHatDL"

// Where the joints are along the hat, as fractions of the hat's length (measured from the base of the model).
static const f32 kJointFractions[NUM_JOINTS] = { 0.16f, 0.44f, 0.72f, 1.0f };
// How much of the spring is applied to each joint, the tip is the floppiest.
static const f32 kStiffnessScale[NUM_JOINTS] = { 0.0f, 1.0f, 0.7f, 0.5f };
// A segment can't bend further than this from where it rests.
static const f32 kMaxBendCos = 0.5f; // 60 degrees

struct HatSkinning {
    s32 joint0;
    s32 joint1;
    f32 weight1; // weight of joint1, joint0 gets 1 - weight1
};

struct HatVertexBlock {
    size_t instructionIndex; // the vertex command in the display list
    std::vector<Vtx> vertices;
    std::vector<HatSkinning> skinning;
};

static struct {
    bool tried = false;
    bool ok = false;

    std::shared_ptr<Fast::DisplayList> resource; // keeps the model loaded
    const Gfx* resourcePointer = nullptr;
    std::vector<Gfx> instructions;
    std::vector<HatVertexBlock> blocks;
    Vec3f restJoints[NUM_JOINTS]; // in the limb's space

    bool simulationReady = false;
    u32 lastFrame = 0xFFFFFFFF;
    Vec3f pos[NUM_JOINTS];
    Vec3f prevPos[NUM_JOINTS];
    Vec3f localJoints[NUM_JOINTS];
} sHat;

static Vec3f Add(const Vec3f& a, const Vec3f& b) {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}
static Vec3f Sub(const Vec3f& a, const Vec3f& b) {
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
static Vec3f Scale(const Vec3f& a, f32 s) {
    return { a.x * s, a.y * s, a.z * s };
}
static f32 Dot(const Vec3f& a, const Vec3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static Vec3f Cross(const Vec3f& a, const Vec3f& b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
static f32 Length(const Vec3f& a) {
    return sqrtf(Dot(a, a));
}
static Vec3f Normalize(const Vec3f& a, const Vec3f& fallback) {
    f32 len = Length(a);
    return len > 0.0001f ? Scale(a, 1.0f / len) : fallback;
}

// Rotates `v` by the smallest rotation that takes the X axis to `to`.
static Vec3f RotateFromXAxis(const Vec3f& to, const Vec3f& v) {
    f32 c = to.x;
    if (c < -0.9999f) {
        return { -v.x, v.y, -v.z }; // half a turn around Y
    }
    Vec3f a = { 0.0f, -to.z, to.y }; // X axis cross `to`
    Vec3f axv = Cross(a, v);
    Vec3f axaxv = Cross(a, axv);
    return Add(Add(v, axv), Scale(axaxv, 1.0f / (1.0f + c)));
}

static bool LoadHatModel() {
    sHat.tried = true;

    auto res = ResourceLoad<Fast::DisplayList>(HAT_DL_PATH);
    if (res == nullptr || res->Instructions.empty()) {
        return false;
    }

    sHat.resource = res;
    sHat.resourcePointer = &res->Instructions[0];
    sHat.instructions = res->Instructions;
    sHat.blocks.clear();

    f32 minX = 1e9f;
    f32 maxX = -1e9f;

    for (size_t i = 0; i + 1 < sHat.instructions.size(); i++) {
        const Gfx& cmd = sHat.instructions[i];
        if ((cmd.words.w0 >> 24) != G_VTX_OTR_HASH) {
            continue;
        }

        // A vertex command takes two words: the second one holds the hash of the file with the vertices, and the first
        // one the number of vertices and an offset into that file (or, once the game has drawn it, a pointer).
        uintptr_t offset = cmd.words.w1;
        const Gfx& hashWord = sHat.instructions[i + 1];
        uint64_t hash = ((uint64_t)hashWord.words.w0 << 32) + hashWord.words.w1;
        s32 count = (cmd.words.w0 >> 12) & 0xFF;

        const Vtx* source;
        if (offset > 0xFFFFF) {
            source = (const Vtx*)offset;
        } else {
            const char* base = (const char*)ResourceGetDataByCrc(hash);
            if (base == nullptr) {
                return false;
            }
            source = (const Vtx*)(base + offset);
        }

        HatVertexBlock block;
        block.instructionIndex = i;
        block.vertices.assign(source, source + count);
        for (const Vtx& v : block.vertices) {
            minX = std::min(minX, (f32)v.v.ob[0]);
            maxX = std::max(maxX, (f32)v.v.ob[0]);
        }
        sHat.blocks.push_back(std::move(block));
        i++; // skip the hash word
    }

    if (sHat.blocks.empty() || maxX - minX < 1.0f) {
        return false;
    }

    // The hat's axis runs through the middle of the vertices that are not at the base.
    f32 length = maxX - minX;
    f32 sumY = 0.0f;
    f32 sumZ = 0.0f;
    s32 num = 0;
    for (const HatVertexBlock& block : sHat.blocks) {
        for (const Vtx& v : block.vertices) {
            if (v.v.ob[0] > minX + 0.3f * length) {
                sumY += v.v.ob[1];
                sumZ += v.v.ob[2];
                num++;
            }
        }
    }
    f32 axisY = num > 0 ? sumY / num : 0.0f;
    f32 axisZ = num > 0 ? sumZ / num : 0.0f;

    for (s32 k = 0; k < NUM_JOINTS; k++) {
        sHat.restJoints[k] = { minX + kJointFractions[k] * length, axisY, axisZ };
    }

    // Work out which joints influence each vertex.
    for (HatVertexBlock& block : sHat.blocks) {
        block.skinning.resize(block.vertices.size());
        for (size_t v = 0; v < block.vertices.size(); v++) {
            f32 x = block.vertices[v].v.ob[0];
            HatSkinning& skin = block.skinning[v];

            if (x <= sHat.restJoints[0].x) {
                skin = { 0, 0, 0.0f };
            } else if (x >= sHat.restJoints[NUM_JOINTS - 1].x) {
                skin = { NUM_JOINTS - 1, NUM_JOINTS - 1, 0.0f };
            } else {
                for (s32 k = 0; k < NUM_JOINTS - 1; k++) {
                    if (x <= sHat.restJoints[k + 1].x) {
                        f32 span = sHat.restJoints[k + 1].x - sHat.restJoints[k].x;
                        skin = { k, k + 1, (x - sHat.restJoints[k].x) / span };
                        break;
                    }
                }
            }
        }
    }

    return true;
}

static bool IsHatDisplayList(const Gfx* dList) {
    if (dList == nullptr) {
        return false;
    }
    if (dList == sHat.resourcePointer) {
        return true;
    }
    // Display lists are often referred to by the path of their file.
    const char* asString = (const char*)dList;
    return strncmp(asString, "__OTR__", 7) == 0 && strcmp(asString, gLinkHumanHatDL) == 0;
}

// Moves the chain, in world space. `rest` are the joints where they would be if the hat did not move.
static void StepSimulation(Player* player, const Vec3f rest[NUM_JOINTS]) {
    f32 segmentLength[NUM_JOINTS];
    f32 total = 0.0f;
    for (s32 k = 1; k < NUM_JOINTS; k++) {
        segmentLength[k] = Length(Sub(rest[k], rest[k - 1]));
        total += segmentLength[k];
    }

    // Start over when the hat is far from where it should be, e.g. after a scene change.
    if (!sHat.simulationReady || Length(Sub(sHat.pos[NUM_JOINTS - 1], rest[NUM_JOINTS - 1])) > total * 3.0f) {
        for (s32 k = 0; k < NUM_JOINTS; k++) {
            sHat.pos[k] = rest[k];
            sHat.prevPos[k] = rest[k];
        }
        sHat.simulationReady = true;
    }

    f32 stiffness = std::clamp(CVAR_STIFFNESS, 0.0f, 1.0f);
    f32 damping = std::clamp(CVAR_DAMPING, 0.0f, 1.0f);
    f32 gravity = CVAR_GRAVITY * 0.02f * total;

    sHat.pos[0] = rest[0];
    sHat.prevPos[0] = rest[0];

    for (s32 k = 1; k < NUM_JOINTS; k++) {
        Vec3f velocity = Scale(Sub(sHat.pos[k], sHat.prevPos[k]), damping);
        sHat.prevPos[k] = sHat.pos[k];
        sHat.pos[k] = Add(sHat.pos[k], velocity);
        sHat.pos[k].y -= gravity;

        // Spring back towards where the hat rests
        sHat.pos[k] = Add(sHat.pos[k], Scale(Sub(rest[k], sHat.pos[k]), stiffness * kStiffnessScale[k]));
    }

    // Body parts the hat should not go through
    static const PlayerBodyPart kSolidParts[] = {
        PLAYER_BODYPART_TORSO,
        PLAYER_BODYPART_LEFT_SHOULDER,
        PLAYER_BODYPART_RIGHT_SHOULDER,
    };
    f32 solidRadius = total * 0.28f;
    f32 floorY = player->actor.floorHeight;

    for (s32 iteration = 0; iteration < 2; iteration++) {
        for (s32 k = 1; k < NUM_JOINTS; k++) {
            if (k >= 2) {
                for (PlayerBodyPart part : kSolidParts) {
                    Vec3f away = Sub(sHat.pos[k], player->bodyPartsPos[part]);
                    f32 distance = Length(away);
                    if (distance < solidRadius && distance > 0.0001f) {
                        sHat.pos[k] = Add(player->bodyPartsPos[part], Scale(away, solidRadius / distance));
                    }
                }
                sHat.pos[k].y = std::max(sHat.pos[k].y, floorY + 1.0f);
            }

            // Keep the segment the same length, and not bent too far from where it rests
            Vec3f restDirection = Normalize(Sub(rest[k], rest[k - 1]), { 1.0f, 0.0f, 0.0f });
            Vec3f direction = Normalize(Sub(sHat.pos[k], sHat.pos[k - 1]), restDirection);
            f32 cosine = Dot(direction, restDirection);
            if (cosine < kMaxBendCos) {
                Vec3f sideways = Normalize(Sub(direction, Scale(restDirection, cosine)), restDirection);
                f32 sine = sqrtf(1.0f - kMaxBendCos * kMaxBendCos);
                direction = Add(Scale(restDirection, kMaxBendCos), Scale(sideways, sine));
            }
            sHat.pos[k] = Add(sHat.pos[k - 1], Scale(direction, segmentLength[k]));
        }
    }
}

// Builds a copy of the hat's display list with its vertices bent to follow the chain, and returns it.
static Gfx* BuildHatDisplayList(PlayState* play, Player* player, Vec3f* limbPos, Vec3s* limbRot) {
    // The matrix of the hat limb is not applied yet, work out where the limb is with the joint transform.
    Vec3f origin, xTip, yTip, zTip;
    Vec3f rest[NUM_JOINTS];
    const Vec3f zero = { 0.0f, 0.0f, 0.0f };
    Vec3f unitX = { 1.0f, 0.0f, 0.0f };
    Vec3f unitY = { 0.0f, 1.0f, 0.0f };
    Vec3f unitZ = { 0.0f, 0.0f, 1.0f };

    Matrix_Push();
    Matrix_TranslateRotateZYX(limbPos, limbRot);
    Vec3f zeroCopy = zero;
    Matrix_MultVec3f(&zeroCopy, &origin);
    Matrix_MultVec3f(&unitX, &xTip);
    Matrix_MultVec3f(&unitY, &yTip);
    Matrix_MultVec3f(&unitZ, &zTip);
    for (s32 k = 0; k < NUM_JOINTS; k++) {
        Vec3f joint = sHat.restJoints[k];
        Matrix_MultVec3f(&joint, &rest[k]);
    }
    Matrix_Pop();

    // The limb's axes in world space, these include the scale of Link.
    Vec3f axisX = Sub(xTip, origin);
    Vec3f axisY = Sub(yTip, origin);
    Vec3f axisZ = Sub(zTip, origin);
    f32 lengthSqX = std::max(Dot(axisX, axisX), 0.000001f);
    f32 lengthSqY = std::max(Dot(axisY, axisY), 0.000001f);
    f32 lengthSqZ = std::max(Dot(axisZ, axisZ), 0.000001f);

    // Only move the hat once per frame, this may be called more than once
    if (play->gameplayFrames != sHat.lastFrame) {
        sHat.lastFrame = play->gameplayFrames;
        StepSimulation(player, rest);
    }

    // Back to the limb's space
    for (s32 k = 0; k < NUM_JOINTS; k++) {
        Vec3f fromOrigin = Sub(sHat.pos[k], origin);
        sHat.localJoints[k] = { Dot(fromOrigin, axisX) / lengthSqX, Dot(fromOrigin, axisY) / lengthSqY,
                                Dot(fromOrigin, axisZ) / lengthSqZ };
    }

    // The direction each joint points in, halfway between the segments on both sides of it
    Vec3f segmentDirection[NUM_JOINTS - 1];
    for (s32 k = 0; k < NUM_JOINTS - 1; k++) {
        segmentDirection[k] = Normalize(Sub(sHat.localJoints[k + 1], sHat.localJoints[k]), { 1.0f, 0.0f, 0.0f });
    }
    Vec3f jointDirection[NUM_JOINTS];
    jointDirection[0] = { 1.0f, 0.0f, 0.0f }; // the base does not move
    for (s32 k = 1; k < NUM_JOINTS - 1; k++) {
        jointDirection[k] = Normalize(Add(segmentDirection[k - 1], segmentDirection[k]), segmentDirection[k]);
    }
    jointDirection[NUM_JOINTS - 1] = segmentDirection[NUM_JOINTS - 2];

    GraphicsContext* gfxCtx = play->state.gfxCtx;
    Gfx* displayList = (Gfx*)GRAPH_ALLOC(gfxCtx, sHat.instructions.size() * sizeof(Gfx));
    memcpy(displayList, sHat.instructions.data(), sHat.instructions.size() * sizeof(Gfx));

    for (const HatVertexBlock& block : sHat.blocks) {
        Vtx* bent = (Vtx*)GRAPH_ALLOC(gfxCtx, block.vertices.size() * sizeof(Vtx));

        for (size_t v = 0; v < block.vertices.size(); v++) {
            const Vtx& source = block.vertices[v];
            const HatSkinning& skin = block.skinning[v];
            Vec3f position = { (f32)source.v.ob[0], (f32)source.v.ob[1], (f32)source.v.ob[2] };
            Vec3f normal = { (f32)source.n.n[0], (f32)source.n.n[1], (f32)source.n.n[2] };
            Vec3f newPosition = { 0.0f, 0.0f, 0.0f };
            Vec3f newNormal = { 0.0f, 0.0f, 0.0f };

            for (s32 i = 0; i < 2; i++) {
                s32 joint = i == 0 ? skin.joint0 : skin.joint1;
                f32 weight = i == 0 ? 1.0f - skin.weight1 : skin.weight1;
                if (weight <= 0.0f) {
                    continue;
                }

                Vec3f offset = Sub(position, sHat.restJoints[joint]);
                Vec3f moved = Add(sHat.localJoints[joint], RotateFromXAxis(jointDirection[joint], offset));
                newPosition = Add(newPosition, Scale(moved, weight));
                newNormal = Add(newNormal, Scale(RotateFromXAxis(jointDirection[joint], normal), weight));
            }

            bent[v] = source;
            bent[v].v.ob[0] = (s16)std::clamp(newPosition.x, -32000.0f, 32000.0f);
            bent[v].v.ob[1] = (s16)std::clamp(newPosition.y, -32000.0f, 32000.0f);
            bent[v].v.ob[2] = (s16)std::clamp(newPosition.z, -32000.0f, 32000.0f);

            f32 normalLength = Length(newNormal);
            if (normalLength > 0.0001f) {
                f32 scale = Length(normal) / normalLength;
                bent[v].n.n[0] = (s8)std::clamp(newNormal.x * scale, -127.0f, 127.0f);
                bent[v].n.n[1] = (s8)std::clamp(newNormal.y * scale, -127.0f, 127.0f);
                bent[v].n.n[2] = (s8)std::clamp(newNormal.z * scale, -127.0f, 127.0f);
            }
        }

        // A real pointer in place of the offset makes the game use these vertices directly
        displayList[block.instructionIndex].words.w1 = (uintptr_t)bent;
    }

    return displayList;
}

void RegisterHatPhysics() {
    COND_VB_SHOULD(VB_OVERRIDE_PLAYER_HAT_DL, CVAR, {
        Player* player = va_arg(args, Player*);
        PlayState* play = va_arg(args, PlayState*);
        Gfx** dList = va_arg(args, Gfx**);
        Vec3f* pos = va_arg(args, Vec3f*);
        Vec3s* rot = va_arg(args, Vec3s*);

        if (player->actor.id != ACTOR_PLAYER || player->transformation != PLAYER_FORM_HUMAN) {
            return;
        }

        if (!sHat.tried) {
            sHat.ok = LoadHatModel();
        }
        if (!sHat.ok || !IsHatDisplayList(*dList)) {
            return;
        }

        *dList = BuildHatDisplayList(play, player, pos, rot);
    });
}

static RegisterShipInitFunc initFunc(RegisterHatPhysics, { CVAR_NAME });
