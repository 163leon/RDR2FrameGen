// Frame bookkeeping for DLSS-G.
//
// The game renders on one thread and presents on another, and may record the next frame before the
// previous one is presented. Each DLSS evaluate therefore opens a frame (one Streamline frame token)
// and queues it; each present takes the oldest queued frame. A present with nothing queued (menus,
// loading screens, videos) gets a token of its own and counts as "no inputs".
//
// Per frame Streamline needs, all under the same token:
//   - latency markers: simulation start/end, render submit start/end, present start/end;
//   - depth and motion vectors, tagged while they are valid (copied: the game reuses them);
//   - the frame without interface (our own copy, see hud.cpp), tagged at present;
//   - the common constants, exactly once.
// Generation is switched on only while the game keeps supplying inputs, as NVIDIA's guide requires.

#include "frames.h"

#include <windows.h>

#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>

#include "camera.h"
#include "config.h"
#include "hud.h"
#include "log.h"

namespace fg {
namespace {

struct Frame {
    sl::FrameToken* token{};
    uint32_t index{};
    bool hasInputs{};
};

std::atomic<bool> g_enabled{false};
std::mutex g_queueMutex;
std::deque<Frame> g_queue;  // evaluated, not yet presented
Frame g_presenting{};       // the frame inside the current present call (present thread only)

// Present-thread state.
bool g_generating = false;      // what DLSS-G was last told
bool g_userWantsIt = true;      // toggled with the hot key
bool g_keyWasDown = false;
bool g_reflexConfigured = false;
unsigned g_framesWithInputs = 0, g_framesWithoutInputs = 0;
unsigned g_presents = 0;
unsigned g_holdOff = 0;         // presents left during which generation stays off after a quiesce

// Shared with the threads that ask for a quiesce.
std::atomic<bool> g_quiesceRequested{false};
std::atomic<unsigned> g_presentsWithoutGeneration{0};  // only ever grows
std::atomic<DWORD> g_presentThread{0};
std::atomic<bool> g_generatingNow{false};  // mirror of g_generating for other threads
constexpr unsigned kHoldOffPresents = 30;

// Generation stops on the first present that has no inputs: DLSS-G must not be asked to interpolate
// a frame it has no depth or motion vectors for. After a real break (menu, loading screen) it waits
// for steady inputs; after a hiccup of a few presents it comes back almost at once.
constexpr unsigned kFramesToSwitchOn = 30;
constexpr unsigned kFramesToResume = 4;
constexpr unsigned kShortBreak = 10;  // presents without inputs that still count as a hiccup
unsigned g_framesToSwitchOn = kFramesToSwitchOn;  // what the next switch-on needs
const sl::ViewportHandle kViewport{0};

bool NewFrame(Frame& frame) {
    static std::atomic<uint32_t> counter{0};
    frame = Frame{};
    frame.index = counter.fetch_add(1) + 1;
    const sl::Result result = slGetNewFrameToken(frame.token, &frame.index);
    if (result != sl::Result::eOk || !frame.token) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("slGetNewFrameToken failed, result=%d", static_cast<int>(result));
        }
        return false;
    }
    return true;
}

void Marker(sl::PCLMarker marker, const Frame& frame) {
    const sl::Result result = slPCLSetMarker(marker, *frame.token);
    if (result != sl::Result::eOk) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("slPCLSetMarker(%d) failed, result=%d", static_cast<int>(marker), static_cast<int>(result));
        }
    }
}

sl::float4x4 Identity() {
    sl::float4x4 m;
    m.setRow(0, {1, 0, 0, 0});
    m.setRow(1, {0, 1, 0, 0});
    m.setRow(2, {0, 0, 1, 0});
    m.setRow(3, {0, 0, 0, 1});
    return m;
}

