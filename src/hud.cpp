// Back buffer tracking: where in a frame the game writes to the swapchain image.
//
// The game receives wrappers for a handful of device functions (it resolves them once through
// vkGet*ProcAddr). They keep track of which image views and framebuffers refer to a swapchain image
// and what is drawn into them, which is what a HUD-less capture needs to find its moment.
//
// What a frame looks like (measured, see NOTES.md): the finished scene arrives in the swapchain image
// as one full-screen triangle in a render pass that clears the image; the interface (HUD, menus, map)
// is drawn afterwards in a separate render pass that keeps the image. So right after the clearing pass
// ends, the image holds the frame without interface, and that is where it is copied.
//
// A probe (debug logging, End key) can still write the structure of a few frames to the log.

#include "hud.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "log.h"

namespace fg {
namespace {

struct Real {
    PFN_vkCreateImageView CreateImageView;
    PFN_vkCreateFramebuffer CreateFramebuffer;
    PFN_vkCreateRenderPass CreateRenderPass;
    PFN_vkCreateRenderPass2 CreateRenderPass2;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkQueueSubmit QueueSubmit;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass;
    PFN_vkCmdBeginRenderPass2 CmdBeginRenderPass2;
    PFN_vkCmdEndRenderPass CmdEndRenderPass;
    PFN_vkCmdEndRenderPass2 CmdEndRenderPass2;
    PFN_vkCmdNextSubpass CmdNextSubpass;
    PFN_vkCmdExecuteCommands CmdExecuteCommands;
    PFN_vkCmdBindPipeline CmdBindPipeline;
    PFN_vkCmdDraw CmdDraw;
    PFN_vkCmdDrawIndexed CmdDrawIndexed;
    PFN_vkCmdDrawIndirect CmdDrawIndirect;
    PFN_vkCmdDrawIndexedIndirect CmdDrawIndexedIndirect;
    PFN_vkCmdCopyImage CmdCopyImage;
    PFN_vkCmdBlitImage CmdBlitImage;
    PFN_vkCmdResolveImage CmdResolveImage;
    PFN_vkCmdClearColorImage CmdClearColorImage;
    PFN_vkCmdClearAttachments CmdClearAttachments;
};
Real g_real{};

// Our own copies of device functions, for the HUD-less copy (independent of what the game resolved).
struct Own {
    PFN_vkCreateImage CreateImage;
    PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateImageView CreateImageView;
    PFN_vkDestroyImageView DestroyImageView;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCmdCopyImage CmdCopyImage;
};
Own g_own{};

struct FramebufferInfo {
    int backBufferAttachment = -1;  // index of the attachment that is a swapchain image view
    int backBufferIndex = -1;       // which swapchain image
    uint32_t attachments = 0, width = 0, height = 0;
};
struct RenderPassInfo {
    std::vector<VkAttachmentLoadOp> loadOps;
    std::vector<VkImageLayout> finalLayouts;
    uint32_t subpasses = 0;
};

std::shared_mutex g_mutex;
std::vector<VkImage> g_backBuffers;
std::unordered_map<VkImageView, int> g_backBufferViews;            // view -> swapchain image index
std::unordered_map<VkFramebuffer, FramebufferInfo> g_framebuffers;  // only those with a swapchain image
std::unordered_map<VkRenderPass, RenderPassInfo> g_renderPasses;

// One HUD-less copy target per swapchain image.
struct Capture {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};
constexpr size_t kMaxBackBuffers = 8;
constexpr VkImageUsageFlags kCaptureUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
VkDevice g_device = VK_NULL_HANDLE;
VkFormat g_captureFormat = VK_FORMAT_UNDEFINED;
uint32_t g_captureWidth = 0, g_captureHeight = 0;
std::vector<Capture> g_captures;                 // current, by swapchain image index
std::vector<Capture> g_retired;                  // from the previous swapchain, freed one swapchain later
std::atomic<bool> g_captureFresh[kMaxBackBuffers]{};  // a copy was recorded and not yet presented
std::atomic<unsigned> g_copiesRecorded{0};

struct PipelineInfo {
    VkRenderPass renderPass = VK_NULL_HANDLE;
    bool blend = false, depthTest = false;
    int srcColor = 0, dstColor = 0, srcAlpha = 0, dstAlpha = 0;
    uint32_t writeMask = 0;
};
std::unordered_map<VkPipeline, PipelineInfo> g_pipelines;

std::unordered_map<VkCommandBuffer, VkRenderPass> g_probeCommands;  // probe only: buffers holding a back-buffer pass

std::atomic<int> g_probeFrames{0};
std::atomic<unsigned> g_present{0};

// The back-buffer render pass being recorded on this thread, if any.
struct PassState {
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    unsigned draws = 0, pipelines = 0, executes = 0, clears = 0;
    VkPipeline pipeline = VK_NULL_HANDLE;
    bool sceneOutput = false;  // the pass that clears the back buffer and writes the finished scene
    int backBufferIndex = -1;
    VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t width = 0, height = 0;
};
thread_local PassState t_pass;

bool Probing() { return g_probeFrames.load(std::memory_order_relaxed) > 0; }

int BackBufferIndexOf(VkImage image) {
    for (size_t i = 0; i < g_backBuffers.size(); ++i) {
        if (g_backBuffers[i] == image) return static_cast<int>(i);
    }
    return -1;
}

VkResult VKAPI_CALL W_CreateImageView(VkDevice device, const VkImageViewCreateInfo* info, const VkAllocationCallbacks* allocator, VkImageView* view) {
    const VkResult result = g_real.CreateImageView(device, info, allocator, view);
    if (result == VK_SUCCESS && info && view) {
        std::unique_lock lock(g_mutex);
        const int index = BackBufferIndexOf(info->image);
        if (index >= 0) {
            g_backBufferViews[*view] = index;
            Log("Back buffer %d: image view %p format=%d", index, static_cast<void*>(*view), info->format);
        } else {
            g_backBufferViews.erase(*view);  // a recycled handle
        }
    }
    return result;
}

VkResult VKAPI_CALL W_CreateFramebuffer(VkDevice device, const VkFramebufferCreateInfo* info, const VkAllocationCallbacks* allocator, VkFramebuffer* framebuffer) {
    const VkResult result = g_real.CreateFramebuffer(device, info, allocator, framebuffer);
    if (result == VK_SUCCESS && info && framebuffer) {
        std::unique_lock lock(g_mutex);
        g_framebuffers.erase(*framebuffer);
        for (uint32_t i = 0; i < info->attachmentCount && info->pAttachments; ++i) {
            const auto found = g_backBufferViews.find(info->pAttachments[i]);
            if (found == g_backBufferViews.end()) continue;
            FramebufferInfo fb;
            fb.backBufferAttachment = static_cast<int>(i);
            fb.backBufferIndex = found->second;
            fb.attachments = info->attachmentCount;
            fb.width = info->width;
            fb.height = info->height;
            g_framebuffers[*framebuffer] = fb;
            Log("Back buffer %d: framebuffer %p, %u attachments (back buffer is #%u), %ux%u", fb.backBufferIndex,
                static_cast<void*>(*framebuffer), fb.attachments, i, fb.width, fb.height);
            break;
        }
    }
    return result;
}

VkResult VKAPI_CALL W_CreateRenderPass(VkDevice device, const VkRenderPassCreateInfo* info, const VkAllocationCallbacks* allocator, VkRenderPass* renderPass) {
    const VkResult result = g_real.CreateRenderPass(device, info, allocator, renderPass);
    if (result == VK_SUCCESS && info && renderPass) {
        RenderPassInfo pass;
        pass.subpasses = info->subpassCount;
        for (uint32_t i = 0; i < info->attachmentCount; ++i) {
            pass.loadOps.push_back(info->pAttachments[i].loadOp);
            pass.finalLayouts.push_back(info->pAttachments[i].finalLayout);
        }
        std::unique_lock lock(g_mutex);
        g_renderPasses[*renderPass] = std::move(pass);
    }
    return result;
}

VkResult VKAPI_CALL W_CreateRenderPass2(VkDevice device, const VkRenderPassCreateInfo2* info, const VkAllocationCallbacks* allocator, VkRenderPass* renderPass) {
    const VkResult result = g_real.CreateRenderPass2(device, info, allocator, renderPass);
    if (result == VK_SUCCESS && info && renderPass) {
        RenderPassInfo pass;
        pass.subpasses = info->subpassCount;
        for (uint32_t i = 0; i < info->attachmentCount; ++i) {
            pass.loadOps.push_back(info->pAttachments[i].loadOp);
            pass.finalLayouts.push_back(info->pAttachments[i].finalLayout);
        }
        std::unique_lock lock(g_mutex);
        g_renderPasses[*renderPass] = std::move(pass);
    }
    return result;
}

VkResult VKAPI_CALL W_CreateGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
    const VkResult result = g_real.CreateGraphicsPipelines(device, cache, count, infos, allocator, pipelines);
    if (result == VK_SUCCESS && infos && pipelines) {
        std::unique_lock lock(g_mutex);
        for (uint32_t i = 0; i < count; ++i) {
            PipelineInfo info;
            info.renderPass = infos[i].renderPass;
            if (infos[i].pColorBlendState && infos[i].pColorBlendState->attachmentCount && infos[i].pColorBlendState->pAttachments) {
                const VkPipelineColorBlendAttachmentState& a = infos[i].pColorBlendState->pAttachments[0];
                info.blend = a.blendEnable != 0;
                info.srcColor = a.srcColorBlendFactor, info.dstColor = a.dstColorBlendFactor;
                info.srcAlpha = a.srcAlphaBlendFactor, info.dstAlpha = a.dstAlphaBlendFactor;
                info.writeMask = a.colorWriteMask;
            }
            info.depthTest = infos[i].pDepthStencilState && infos[i].pDepthStencilState->depthTestEnable;
            g_pipelines[pipelines[i]] = info;
        }
    }
    return result;
}

void OnBeginRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info) {
    if (!info) return;
    FramebufferInfo fb;
    int loadOp = -1;
    uint32_t subpasses = 0;
    VkImageLayout finalLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    {
        std::shared_lock lock(g_mutex);
        const auto found = g_framebuffers.find(info->framebuffer);
        if (found == g_framebuffers.end()) return;
        fb = found->second;
        const auto pass = g_renderPasses.find(info->renderPass);
        if (pass != g_renderPasses.end()) {
            subpasses = pass->second.subpasses;
            if (static_cast<size_t>(fb.backBufferAttachment) < pass->second.loadOps.size()) {
                loadOp = pass->second.loadOps[fb.backBufferAttachment];
                finalLayout = pass->second.finalLayouts[fb.backBufferAttachment];
            }
        }
    }
    t_pass = PassState{};
    t_pass.cmd = cmd;
    t_pass.sceneOutput = loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
    t_pass.backBufferIndex = fb.backBufferIndex;
    t_pass.finalLayout = finalLayout;
    t_pass.width = fb.width;
    t_pass.height = fb.height;
    if (Probing()) {
        {
            std::unique_lock lock(g_mutex);
            g_probeCommands[cmd] = info->renderPass;
        }
        Log("[probe %u] begin back-buffer pass: cmd=%p back buffer %d, renderPass=%p loadOp=%d (0=load 1=clear 2=dont care) subpasses=%u attachments=%u area %ux%u",
            g_present.load(), static_cast<void*>(cmd), fb.backBufferIndex, static_cast<void*>(info->renderPass), loadOp, subpasses,
            fb.attachments, info->renderArea.extent.width, info->renderArea.extent.height);
    }
}

