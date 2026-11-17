// The game's real camera for DLSS-G.
//
// A ScriptHook script reads the final rendered camera (position, rotation, field of view) once per
// game tick. The render thread turns the two most recent distinct readings it has used into the
// matrices Streamline asks for. Everything is computed in double precision relative to the current
// camera position: world coordinates reach several thousand units and single floats lose the small
// frame-to-frame motion.
//
// Conventions. Game world: right-handed, Z up; rotation is (pitch, roll, yaw) in degrees applied as
// Rz(yaw) * Rx(pitch) * Ry(roll), camera looking along +Y. Streamline: row-major matrices used with
// row vectors (v * M), view space x right / y up / z forward, unjittered projection.

#include "camera.h"

#include <cmath>
#include <mutex>

#include "config.h"
#include "log.h"
#include "scripthook.h"

namespace fg {
namespace {

constexpr UINT64 kGetFinalRenderedCamCoord = 0x5352E025EC2B416F;
constexpr UINT64 kGetFinalRenderedCamRot = 0x602685BD85DD26CA;
constexpr UINT64 kGetFinalRenderedCamFov = 0x04AF77971E508F6A;
constexpr double kPi = 3.14159265358979323846;
constexpr ULONGLONG kStaleAfterMs = 250;  // the script stopped ticking (loading, pause): readings are old

struct Sample {
    double pos[3]{};
    double rot[3]{};  // pitch, roll, yaw in degrees
    double fov{};     // vertical, degrees
    unsigned tick{};  // script tick the reading was taken on; 0 = none
    ULONGLONG time{};
};

constexpr int kHistory = 8;
std::mutex g_mutex;
Sample g_history[kHistory];  // ring, newest at g_newest
int g_newest = 0;
unsigned g_ticks = 0;

// Render-thread state.
Sample g_previous{};  // the reading the previous frame was built from
unsigned g_cuts = 0;       // camera cuts detected (history reset)
constexpr double kCutDistance = 20.0;  // metres within one tick
constexpr double kCutFacing = 0.5;     // cosine of the turn within one tick (60 degrees)
unsigned g_resyncs = 0;    // frames that could not take the next reading in order
unsigned g_tickStep[4]{};  // how many script ticks passed between consecutive frames: 0, 1, 2, 3+

void ScriptMain() {
    Log("Camera script running");
    for (;;) {
        const NativeVector3 pos = CallNative<NativeVector3>(kGetFinalRenderedCamCoord);
        const NativeVector3 rot = CallNative<NativeVector3>(kGetFinalRenderedCamRot, 2);
        const float fov = CallNative<float>(kGetFinalRenderedCamFov);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_newest = (g_newest + 1) % kHistory;
            Sample& s = g_history[g_newest];
            s.pos[0] = pos.x, s.pos[1] = pos.y, s.pos[2] = pos.z;
            s.rot[0] = rot.x, s.rot[1] = rot.y, s.rot[2] = rot.z;
            s.fov = fov;
            s.tick = ++g_ticks;
            s.time = GetTickCount64();
        }
        scriptWait(0);
    }
}

struct Mat4 {
    double m[4][4]{};
};

Mat4 Mul(const Mat4& a, const Mat4& b) {
    Mat4 r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

struct Basis {
    double right[3], up[3], fwd[3];
};

Basis BasisOf(const Sample& s) {
    const double p = s.rot[0] * kPi / 180.0, r = s.rot[1] * kPi / 180.0, y = s.rot[2] * kPi / 180.0;
    const double cp = std::cos(p), sp = std::sin(p), cr = std::cos(r), sr = std::sin(r), cy = std::cos(y), sy = std::sin(y);
    // Columns of Rz(yaw) * Rx(pitch) * Ry(roll).
    Basis b;
    b.right[0] = cy * cr - sy * sp * sr, b.right[1] = sy * cr + cy * sp * sr, b.right[2] = -cp * sr;
    b.fwd[0] = -sy * cp, b.fwd[1] = cy * cp, b.fwd[2] = sp;
    b.up[0] = cy * sr + sy * sp * cr, b.up[1] = sy * sr - cy * sp * cr, b.up[2] = cp * cr;
    return b;
}

// View <-> world, with world measured from `origin`.
Mat4 ViewToWorld(const Sample& s, const Basis& b, const double origin[3]) {
    Mat4 m;
    for (int i = 0; i < 3; ++i) {
        m.m[0][i] = b.right[i];
        m.m[1][i] = b.up[i];
        m.m[2][i] = b.fwd[i];
        m.m[3][i] = s.pos[i] - origin[i];
    }
    m.m[3][3] = 1;
    return m;
}

Mat4 WorldToView(const Sample& s, const Basis& b, const double origin[3]) {
    Mat4 m;
    double t[3]{};
    for (int i = 0; i < 3; ++i) {
        m.m[i][0] = b.right[i];
        m.m[i][1] = b.up[i];
        m.m[i][2] = b.fwd[i];
        const double rel = s.pos[i] - origin[i];
        t[0] -= rel * b.right[i], t[1] -= rel * b.up[i], t[2] -= rel * b.fwd[i];
    }
    m.m[3][0] = t[0], m.m[3][1] = t[1], m.m[3][2] = t[2], m.m[3][3] = 1;
    return m;
}

struct Projection {
    double fx, fy, a, b;
};

Projection ProjectionOf(const Sample& s, double aspect, double nearPlane, double farPlane, bool depthInverted) {
    Projection p;
    p.fy = 1.0 / std::tan(s.fov * kPi / 360.0);
    p.fx = p.fy / aspect;
    p.a = depthInverted ? nearPlane / (nearPlane - farPlane) : farPlane / (farPlane - nearPlane);
    p.b = depthInverted ? nearPlane * farPlane / (farPlane - nearPlane) : -nearPlane * farPlane / (farPlane - nearPlane);
    return p;
}

Mat4 ViewToClip(const Projection& p) {
    Mat4 m;
    m.m[0][0] = p.fx, m.m[1][1] = p.fy, m.m[2][2] = p.a, m.m[2][3] = 1, m.m[3][2] = p.b;
    return m;
}

Mat4 ClipToView(const Projection& p) {
    Mat4 m;
    m.m[0][0] = 1.0 / p.fx, m.m[1][1] = 1.0 / p.fy, m.m[2][3] = 1.0 / p.b, m.m[3][2] = 1, m.m[3][3] = -p.a / p.b;
    return m;
}

void Store(sl::float4x4& out, const Mat4& m) {
    for (int i = 0; i < 4; ++i) {
        out.setRow(i, {static_cast<float>(m.m[i][0]), static_cast<float>(m.m[i][1]), static_cast<float>(m.m[i][2]),
                       static_cast<float>(m.m[i][3])});
    }
}

bool SameView(const Sample& a, const Sample& b) {
    for (int i = 0; i < 3; ++i) {
        if (a.pos[i] != b.pos[i] || a.rot[i] != b.rot[i]) return false;
    }
    return a.fov == b.fov;
}

}  // namespace

void CameraRegisterScript(HMODULE module) { scriptRegister(module, &ScriptMain); }

void CameraUnregisterScript(HMODULE module) { scriptUnregister(module); }

bool CameraFillConstants(sl::Constants& c, float aspect, bool depthInverted, bool reset, bool* cut) {
    if (cut) *cut = false;
    // The script ticks once per game frame, but on another thread: "the newest reading" is sometimes
    // the same one as last frame and sometimes two ahead (measured: about one frame in five). Frames
    // therefore take readings strictly in order, one tick each, staying about one tick behind the
    // newest so the next one always exists. Only when that drifts out of the history do we resync.
    Sample current;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        const unsigned newest = g_history[g_newest].tick;
        const unsigned lag = static_cast<unsigned>(Cfg().cameraDelay < 0 ? 0 : Cfg().cameraDelay > 3 ? 3 : Cfg().cameraDelay);
        unsigned wanted = g_previous.tick + 1;
        const bool inOrder = g_previous.tick != 0 && wanted <= newest && newest - wanted <= lag + 2;
        if (!inOrder) {
            wanted = newest > lag ? newest - lag : newest;
            if (g_previous.tick != 0) ++g_resyncs;
        }
        current = Sample{};
        for (const Sample& sample : g_history) {
            if (sample.tick == wanted && wanted != 0) current = sample;
        }
    }
    if (!current.tick || current.fov < 1.0 || current.fov > 179.0 || GetTickCount64() - current.time > kStaleAfterMs) {
        g_previous = Sample{};
        return false;
    }