// Fallback when the game's camera cannot be read (ScriptHook not ticking): a nominal camera with no
// camera motion. Motion then comes from the motion vectors alone.
void FillPlaceholderCamera(sl::Constants& c, float aspect, bool depthInverted) {
    constexpr float nearPlane = 0.05f, farPlane = 10000.0f;
    constexpr float fov = 1.0471975512f;  // 60 degrees, vertical
    const float focal = 1.0f / std::tan(fov * 0.5f);
    const float a = depthInverted ? nearPlane / (nearPlane - farPlane) : farPlane / (farPlane - nearPlane);
    const float b = depthInverted ? nearPlane * farPlane / (farPlane - nearPlane) : -nearPlane * farPlane / (farPlane - nearPlane);

    c.cameraViewToClip.setRow(0, {focal / aspect, 0, 0, 0});
    c.cameraViewToClip.setRow(1, {0, focal, 0, 0});
    c.cameraViewToClip.setRow(2, {0, 0, a, 1});
    c.cameraViewToClip.setRow(3, {0, 0, b, 0});
    c.clipToCameraView.setRow(0, {aspect / focal, 0, 0, 0});
    c.clipToCameraView.setRow(1, {0, 1.0f / focal, 0, 0});
    c.clipToCameraView.setRow(2, {0, 0, 0, 1.0f / b});
    c.clipToCameraView.setRow(3, {0, 0, 1, -a / b});
    c.clipToLensClip = Identity();
    c.clipToPrevClip = Identity();
    c.prevClipToClip = Identity();
    c.cameraPos = {0, 0, 0};
    c.cameraRight = {1, 0, 0};
    c.cameraUp = {0, 0, 1};
    c.cameraFwd = {0, 1, 0};
    c.cameraNear = nearPlane;
    c.cameraFar = farPlane;
    c.cameraFOV = fov;
    c.cameraAspectRatio = aspect;
}

sl::Resource DescribeImage(VkImage image, VkImageView view, VkFormat format, uint32_t width, uint32_t height, VkImageUsageFlags usage) {
    // The layout the game leaves its DLSS inputs in while the evaluate is recorded.
    sl::Resource resource(sl::ResourceType::eTex2d, image, nullptr, view, static_cast<uint32_t>(VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL));
    resource.width = width;
    resource.height = height;
    resource.nativeFormat = static_cast<uint32_t>(format);
    resource.mipLevels = 1;
    resource.arrayLayers = 1;
    resource.flags = 0;
    resource.usage = usage;
    return resource;
}

void SetGeneration(bool on) {
    sl::DLSSGOptions options{};
    options.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    options.numFramesToGenerate = 1;  // 2x
    // Without this every off/on cycle frees and re-creates all DLSS-G resources (a long hitch).
    options.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
    const sl::Result result = slDLSSGSetOptions(kViewport, options);
    Log("Frame generation %s (present %u), slDLSSGSetOptions result=%d", on ? "ON" : "OFF", g_presents, static_cast<int>(result));
    if (result == sl::Result::eOk) g_generating = on;
    g_generatingNow.store(g_generating, std::memory_order_release);
    if (on && Cfg().debugLogging) HudStartProbe(3);
}

bool GameWindowFocused() {
    DWORD process = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}

}  // namespace

void FramesEnable() { g_enabled.store(true, std::memory_order_release); }