VkImageMemoryBarrier Barrier(VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags sourceAccess, VkAccessFlags destinationAccess) {
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.srcAccessMask = sourceAccess;
    barrier.dstAccessMask = destinationAccess;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return barrier;
}

// Records, right after the scene-output pass, a copy of the back buffer into our image. The back
// buffer is returned to the layout the pass left it in, so the game's following passes see no change.
void RecordHudLessCopy(VkCommandBuffer cmd) {
    const int index = t_pass.backBufferIndex;
    VkImage source, destination;
    {
        std::shared_lock lock(g_mutex);
        if (index < 0 || static_cast<size_t>(index) >= g_captures.size() || static_cast<size_t>(index) >= g_backBuffers.size()) return;
        if (t_pass.width != g_captureWidth || t_pass.height != g_captureHeight) return;
        source = g_backBuffers[index];
        destination = g_captures[index].image;
    }
    if (!destination || t_pass.finalLayout == VK_IMAGE_LAYOUT_UNDEFINED) return;

    const VkAccessFlags attachmentAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    const VkImageMemoryBarrier before[] = {
        Barrier(source, t_pass.finalLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT),
        Barrier(destination, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT),
    };
    g_own.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                             0, nullptr, 0, nullptr, 2, before);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {t_pass.width, t_pass.height, 1};
    g_own.CmdCopyImage(cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    const VkImageMemoryBarrier after[] = {
        Barrier(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t_pass.finalLayout, VK_ACCESS_TRANSFER_READ_BIT, attachmentAccess),
        Barrier(destination, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT),
    };
    g_own.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 2, after);

    g_captureFresh[index].store(true, std::memory_order_release);
    if (g_copiesRecorded.fetch_add(1, std::memory_order_relaxed) == 0) {
        Log("HUD-less copy: first one recorded (back buffer %d, %ux%u, back buffer layout after the scene pass = %d)", index, t_pass.width,
            t_pass.height, static_cast<int>(t_pass.finalLayout));
    }
    if (Probing()) Log("[probe %u] HUD-less copy recorded after the scene pass, cmd=%p back buffer %d", g_present.load(), static_cast<void*>(cmd), index);
}