    // First frame after a gap or a cut: no trustworthy previous camera, so no camera motion.
    bool hasPrevious = g_previous.tick != 0 && !reset;
    if (hasPrevious) {
        // A camera cut (cutscene edit, teleport, fast travel): the camera is somewhere else entirely
        // within one tick. Interpolating across it would blend two unrelated pictures.
        const Basis now = BasisOf(current), before = BasisOf(g_previous);
        const double dx = current.pos[0] - g_previous.pos[0], dy = current.pos[1] - g_previous.pos[1], dz = current.pos[2] - g_previous.pos[2];
        const double facing = now.fwd[0] * before.fwd[0] + now.fwd[1] * before.fwd[1] + now.fwd[2] * before.fwd[2];
        if (dx * dx + dy * dy + dz * dz > kCutDistance * kCutDistance || facing < kCutFacing) {
            hasPrevious = false;
            ++g_cuts;
            if (cut) *cut = true;
        }
    }
    const Sample previous = hasPrevious ? g_previous : current;
    if (hasPrevious) {
        const unsigned step = current.tick - g_previous.tick;
        ++g_tickStep[step > 3 ? 3 : step];
    }

    const double nearPlane = Cfg().cameraNear, farPlane = Cfg().cameraFar;
    const Basis basis = BasisOf(current), previousBasis = BasisOf(previous);
    const Projection projection = ProjectionOf(current, aspect, nearPlane, farPlane, depthInverted);
    const Projection previousProjection = ProjectionOf(previous, aspect, nearPlane, farPlane, depthInverted);
    const double* origin = current.pos;