void FrameOnDlssEvaluate(const DlssInputs& in) {
    if (!g_enabled.load(std::memory_order_acquire)) return;

    Frame frame;
    if (!NewFrame(frame)) return;
    Marker(sl::PCLMarker::eSimulationStart, frame);
    Marker(sl::PCLMarker::eSimulationEnd, frame);
    Marker(sl::PCLMarker::eRenderSubmitStart, frame);

    sl::Resource depth = DescribeImage(in.depth, in.depthView, in.depthFormat, in.depthWidth, in.depthHeight,
                                       VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    sl::Resource motion = DescribeImage(in.motion, in.motionView, in.motionFormat, in.motionWidth, in.motionHeight,
                                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    const uint32_t motionW = in.motionLowRes ? in.renderWidth : in.motionWidth;
    const uint32_t motionH = in.motionLowRes ? in.renderHeight : in.motionHeight;
    const sl::Extent depthExtent{0, 0, in.renderWidth, in.renderHeight};
    const sl::Extent motionExtent{0, 0, motionW, motionH};
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &depthExtent),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &motionExtent),
    };
    const sl::Result tagResult = slSetTagForFrame(*frame.token, kViewport, tags, 2, reinterpret_cast<sl::CommandBuffer*>(in.commandBuffer));

    sl::Constants constants{};
    const float aspect = static_cast<float>(in.renderWidth) / static_cast<float>(in.renderHeight);
    bool cameraCut = false;
    const bool realCamera = CameraFillConstants(constants, aspect, in.depthInverted, in.reset, &cameraCut);
    if (!realCamera) FillPlaceholderCamera(constants, aspect, in.depthInverted);
    static int lastCamera = -1;
    if (lastCamera != static_cast<int>(realCamera)) {
        lastCamera = realCamera;
        Log("Camera for frame generation: %s", realCamera ? "the game's camera" : "neutral fallback (no camera readings)");
    }
    constants.jitterOffset = {in.jitterX, in.jitterY};
    // NGX scale turns buffer values into pixels; Streamline wants values normalised to [-1, 1].
    constants.mvecScale = {in.motionScaleX / static_cast<float>(motionW), in.motionScaleY / static_cast<float>(motionH)};
    constants.cameraPinholeOffset = {0, 0};
    constants.depthInverted = in.depthInverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.cameraMotionIncluded = sl::Boolean::eTrue;
    constants.motionVectors3D = sl::Boolean::eFalse;
    constants.reset = in.reset || cameraCut ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    constants.orthographicProjection = sl::Boolean::eFalse;
    constants.motionVectorsDilated = sl::Boolean::eFalse;
    constants.motionVectorsJittered = in.motionJittered ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    const sl::Result constantsResult = slSetConstants(constants, *frame.token, kViewport);

    frame.hasInputs = tagResult == sl::Result::eOk && constantsResult == sl::Result::eOk;
    static unsigned evaluates = 0;
    if (++evaluates == 1 || !frame.hasInputs) {
        static unsigned failures = 0;
        if (frame.hasInputs || ++failures <= 5) {
            Log("DLSS inputs frame %u: tags result=%d constants result=%d; render %ux%u depth %ux%u fmt=%d motion %ux%u fmt=%d "
                "jitter %.3f/%.3f mvScale %.3f/%.3f depthInverted=%d mvJittered=%d mvLowRes=%d",
                frame.index, static_cast<int>(tagResult), static_cast<int>(constantsResult), in.renderWidth, in.renderHeight,
                in.depthWidth, in.depthHeight, in.depthFormat, in.motionWidth, in.motionHeight, in.motionFormat, in.jitterX,
                in.jitterY, in.motionScaleX, in.motionScaleY, in.depthInverted, in.motionJittered, in.motionLowRes);
        }
    }

    std::lock_guard<std::mutex> lock(g_queueMutex);
    g_queue.push_back(frame);
    // A frame that is never presented must not hold the queue back.
    while (g_queue.size() > 4) g_queue.pop_front();
}

// Hands DLSS-G the frame without interface, so that it interpolates the scene only and takes the
// interface from the real frame instead of smearing it.
void TagHudLess(uint32_t imageIndex) {
    static int lastState = -1;
    HudLessImage image;
    int state = 0;
    if (Cfg().hudLess && g_presenting.hasInputs && HudLessForPresent(imageIndex, image)) {
        sl::Resource resource(sl::ResourceType::eTex2d, image.image, nullptr, image.view, static_cast<uint32_t>(image.layout));
        resource.width = image.width;
        resource.height = image.height;
        resource.nativeFormat = static_cast<uint32_t>(image.format);
        resource.mipLevels = 1;
        resource.arrayLayers = 1;
        resource.flags = 0;
        resource.usage = image.usage;
        const sl::Extent extent{0, 0, image.width, image.height};
        const sl::ResourceTag tag(&resource, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &extent);
        const sl::Result result = slSetTagForFrame(*g_presenting.token, kViewport, &tag, 1, nullptr);
        state = result == sl::Result::eOk ? 1 : 2 + static_cast<int>(result);
    }
    if (state != lastState) {
        lastState = state;
        if (state == 0) Log("Frame without interface: not available for this frame (present %u)", g_presents);
        else if (state == 1) Log("Frame without interface: handed to DLSS-G (present %u)", g_presents);
        else Log("Frame without interface: slSetTagForFrame failed, result=%d", state - 2);
    }
}

void FramesQuiesce(const char* reason) {
    if (!g_enabled.load(std::memory_order_acquire)) return;
    const unsigned before = g_presentsWithoutGeneration.load(std::memory_order_acquire);
    g_quiesceRequested.store(true, std::memory_order_release);
    if (!g_generatingNow.load(std::memory_order_acquire)) {
        LogDebug("Quiesce (%s): generation is already off", reason);
        return;
    }
    if (GetCurrentThreadId() == g_presentThread.load(std::memory_order_acquire)) {
        Log("Quiesce (%s) on the present thread", reason);
        return;  // nothing presents while this thread is here; the next present switches off first
    }
    // Two presents without generation: the first switches off, the second proves Streamline's own
    // presenting thread is done. If the game is not presenting at all there is nothing to wait for.
    const ULONGLONG start = GetTickCount64();
    while (g_presentsWithoutGeneration.load(std::memory_order_acquire) - before < 2 && GetTickCount64() - start < 300) Sleep(2);
    Log("Quiesce (%s): %u presents without generation after %llu ms", reason,
        g_presentsWithoutGeneration.load(std::memory_order_acquire) - before, GetTickCount64() - start);
}