void OnEndRenderPass(VkCommandBuffer cmd) {
    if (t_pass.cmd != cmd) return;
    if (t_pass.sceneOutput) RecordHudLessCopy(cmd);
    if (Probing()) {
        Log("[probe %u] end back-buffer pass: cmd=%p draws=%u pipeline binds=%u executeCommands=%u clears=%u", g_present.load(),
            static_cast<void*>(cmd), t_pass.draws, t_pass.pipelines, t_pass.executes, t_pass.clears);
    }
    t_pass = PassState{};
}

void VKAPI_CALL W_CmdBeginRenderPass(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, VkSubpassContents contents) {
    OnBeginRenderPass(cmd, info);
    g_real.CmdBeginRenderPass(cmd, info, contents);
}

void VKAPI_CALL W_CmdBeginRenderPass2(VkCommandBuffer cmd, const VkRenderPassBeginInfo* info, const VkSubpassBeginInfo* subpass) {
    OnBeginRenderPass(cmd, info);
    g_real.CmdBeginRenderPass2(cmd, info, subpass);
}

void VKAPI_CALL W_CmdEndRenderPass(VkCommandBuffer cmd) {
    g_real.CmdEndRenderPass(cmd);
    OnEndRenderPass(cmd);
}

void VKAPI_CALL W_CmdEndRenderPass2(VkCommandBuffer cmd, const VkSubpassEndInfo* info) {
    g_real.CmdEndRenderPass2(cmd, info);
    OnEndRenderPass(cmd);
}

