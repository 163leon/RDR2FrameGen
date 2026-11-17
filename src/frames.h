#pragma once

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

namespace fg {

// What the game hands its own DLSS upscaler for one frame; the same data feeds frame generation.
struct DlssInputs {
    VkCommandBuffer commandBuffer{};  // the game's command buffer the DLSS evaluate is recorded into
    VkImage depth{}, motion{};
    VkImageView depthView{}, motionView{};
    VkFormat depthFormat{}, motionFormat{};
    uint32_t depthWidth{}, depthHeight{}, motionWidth{}, motionHeight{};
    uint32_t renderWidth{}, renderHeight{};  // the part of the buffers that holds this frame
    float jitterX{}, jitterY{};
    float motionScaleX{}, motionScaleY{};  // turns buffer values into pixels (NGX convention)
    bool depthInverted{}, motionJittered{}, motionLowRes{}, reset{};
};

// Streamline is ready on the game's device; frames may be tracked from now on.
void FramesEnable();

// Render thread, inside the game's DLSS evaluate: starts a frame, tags depth and motion vectors
// (Streamline copies them inside the given command buffer) and publishes the frame's constants.
void FrameOnDlssEvaluate(const DlssInputs& inputs);

// Present thread, around the game's vkQueuePresentKHR. `imageIndex` is the swapchain image presented.
void FramePresentBegin(uint32_t imageIndex);
void FramePresentEnd();

// Any thread, before the game touches its window or swapchain (mode switch, resize, teardown):
// generation goes off and the call waits briefly until the present thread has shown frames without
// it. While generating, Streamline presents from its own thread, and the game's overlays do not
// survive a present that races with the window being rebuilt.
void FramesQuiesce(const char* reason);

}  // namespace fg