void FramePresentBegin(uint32_t imageIndex) {
    g_presenting = Frame{};
    if (!g_enabled.load(std::memory_order_acquire)) return;
    ++g_presents;
    g_presentThread.store(GetCurrentThreadId(), std::memory_order_release);
    if (g_quiesceRequested.exchange(false, std::memory_order_acq_rel)) g_holdOff = kHoldOffPresents;
    else if (g_holdOff) --g_holdOff;

    bool queued = false;
    {
        std::lock_guard<std::mutex> lock(g_queueMutex);
        if (!g_queue.empty()) {
            g_presenting = g_queue.front();
            g_queue.pop_front();
            queued = true;
        }
    }
    if (!queued) {
        if (!NewFrame(g_presenting)) return;
        Marker(sl::PCLMarker::eSimulationStart, g_presenting);
        Marker(sl::PCLMarker::eSimulationEnd, g_presenting);
        Marker(sl::PCLMarker::eRenderSubmitStart, g_presenting);
    }

    if (!g_reflexConfigured) {
        g_reflexConfigured = true;
        sl::ReflexOptions reflex{};
        reflex.mode = sl::ReflexMode::eLowLatency;  // DLSS-G refuses to run without Reflex
        const sl::Result result = slReflexSetOptions(reflex);
        Log("slReflexSetOptions(low latency) result=%d", static_cast<int>(result));
    }

    // Hot key: flips the user's wish; the gate below does the actual switching.
    const bool keyDown = (GetAsyncKeyState(Cfg().toggleKey) & 0x8000) != 0 && GameWindowFocused();
    if (keyDown && !g_keyWasDown) {
        g_userWantsIt = !g_userWantsIt;
        Log("Hot key: frame generation %s by the user", g_userWantsIt ? "allowed" : "disabled");
    }
    g_keyWasDown = keyDown;
    // Diagnostics: End writes the back-buffer activity of the next two frames to the log.
    static bool probeKeyWasDown = false;
    const bool probeKeyDown = Cfg().debugLogging && (GetAsyncKeyState(VK_END) & 0x8000) != 0 && GameWindowFocused();
    if (probeKeyDown && !probeKeyWasDown) HudStartProbe(2);
    probeKeyWasDown = probeKeyDown;

    if (g_presenting.hasInputs) {
        if (g_framesWithInputs == 0) g_framesToSwitchOn = g_framesWithoutInputs && g_framesWithoutInputs <= kShortBreak ? kFramesToResume : kFramesToSwitchOn;
        ++g_framesWithInputs;
        g_framesWithoutInputs = 0;
    } else {
        ++g_framesWithoutInputs;
        g_framesWithInputs = 0;
    }
    // In the background the game may be minimised or lose exclusive full screen at any moment.
    const bool allowed = g_userWantsIt && g_holdOff == 0 && GameWindowFocused();
    const bool shouldGenerate = allowed && g_framesWithInputs >= g_framesToSwitchOn;
    const bool mustStop = !allowed || g_framesWithoutInputs > 0;
    if (!g_generating && shouldGenerate) SetGeneration(true);
    if (g_generating && mustStop) SetGeneration(false);

    TagHudLess(imageIndex);

    Marker(sl::PCLMarker::eRenderSubmitEnd, g_presenting);
    Marker(sl::PCLMarker::ePresentStart, g_presenting);
}

void FramePresentEnd() {
    if (!g_presenting.token) return;
    Marker(sl::PCLMarker::ePresentEnd, g_presenting);
    if (!g_generating) g_presentsWithoutGeneration.fetch_add(1, std::memory_order_acq_rel);

    // Periodic health line: how many frames reached the display per game frame.
    static unsigned lastReport = 0;
    const unsigned interval = Cfg().debugLogging ? 300 : 3000;
    if (g_presents - lastReport < interval) return;
    const unsigned gameFrames = g_presents - lastReport;
    lastReport = g_presents;

    sl::DLSSGState state{};
    const sl::Result result = slDLSSGGetState(kViewport, state, nullptr);
    Log("DLSS-G: generating=%d status=0x%X %u frames shown per game frame (last present), %u game frames since last report, result=%d", g_generating,
        static_cast<unsigned>(state.status), state.numFramesActuallyPresented, gameFrames, static_cast<int>(result));
}

}  // namespace fg