void VKAPI_CALL W_CmdNextSubpass(VkCommandBuffer cmd, VkSubpassContents contents) {
    if (t_pass.cmd == cmd && Probing()) Log("[probe %u] next subpass after %u draws", g_present.load(), t_pass.draws);
    g_real.CmdNextSubpass(cmd, contents);
}

VkResult VKAPI_CALL W_QueueSubmit(VkQueue queue, uint32_t submitCount, const VkSubmitInfo* submits, VkFence fence) {
    if (Probing() && submits) {
        std::unique_lock lock(g_mutex);
        for (uint32_t s = 0; s < submitCount; ++s) {
            for (uint32_t c = 0; c < submits[s].commandBufferCount; ++c) {
                const auto found = g_probeCommands.find(submits[s].pCommandBuffers[c]);
                if (found == g_probeCommands.end()) continue;
                Log("[probe %u] submitted: cmd=%p (back-buffer pass renderPass=%p), position %u of %u in the submit", g_present.load(),
                    static_cast<void*>(found->first), static_cast<void*>(found->second), c, submits[s].commandBufferCount);
                g_probeCommands.erase(found);
            }
        }
    }
    return g_real.QueueSubmit(queue, submitCount, submits, fence);
}

VkResult VKAPI_CALL W_BeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo* info) {
    if (info && info->pInheritanceInfo && (info->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) && Probing()) {
        std::shared_lock lock(g_mutex);
        if (g_framebuffers.count(info->pInheritanceInfo->framebuffer)) {
            Log("[probe %u] secondary command buffer %p continues a back-buffer pass", g_present.load(), static_cast<void*>(cmd));
        }
    }
    return g_real.BeginCommandBuffer(cmd, info);
}