    Store(c.cameraViewToClip, ViewToClip(projection));
    Store(c.clipToCameraView, ClipToView(projection));
    Mat4 identity;
    identity.m[0][0] = identity.m[1][1] = identity.m[2][2] = identity.m[3][3] = 1;
    Store(c.clipToLensClip, identity);
    if (SameView(current, previous)) {
        Store(c.clipToPrevClip, identity);
        Store(c.prevClipToClip, identity);
    } else {
        Store(c.clipToPrevClip, Mul(Mul(ClipToView(projection), ViewToWorld(current, basis, origin)),
                                    Mul(WorldToView(previous, previousBasis, origin), ViewToClip(previousProjection))));
        Store(c.prevClipToClip, Mul(Mul(ClipToView(previousProjection), ViewToWorld(previous, previousBasis, origin)),
                                    Mul(WorldToView(current, basis, origin), ViewToClip(projection))));
    }

    c.cameraPos = {static_cast<float>(current.pos[0]), static_cast<float>(current.pos[1]), static_cast<float>(current.pos[2])};
    c.cameraRight = {static_cast<float>(basis.right[0]), static_cast<float>(basis.right[1]), static_cast<float>(basis.right[2])};
    c.cameraUp = {static_cast<float>(basis.up[0]), static_cast<float>(basis.up[1]), static_cast<float>(basis.up[2])};
    c.cameraFwd = {static_cast<float>(basis.fwd[0]), static_cast<float>(basis.fwd[1]), static_cast<float>(basis.fwd[2])};
    c.cameraNear = static_cast<float>(nearPlane);
    c.cameraFar = static_cast<float>(farPlane);
    c.cameraFOV = static_cast<float>(current.fov * kPi / 180.0);
    c.cameraAspectRatio = aspect;

    static unsigned frames = 0;
    if (Cfg().debugLogging && ++frames % 600 == 1) {
        Log("Camera: pos %.2f %.2f %.2f rot %.2f %.2f %.2f fov %.2f; script ticks between frames: 0=%u 1=%u 2=%u 3+=%u, resyncs=%u, cuts=%u",
            current.pos[0], current.pos[1], current.pos[2], current.rot[0], current.rot[1], current.rot[2], current.fov,
            g_tickStep[0], g_tickStep[1], g_tickStep[2], g_tickStep[3], g_resyncs, g_cuts);
        g_resyncs = 0;
        g_cuts = 0;
        g_tickStep[0] = g_tickStep[1] = g_tickStep[2] = g_tickStep[3] = 0;
    }

    g_previous = current;
    return true;
}

}  // namespace fg