void VKAPI_CALL W_CmdExecuteCommands(VkCommandBuffer cmd, uint32_t count, const VkCommandBuffer* buffers) {
    if (t_pass.cmd == cmd) {
        ++t_pass.executes;
        if (Probing()) Log("[probe %u] executeCommands x%u inside the back-buffer pass after %u draws", g_present.load(), count, t_pass.draws);
    }
    g_real.CmdExecuteCommands(cmd, count, buffers);
}

void VKAPI_CALL W_CmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bindPoint, VkPipeline pipeline) {
    if (t_pass.cmd == cmd && bindPoint == VK_PIPELINE_BIND_POINT_GRAPHICS) {
        ++t_pass.pipelines;
        t_pass.pipeline = pipeline;
    }
    g_real.CmdBindPipeline(cmd, bindPoint, pipeline);
}

void OnDraw(VkCommandBuffer cmd, const char* kind, uint32_t count, uint32_t instances) {
    if (t_pass.cmd != cmd) return;
    ++t_pass.draws;
    if (t_pass.draws <= 40 && Probing()) {
        PipelineInfo info;
        {
            std::shared_lock lock(g_mutex);
            const auto found = g_pipelines.find(t_pass.pipeline);
            if (found != g_pipelines.end()) info = found->second;
        }
        Log("[probe %u]   draw #%u %s count=%u instances=%u pipeline=%p blend=%d color %d/%d alpha %d/%d mask=0x%X depthTest=%d", g_present.load(),
            t_pass.draws, kind, count, instances, static_cast<void*>(t_pass.pipeline), info.blend, info.srcColor, info.dstColor,
            info.srcAlpha, info.dstAlpha, info.writeMask, info.depthTest);
    }
}

void VKAPI_CALL W_CmdDraw(VkCommandBuffer cmd, uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) {
    OnDraw(cmd, "draw", vertexCount, instanceCount);
    g_real.CmdDraw(cmd, vertexCount, instanceCount, firstVertex, firstInstance);
}

void VKAPI_CALL W_CmdDrawIndexed(VkCommandBuffer cmd, uint32_t indexCount, uint32_t instanceCount, uint32_t firstIndex, int32_t vertexOffset, uint32_t firstInstance) {
    OnDraw(cmd, "indexed", indexCount, instanceCount);
    g_real.CmdDrawIndexed(cmd, indexCount, instanceCount, firstIndex, vertexOffset, firstInstance);
}

void VKAPI_CALL W_CmdDrawIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
    OnDraw(cmd, "indirect", drawCount, 0);
    g_real.CmdDrawIndirect(cmd, buffer, offset, drawCount, stride);
}

void VKAPI_CALL W_CmdDrawIndexedIndirect(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset, uint32_t drawCount, uint32_t stride) {
    OnDraw(cmd, "indexed indirect", drawCount, 0);
    g_real.CmdDrawIndexedIndirect(cmd, buffer, offset, drawCount, stride);
}

void VKAPI_CALL W_CmdClearAttachments(VkCommandBuffer cmd, uint32_t attachmentCount, const VkClearAttachment* attachments, uint32_t rectCount, const VkClearRect* rects) {
    if (t_pass.cmd == cmd) ++t_pass.clears;
    g_real.CmdClearAttachments(cmd, attachmentCount, attachments, rectCount, rects);
}

void OnImageWrite(const char* what, VkCommandBuffer cmd, VkImage source, VkImage destination) {
    if (!Probing()) return;
    std::shared_lock lock(g_mutex);
    const int to = BackBufferIndexOf(destination), from = source ? BackBufferIndexOf(source) : -1;
    if (to >= 0) Log("[probe %u] %s INTO back buffer %d (cmd=%p, source=%p)", g_present.load(), what, to, static_cast<void*>(cmd), static_cast<void*>(source));
    if (from >= 0) Log("[probe %u] %s FROM back buffer %d (cmd=%p)", g_present.load(), what, from, static_cast<void*>(cmd));
}

void VKAPI_CALL W_CmdCopyImage(VkCommandBuffer cmd, VkImage source, VkImageLayout sourceLayout, VkImage destination, VkImageLayout destinationLayout, uint32_t regionCount, const VkImageCopy* regions) {
    OnImageWrite("copy", cmd, source, destination);
    g_real.CmdCopyImage(cmd, source, sourceLayout, destination, destinationLayout, regionCount, regions);
}

void VKAPI_CALL W_CmdBlitImage(VkCommandBuffer cmd, VkImage source, VkImageLayout sourceLayout, VkImage destination, VkImageLayout destinationLayout, uint32_t regionCount, const VkImageBlit* regions, VkFilter filter) {
    OnImageWrite("blit", cmd, source, destination);
    g_real.CmdBlitImage(cmd, source, sourceLayout, destination, destinationLayout, regionCount, regions, filter);
}

void VKAPI_CALL W_CmdResolveImage(VkCommandBuffer cmd, VkImage source, VkImageLayout sourceLayout, VkImage destination, VkImageLayout destinationLayout, uint32_t regionCount, const VkImageResolve* regions) {
    OnImageWrite("resolve", cmd, source, destination);
    g_real.CmdResolveImage(cmd, source, sourceLayout, destination, destinationLayout, regionCount, regions);
}

void VKAPI_CALL W_CmdClearColorImage(VkCommandBuffer cmd, VkImage image, VkImageLayout layout, const VkClearColorValue* color, uint32_t rangeCount, const VkImageSubresourceRange* ranges) {
    OnImageWrite("clear", cmd, VK_NULL_HANDLE, image);
    g_real.CmdClearColorImage(cmd, image, layout, color, rangeCount, ranges);
}

struct Wrapper {
    const char* name;
    PFN_vkVoidFunction wrapper;
    PFN_vkVoidFunction* real;
};
#define FG_WRAP(name, slot) {"vk" #name, reinterpret_cast<PFN_vkVoidFunction>(&W_##slot), reinterpret_cast<PFN_vkVoidFunction*>(&g_real.slot)}
const Wrapper kWrappers[] = {
    FG_WRAP(CreateImageView, CreateImageView),
    FG_WRAP(CreateFramebuffer, CreateFramebuffer),
    FG_WRAP(CreateRenderPass, CreateRenderPass),
    FG_WRAP(CreateRenderPass2, CreateRenderPass2),
    FG_WRAP(CreateRenderPass2KHR, CreateRenderPass2),
    FG_WRAP(CreateGraphicsPipelines, CreateGraphicsPipelines),
    FG_WRAP(BeginCommandBuffer, BeginCommandBuffer),
    FG_WRAP(QueueSubmit, QueueSubmit),
    FG_WRAP(CmdBeginRenderPass, CmdBeginRenderPass),
    FG_WRAP(CmdBeginRenderPass2, CmdBeginRenderPass2),
    FG_WRAP(CmdBeginRenderPass2KHR, CmdBeginRenderPass2),
    FG_WRAP(CmdEndRenderPass, CmdEndRenderPass),
    FG_WRAP(CmdEndRenderPass2, CmdEndRenderPass2),
    FG_WRAP(CmdEndRenderPass2KHR, CmdEndRenderPass2),
    FG_WRAP(CmdNextSubpass, CmdNextSubpass),
    FG_WRAP(CmdExecuteCommands, CmdExecuteCommands),
    FG_WRAP(CmdBindPipeline, CmdBindPipeline),
    FG_WRAP(CmdDraw, CmdDraw),
    FG_WRAP(CmdDrawIndexed, CmdDrawIndexed),
    FG_WRAP(CmdDrawIndirect, CmdDrawIndirect),
    FG_WRAP(CmdDrawIndexedIndirect, CmdDrawIndexedIndirect),
    FG_WRAP(CmdCopyImage, CmdCopyImage),
    FG_WRAP(CmdBlitImage, CmdBlitImage),
    FG_WRAP(CmdResolveImage, CmdResolveImage),
    FG_WRAP(CmdClearColorImage, CmdClearColorImage),
    FG_WRAP(CmdClearAttachments, CmdClearAttachments),
};
#undef FG_WRAP

}  // namespace

PFN_vkVoidFunction HudWrap(const char* name, PFN_vkVoidFunction real) {
    if (!real || !name) return real;
    for (const Wrapper& entry : kWrappers) {
        if (strcmp(entry.name, name) != 0) continue;
        *entry.real = real;
        LogDebug("Wrapped %s for back-buffer tracking", name);
        return entry.wrapper;
    }
    return real;
}

void HudOnSwapchainCreated(VkDevice device, const VkPhysicalDeviceMemoryProperties& memory, uint32_t queueFamilies,
                           PFN_vkGetDeviceProcAddr resolve, VkFormat format, uint32_t width, uint32_t height) {
    std::unique_lock lock(g_mutex);
#define FG_OWN(name) g_own.name = reinterpret_cast<PFN_vk##name>(resolve(device, "vk" #name))
    FG_OWN(CreateImage);
    FG_OWN(DestroyImage);
    FG_OWN(GetImageMemoryRequirements);
    FG_OWN(AllocateMemory);
    FG_OWN(FreeMemory);
    FG_OWN(BindImageMemory);
    FG_OWN(CreateImageView);
    FG_OWN(DestroyImageView);
    FG_OWN(CmdPipelineBarrier);
    FG_OWN(CmdCopyImage);
#undef FG_OWN

    // The copies of the swapchain before the previous one can no longer be in use; the previous
    // swapchain's copies may still be referenced by command buffers in flight, so they wait a turn.
    for (Capture& old : g_retired) {
        if (old.view) g_own.DestroyImageView(g_device, old.view, nullptr);
        if (old.image) g_own.DestroyImage(g_device, old.image, nullptr);
        if (old.memory) g_own.FreeMemory(g_device, old.memory, nullptr);
    }
    g_retired = std::move(g_captures);
    g_captures.clear();
    for (auto& fresh : g_captureFresh) fresh.store(false, std::memory_order_release);

    g_device = device;
    g_captureFormat = format;
    g_captureWidth = width;
    g_captureHeight = height;

    std::vector<uint32_t> families;
    for (uint32_t i = 0; i < queueFamilies; ++i) families.push_back(i);
    constexpr size_t kTargets = 3;  // the game asks for 2 images; one spare
    for (size_t i = 0; i < kTargets; ++i) {
        Capture capture;
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = format;
        image.extent = {width, height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.tiling = VK_IMAGE_TILING_OPTIMAL;
        image.usage = kCaptureUsage;
        image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        // Written on the game's queue, read on Streamline's: shared between all queue families.
        if (families.size() > 1) {
            image.sharingMode = VK_SHARING_MODE_CONCURRENT;
            image.queueFamilyIndexCount = static_cast<uint32_t>(families.size());
            image.pQueueFamilyIndices = families.data();
        }
        VkResult result = g_own.CreateImage(device, &image, nullptr, &capture.image);
        if (result == VK_SUCCESS) {
            VkMemoryRequirements requirements{};
            g_own.GetImageMemoryRequirements(device, capture.image, &requirements);
            uint32_t type = UINT32_MAX;
            for (uint32_t t = 0; t < memory.memoryTypeCount; ++t) {
                if ((requirements.memoryTypeBits & (1u << t)) && (memory.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                    type = t;
                    break;
                }
            }
            VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            allocate.allocationSize = requirements.size;
            allocate.memoryTypeIndex = type;
            result = type == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT : g_own.AllocateMemory(device, &allocate, nullptr, &capture.memory);
        }
        if (result == VK_SUCCESS) result = g_own.BindImageMemory(device, capture.image, capture.memory, 0);
        if (result == VK_SUCCESS) {
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = capture.image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = format;
            view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            result = g_own.CreateImageView(device, &view, nullptr, &capture.view);
        }
        if (result != VK_SUCCESS) {
            Log("HUD-less copy: could not create target %zu (%d); the interface will not be separated", i, result);
            if (capture.view) g_own.DestroyImageView(device, capture.view, nullptr);
            if (capture.image) g_own.DestroyImage(device, capture.image, nullptr);
            if (capture.memory) g_own.FreeMemory(device, capture.memory, nullptr);
            capture = Capture{};
        }
        g_captures.push_back(capture);
    }
    Log("HUD-less copy targets ready: %zu x %ux%u format=%d, shared between %u queue families", g_captures.size(), width, height,
        static_cast<int>(format), queueFamilies);
}

bool HudLessForPresent(uint32_t imageIndex, HudLessImage& out) {
    if (imageIndex >= kMaxBackBuffers || !g_captureFresh[imageIndex].exchange(false, std::memory_order_acq_rel)) return false;
    std::shared_lock lock(g_mutex);
    if (imageIndex >= g_captures.size() || !g_captures[imageIndex].image) return false;
    out.image = g_captures[imageIndex].image;
    out.view = g_captures[imageIndex].view;
    out.format = g_captureFormat;
    out.layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    out.usage = kCaptureUsage;
    out.width = g_captureWidth;
    out.height = g_captureHeight;
    return true;
}

void HudOnSwapchainImages(const VkImage* images, uint32_t count) {
    std::unique_lock lock(g_mutex);
    g_backBuffers.assign(images, images + count);
    g_backBufferViews.clear();
    g_framebuffers.clear();
    Log("Swapchain images handed to the game: %u", count);
}

void HudOnPresent() {
    g_present.fetch_add(1, std::memory_order_relaxed);
    if (Probing()) {
        Log("[probe %u] ---- present ----", g_present.load());
        g_probeFrames.fetch_sub(1, std::memory_order_relaxed);
    }
}

void HudStartProbe(int frames) { g_probeFrames.store(frames, std::memory_order_relaxed); }

}  // namespace fg
