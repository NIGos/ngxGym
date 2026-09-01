// ngxhost-vk -- the Vulkan half.
//
// PHASE 4b. A real Vulkan DLSS host: a moving scene into four images, sub-pixel
// jitter, motion vectors correct by construction, and a real NGX SuperSampling
// feature created and evaluated every frame through NVSDK_NGX_VULKAN_*.
//
// The dlss5-bridge add-on hooks those four entry points and mirrors whatever
// contract it reads onto a private D3D12 session. From its side there is nothing
// to distinguish this from Baldur's Gate 3 -- which is the point, because almost
// every defect found in that add-on in the last three days was on this path and
// needed a game launched and played to a particular state to see.
//
// WHAT WAS EXPECTED TO BLOCK THIS AND DID NOT: ReShade on Vulkan is an implicit
// layer, and it was assumed the per-executable list in ReShadeApps.ini gated
// attachment. Measured on 2026-09-01: the layer attaches to this executable with its
// path absent from that list. run-vk.ps1 keeps -Register anyway, for a machine where
// that turns out not to hold.
//
// The images are left in VK_IMAGE_LAYOUT_GENERAL when handed to NGX. That is what
// the add-on assumes of a game's images -- its own comment says ASSUMED, NOT
// MEASURED -- so a host that used optimal layouts would be testing a different
// premise from the one that ships.
//
//   ..\build.cmd

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <vector>

#include "contract.h"
#include "scenario.h"
#include "nvsdk_ngx_vk.h"
// NGX_DLSS_GET_OPTIMAL_SETTINGS lives in the API-agnostic helpers, not the Vulkan
// ones -- it only reads capability parameters and has no device in it.
#include "nvsdk_ngx_helpers.h"
#include "nvsdk_ngx_helpers_vk.h"

static const uint32_t kSceneVert[] =
#include "generated/scene_vert.h"
;
static const uint32_t kSceneFrag[] =
#include "generated/scene_frag.h"
;

#define VKC(x, what) do { \
    const VkResult r_ = (x); \
    if (r_ != VK_SUCCESS) { printf("FAIL: %s -> VkResult %d\n", (what), static_cast<int>(r_)); return false; } \
} while (0)

static const float kVelX = 0.37f;
static const float kVelY = 0.11f;

struct Push { float pan[2]; float jitter[2]; float inv_render[2]; float mv_texel[2]; };

struct Img
{
    VkImage        img  = VK_NULL_HANDLE;
    VkDeviceMemory mem  = VK_NULL_HANDLE;
    VkImageView    view = VK_NULL_HANDLE;
    VkFormat       fmt  = VK_FORMAT_UNDEFINED;
    uint32_t       w = 0, h = 0;
};

struct Host
{
    HWND             hwnd = nullptr;
    VkInstance       inst = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice         dev  = VK_NULL_HANDLE;
    VkQueue          queue = VK_NULL_HANDLE;
    uint32_t         qfam = 0;
    VkSurfaceKHR     surf = VK_NULL_HANDLE;
    VkSwapchainKHR   swap = VK_NULL_HANDLE;
    std::vector<VkImage> swap_imgs;
    VkExtent2D       swap_ext = {};

    VkCommandPool    pool = VK_NULL_HANDLE;
    VkCommandBuffer  cmd  = VK_NULL_HANDLE;
    // One acquire semaphore per frame in flight and one render-finished semaphore
    // PER SWAPCHAIN IMAGE. A single pair is the textbook mistake and validation
    // named it: a semaphore signalled for image 1 may still be in use by a present
    // of image 0 that has not been re-acquired.
    std::vector<VkSemaphore> acquired, rendered;
    uint32_t         sem_i = 0;
    VkFence          fence = VK_NULL_HANDLE;

    VkPipelineLayout play = VK_NULL_HANDLE;
    VkPipeline       pipe = VK_NULL_HANDLE;

    Img color, mv, depth, output;
    uint32_t out_w = 1920, out_h = 1080, rw = 0, rh = 0;
    int  quality = NVSDK_NGX_PerfQuality_Value_MaxQuality;

    NVSDK_NGX_Parameter *caps = nullptr;
    NVSDK_NGX_Parameter *p    = nullptr;
    NVSDK_NGX_Handle    *feat = nullptr;

    bool dlss_on   = true;
    bool transpose = false;
    Omit omit      = OMIT_NONE;
    int  frame = 0, evaluated = 0, delivered = 0;
};

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

static uint32_t FindMem(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp = {};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static void DestroyImg(VkDevice dev, Img *i)
{
    if (i->view != VK_NULL_HANDLE) { vkDestroyImageView(dev, i->view, nullptr); i->view = VK_NULL_HANDLE; }
    if (i->img  != VK_NULL_HANDLE) { vkDestroyImage(dev, i->img, nullptr);      i->img  = VK_NULL_HANDLE; }
    if (i->mem  != VK_NULL_HANDLE) { vkFreeMemory(dev, i->mem, nullptr);        i->mem  = VK_NULL_HANDLE; }
}

static bool MakeImg(Host &h, Img *out, uint32_t w, uint32_t hh, VkFormat fmt,
                    VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    DestroyImg(h.dev, out);
    out->fmt = fmt; out->w = w; out->h = hh;

    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = fmt;
    ici.extent = { w, hh, 1 };
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(h.dev, &ici, nullptr, &out->img), "vkCreateImage");

    VkMemoryRequirements mr = {};
    vkGetImageMemoryRequirements(h.dev, out->img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = FindMem(h.phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX) { printf("FAIL: no device-local memory type\n"); return false; }
    VKC(vkAllocateMemory(h.dev, &mai, nullptr, &out->mem), "vkAllocateMemory");
    VKC(vkBindImageMemory(h.dev, out->img, out->mem, 0), "vkBindImageMemory");

    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = out->img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange = { aspect, 0, 1, 0, 1 };
    VKC(vkCreateImageView(h.dev, &vci, nullptr, &out->view), "vkCreateImageView");
    return true;
}

static void Barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = { aspect, 0, 1, 0, 1 };
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

static NVSDK_NGX_Resource_VK AsResource(const Img &i, bool rw)
{
    VkImageSubresourceRange sr = {};
    sr.aspectMask = (i.fmt == VK_FORMAT_D32_SFLOAT) ? VK_IMAGE_ASPECT_DEPTH_BIT
                                                    : VK_IMAGE_ASPECT_COLOR_BIT;
    sr.levelCount = 1; sr.layerCount = 1;
    // A helper that FILLS a struct is fine; the ones that WRITE contract keys are
    // banned -- see contract.h.
    return NVSDK_NGX_Create_ImageView_Resource_VK(i.view, i.img, sr, i.fmt, i.w, i.h, rw);
}

// ---------------------------------------------------------------------------

static bool BuildPipeline(Host &h)
{
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    smi.codeSize = sizeof(kSceneVert); smi.pCode = kSceneVert;
    VKC(vkCreateShaderModule(h.dev, &smi, nullptr, &vs), "vkCreateShaderModule(vert)");
    smi.codeSize = sizeof(kSceneFrag); smi.pCode = kSceneFrag;
    VKC(vkCreateShaderModule(h.dev, &smi, nullptr, &fs), "vkCreateShaderModule(frag)");

    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                0, sizeof(Push) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    VKC(vkCreatePipelineLayout(h.dev, &pli, nullptr, &h.play), "vkCreatePipelineLayout");

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   stages[0].module = vs; stages[0].pName = "main";
    stages[1] = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; stages[1].module = fs; stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendAttachmentState cba[2] = {};
    cba[0].colorWriteMask = 0xF; cba[1].colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 2; cb.pAttachments = cba;
    const VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dsi.dynamicStateCount = 2; dsi.pDynamicStates = dyn;

    // Dynamic rendering: no render pass, no framebuffer, and no object that has to be
    // rebuilt every time the render size changes.
    const VkFormat colfmt[2] = { h.color.fmt, h.mv.fmt };
    VkPipelineRenderingCreateInfo pri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
    pri.colorAttachmentCount = 2; pri.pColorAttachmentFormats = colfmt;
    pri.depthAttachmentFormat = h.depth.fmt;

    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    gp.pNext = &pri;
    gp.stageCount = 2; gp.pStages = stages;
    gp.pVertexInputState = &vi; gp.pInputAssemblyState = &ia; gp.pViewportState = &vp;
    gp.pRasterizationState = &rs; gp.pMultisampleState = &ms; gp.pDepthStencilState = &ds;
    gp.pColorBlendState = &cb; gp.pDynamicState = &dsi; gp.layout = h.play;
    VKC(vkCreateGraphicsPipelines(h.dev, VK_NULL_HANDLE, 1, &gp, nullptr, &h.pipe),
        "vkCreateGraphicsPipelines");

    vkDestroyShaderModule(h.dev, vs, nullptr);
    vkDestroyShaderModule(h.dev, fs, nullptr);
    return true;
}

static bool OneShot(Host &h, VkCommandBuffer *out)
{
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = h.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VKC(vkAllocateCommandBuffers(h.dev, &ai, out), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(*out, &bi), "vkBeginCommandBuffer");
    return true;
}

static bool Flush(Host &h, VkCommandBuffer cmd)
{
    VKC(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    VKC(vkQueueSubmit(h.queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
    VKC(vkQueueWaitIdle(h.queue), "vkQueueWaitIdle");
    vkFreeCommandBuffers(h.dev, h.pool, 1, &cmd);
    return true;
}

static void ReleaseFeat(Host &h)
{
    if (h.feat != nullptr) { NVSDK_NGX_VULKAN_ReleaseFeature(h.feat); h.feat = nullptr; }
}

static unsigned int HostFlags() { return NVSDK_NGX_DLSS_Feature_Flags_MVLowRes; }

static bool Rebuild(Host &h, const char *why)
{
    vkDeviceWaitIdle(h.dev);

    unsigned int maxw = 0, maxh = 0, minw = 0, minh = 0;
    float sharp = 0.0f;
    NGX_DLSS_GET_OPTIMAL_SETTINGS(h.caps, h.out_w, h.out_h,
                                  static_cast<NVSDK_NGX_PerfQuality_Value>(h.quality),
                                  &h.rw, &h.rh, &maxw, &maxh, &minw, &minh, &sharp);
    if (h.rw == 0 || h.rh == 0) { h.rw = h.out_w; h.rh = h.out_h; }
    printf("  rebuild (%s): %ux%u -> %ux%u, quality %d\n", why, h.rw, h.rh, h.out_w, h.out_h, h.quality);

    const VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                 VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!MakeImg(h, &h.color,  h.rw, h.rh, VK_FORMAT_R16G16B16A16_SFLOAT, cu, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!MakeImg(h, &h.mv,     h.rw, h.rh, VK_FORMAT_R16G16_SFLOAT,       cu, VK_IMAGE_ASPECT_COLOR_BIT)) return false;
    if (!MakeImg(h, &h.depth,  h.rw, h.rh, VK_FORMAT_D32_SFLOAT,
                 VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                 VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_DEPTH_BIT)) return false;
    if (!MakeImg(h, &h.output, h.out_w, h.out_h, VK_FORMAT_R16G16B16A16_SFLOAT, cu, VK_IMAGE_ASPECT_COLOR_BIT)) return false;

    // Into GENERAL once, and left there. See the note at the top of this file: it is
    // the layout the add-on assumes of a game's images, and using a different one
    // would test a premise the shipped code does not make.
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    if (!OneShot(h, &cmd)) return false;
    Barrier(cmd, h.color.img,  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    Barrier(cmd, h.mv.img,     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    Barrier(cmd, h.output.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    Barrier(cmd, h.depth.img,  VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (!Flush(h, cmd)) return false;

    if (h.pipe != VK_NULL_HANDLE) { vkDestroyPipeline(h.dev, h.pipe, nullptr); h.pipe = VK_NULL_HANDLE; }
    if (h.play != VK_NULL_HANDLE) { vkDestroyPipelineLayout(h.dev, h.play, nullptr); h.play = VK_NULL_HANDLE; }
    if (!BuildPipeline(h)) return false;

    ReleaseFeat(h);

    CreateContract cc = {};
    SetU(&cc.width, h.rw); SetU(&cc.height, h.rh);
    SetU(&cc.out_width, h.out_w); SetU(&cc.out_height, h.out_h);
    SetU(&cc.perf_quality, static_cast<unsigned int>(h.quality));
    if (h.omit != OMIT_FLAGS) SetU(&cc.create_flags, HostFlags());
    SetU(&cc.output_subrects, 0);

    h.p->Reset();
    ApplyCreate(h.p, cc);

    VkCommandBuffer ccmd = VK_NULL_HANDLE;
    if (!OneShot(h, &ccmd)) return false;
    const NVSDK_NGX_Result r =
        NVSDK_NGX_VULKAN_CreateFeature(ccmd, NVSDK_NGX_Feature_SuperSampling, h.p, &h.feat);
    if (!Flush(h, ccmd)) return false;
    if (NVSDK_NGX_FAILED(r) || h.feat == nullptr)
    { printf("FAIL: VULKAN_CreateFeature -> 0x%08X\n", r); return false; }
    return true;
}

static bool RenderFrame(Host &h)
{
    const float jx = Halton((h.frame % 32) + 1, 2) - 0.5f;
    const float jy = Halton((h.frame % 32) + 1, 3) - 0.5f;
    const float mvsx = -static_cast<float>(h.rw), mvsy = -static_cast<float>(h.rh);

    vkWaitForFences(h.dev, 1, &h.fence, VK_TRUE, UINT64_MAX);

    uint32_t idx = 0;
    const VkSemaphore acq = h.acquired[h.sem_i];
    h.sem_i = (h.sem_i + 1) % static_cast<uint32_t>(h.acquired.size());
    const VkResult ar = vkAcquireNextImageKHR(h.dev, h.swap, UINT64_MAX, acq, VK_NULL_HANDLE, &idx);
    // The fence is reset AFTER the acquire can bail. Resetting first and then
    // returning without submitting left nothing to signal it, and the next frame
    // waited UINT64_MAX on a fence that would never come -- a hang with no verdict
    // and no timeout, reachable by minimising the window during a long scenario.
    //
    // SUBOPTIMAL proceeds: the image is valid and acq was signalled. There is
    // deliberately no swapchain-rebuild path -- the swapchain here is decorative,
    // DLSS evaluates into h.output and the blit exists so a human can watch.
    if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR)
    {
        printf("FAIL: vkAcquireNextImageKHR -> %d (do not resize or minimise the window)\n",
               static_cast<int>(ar));
        return false;
    }
    vkResetFences(h.dev, 1, &h.fence);

    vkResetCommandBuffer(h.cmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(h.cmd, &bi);

    VkRenderingAttachmentInfo ca[2] = {};
    ca[0] = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    ca[0].imageView = h.color.view; ca[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ca[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca[1] = ca[0]; ca[1].imageView = h.mv.view;
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO };
    da.imageView = h.depth.view; da.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.clearValue.depthStencil.depth = 1.0f;

    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO };
    ri.renderArea = { { 0, 0 }, { h.rw, h.rh } };
    ri.layerCount = 1; ri.colorAttachmentCount = 2; ri.pColorAttachments = ca;
    ri.pDepthAttachment = &da;

    vkCmdBeginRendering(h.cmd, &ri);
    VkViewport vp = { 0, 0, static_cast<float>(h.rw), static_cast<float>(h.rh), 0, 1 };
    VkRect2D sc = { { 0, 0 }, { h.rw, h.rh } };
    vkCmdSetViewport(h.cmd, 0, 1, &vp);
    vkCmdSetScissor(h.cmd, 0, 1, &sc);
    vkCmdBindPipeline(h.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, h.pipe);
    Push pc = {};
    pc.pan[0] = static_cast<float>(h.frame) * kVelX;
    pc.pan[1] = static_cast<float>(h.frame) * kVelY;
    pc.jitter[0] = 2.0f * jx / static_cast<float>(h.rw);
    pc.jitter[1] = 2.0f * jy / static_cast<float>(h.rh);
    pc.inv_render[0] = 1.0f / static_cast<float>(h.rw);
    pc.inv_render[1] = 1.0f / static_cast<float>(h.rh);
    pc.mv_texel[0] = kVelX / mvsx;
    pc.mv_texel[1] = kVelY / mvsy;
    vkCmdPushConstants(h.cmd, h.play, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDraw(h.cmd, 3, 1, 0, 0);
    vkCmdEndRendering(h.cmd);

    if (h.dlss_on && h.feat != nullptr)
    {
        EvalContract ec = {};
        if (h.omit != OMIT_JITTER)  { SetF(&ec.jitter_x, jx);     SetF(&ec.jitter_y, jy); }
        if (h.omit != OMIT_MVSCALE) { SetF(&ec.mv_scale_x, mvsx); SetF(&ec.mv_scale_y, mvsy); }
        SetF(&ec.sharpness, 0.0f);
        SetF(&ec.pre_exposure, 1.0f);
        SetU(&ec.reset, h.frame == 0 ? 1u : 0u);
        SetU(&ec.subrect_w, h.rw); SetU(&ec.subrect_h, h.rh);
        if (h.omit != OMIT_FLAGS) SetU(&ec.create_flags, HostFlags());
        if (h.omit != OMIT_QUALITY) SetU(&ec.perf_quality, static_cast<unsigned int>(h.quality));

        h.p->Reset();
        ApplyEval(h.p, ec);
        if (h.transpose)
        {
            h.p->Set("Width",     h.out_w); h.p->Set("Height",    h.out_h);
            h.p->Set("OutWidth",  h.rw);    h.p->Set("OutHeight", h.rh);
        }

        NVSDK_NGX_Resource_VK rc = AsResource(h.color, false);
        NVSDK_NGX_Resource_VK ro = AsResource(h.output, true);
        NVSDK_NGX_Resource_VK rd = AsResource(h.depth, false);
        NVSDK_NGX_Resource_VK rm = AsResource(h.mv, false);
        h.p->Set(NVSDK_NGX_Parameter_Color,         &rc);
        h.p->Set(NVSDK_NGX_Parameter_Output,        &ro);
        h.p->Set(NVSDK_NGX_Parameter_Depth,         &rd);
        h.p->Set(NVSDK_NGX_Parameter_MotionVectors, &rm);

        const NVSDK_NGX_Result r = NVSDK_NGX_VULKAN_EvaluateFeature(h.cmd, h.feat, h.p, nullptr);
        ++h.evaluated;
        if (NVSDK_NGX_SUCCEED(r)) ++h.delivered;
        else if (h.delivered == 0 || (h.evaluated % 600) == 0)
            printf("  EvaluateFeature frame %d -> 0x%08X\n", h.frame, r);
    }

    // Present the upscaled output.
    Barrier(h.cmd, h.swap_imgs[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit blit = {};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1] = { static_cast<int32_t>(h.output.w), static_cast<int32_t>(h.output.h), 1 };
    blit.dstOffsets[1] = { static_cast<int32_t>(h.swap_ext.width), static_cast<int32_t>(h.swap_ext.height), 1 };
    vkCmdBlitImage(h.cmd, h.output.img, VK_IMAGE_LAYOUT_GENERAL,
                   h.swap_imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    Barrier(h.cmd, h.swap_imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    vkEndCommandBuffer(h.cmd);

    const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.waitSemaphoreCount = 1; si.pWaitSemaphores = &acq; si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1; si.pCommandBuffers = &h.cmd;
    si.signalSemaphoreCount = 1; si.pSignalSemaphores = &h.rendered[idx];
    vkQueueSubmit(h.queue, 1, &si, h.fence);

    VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &h.rendered[idx];
    pi.swapchainCount = 1; pi.pSwapchains = &h.swap; pi.pImageIndices = &idx;
    vkQueuePresentKHR(h.queue, &pi);
    ++h.frame;

    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
    {
        if (msg.message == WM_QUIT) return false;
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return true;
}

static bool Setup(Host &h)
{
    // What NGX needs of the instance and the device, asked rather than assumed.
    NVSDK_NGX_FeatureDiscoveryInfo fdi = {};
    fdi.SDKVersion = NVSDK_NGX_Version_API;
    fdi.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    fdi.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    fdi.Identifier.v.ProjectDesc.ProjectId = "a7d3f0c8-6b21-4e5a-9f14-3c07b1e9d240";
    fdi.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
    fdi.Identifier.v.ProjectDesc.EngineVersion = "1.0";
    fdi.ApplicationDataPath = L".";

    uint32_t nie = 0; VkExtensionProperties *ie = nullptr;
    uint32_t nde = 0; VkExtensionProperties *de = nullptr;
    NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&fdi, &nie, &ie);
    printf("NGX wants %u instance extension(s)\n", nie);

    std::vector<const char *> inst_ext = { VK_KHR_SURFACE_EXTENSION_NAME,
                                           VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    for (uint32_t i = 0; i < nie; ++i) inst_ext.push_back(ie[i].extensionName);

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "ngxhost"; app.applicationVersion = 1;
    app.pEngineName = "ngxhost"; app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = static_cast<uint32_t>(inst_ext.size());
    ici.ppEnabledExtensionNames = inst_ext.data();
    VKC(vkCreateInstance(&ici, nullptr, &h.inst), "vkCreateInstance");

    VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = GetModuleHandleW(nullptr); sci.hwnd = h.hwnd;
    VKC(vkCreateWin32SurfaceKHR(h.inst, &sci, nullptr, &h.surf), "vkCreateWin32SurfaceKHR");

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(h.inst, &ndev, nullptr);
    std::vector<VkPhysicalDevice> devs(ndev);
    vkEnumeratePhysicalDevices(h.inst, &ndev, devs.data());
    for (VkPhysicalDevice d : devs)
    {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qs(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, qs.data());
        for (uint32_t i = 0; i < nq; ++i)
        {
            VkBool32 pr = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, h.surf, &pr);
            if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pr) { h.phys = d; h.qfam = i; break; }
        }
        if (h.phys != VK_NULL_HANDLE) break;
    }
    if (h.phys == VK_NULL_HANDLE) { printf("FAIL: no graphics+present queue\n"); return false; }
    VkPhysicalDeviceProperties pd = {};
    vkGetPhysicalDeviceProperties(h.phys, &pd);
    printf("device: %s\n", pd.deviceName);

    NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(h.inst, h.phys, &fdi, &nde, &de);
    printf("NGX wants %u device extension(s)\n", nde);
    std::vector<const char *> dev_ext = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    // By name, not only by count. An extension arriving from NGX with nothing
    // enabling its feature is the shape of the defect above, and a count cannot
    // show it.
    for (uint32_t i = 0; i < nde; ++i)
    { printf("  %s\n", de[i].extensionName); dev_ext.push_back(de[i].extensionName); }

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = h.qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    // bufferDeviceAddress, because NGX asks for VK_KHR_buffer_device_address and an
    // extension enabled without its feature is fourteen validation errors before
    // frame 1 -- all this host own, and the first thing a reader triaging that log
    // meets. Only what was measured: adding descriptorIndexing or timelineSemaphore
    // because NGX probably uses them is the supply-what-nothing-declared move, and
    // feeding the queried feature chain straight back would enable everything this
    // card supports and hide the next missing feature exactly as this one was hidden.
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.pNext = &f12;
    f13.dynamicRendering = VK_TRUE; f13.synchronization2 = VK_TRUE;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.pNext = &f13;
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(dev_ext.size());
    dci.ppEnabledExtensionNames = dev_ext.data();
    VKC(vkCreateDevice(h.phys, &dci, nullptr, &h.dev), "vkCreateDevice");
    vkGetDeviceQueue(h.dev, h.qfam, 0, &h.queue);

    VkSurfaceCapabilitiesKHR caps = {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(h.phys, h.surf, &caps);
    h.swap_ext = caps.currentExtent;
    if (h.swap_ext.width == 0xFFFFFFFFu) { h.swap_ext = { h.out_w, h.out_h }; }

    VkSwapchainCreateInfoKHR swci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    swci.surface = h.surf;
    swci.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
    swci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    swci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swci.imageExtent = h.swap_ext;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swci.clipped = VK_TRUE;
    VKC(vkCreateSwapchainKHR(h.dev, &swci, nullptr, &h.swap), "vkCreateSwapchainKHR");
    uint32_t nimg = 0;
    vkGetSwapchainImagesKHR(h.dev, h.swap, &nimg, nullptr);
    h.swap_imgs.resize(nimg);
    vkGetSwapchainImagesKHR(h.dev, h.swap, &nimg, h.swap_imgs.data());
    printf("swapchain: %ux%u, %u images\n", h.swap_ext.width, h.swap_ext.height, nimg);

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = h.qfam;
    VKC(vkCreateCommandPool(h.dev, &pci, nullptr, &h.pool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = h.pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VKC(vkAllocateCommandBuffers(h.dev, &cbi, &h.cmd), "vkAllocateCommandBuffers");
    VkSemaphoreCreateInfo semi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    h.acquired.resize(nimg); h.rendered.resize(nimg);
    for (uint32_t i = 0; i < nimg; ++i)
    {
        VKC(vkCreateSemaphore(h.dev, &semi, nullptr, &h.acquired[i]), "vkCreateSemaphore(acquire)");
        VKC(vkCreateSemaphore(h.dev, &semi, nullptr, &h.rendered[i]), "vkCreateSemaphore(render)");
    }
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VKC(vkCreateFence(h.dev, &fci, nullptr, &h.fence), "vkCreateFence");

    wchar_t here[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, here, MAX_PATH);
    if (wchar_t *s = wcsrchr(here, L'\\')) *(s + 1) = 0;
    const wchar_t *paths[1] = { here };
    NVSDK_NGX_FeatureCommonInfo ci = {};
    ci.PathListInfo.Path = paths;
    ci.PathListInfo.Length = 1;

    const NVSDK_NGX_Result nr = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        "a7d3f0c8-6b21-4e5a-9f14-3c07b1e9d240", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "1.0",
        here, h.inst, h.phys, h.dev, vkGetInstanceProcAddr, vkGetDeviceProcAddr,
        &ci, NVSDK_NGX_Version_API);
    printf("VULKAN_Init_with_ProjectID -> 0x%08X\n", nr);
    if (NVSDK_NGX_FAILED(nr)) return false;

    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_GetCapabilityParameters(&h.caps)) || !h.caps)
    { printf("FAIL: GetCapabilityParameters\n"); return false; }
    int avail = 0;
    h.caps->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    printf("SuperSampling available: %d\n", avail);
    if (!avail)
    {
        int res = 0; h.caps->Get(NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &res);
        printf("FAIL: DLSS unavailable, FeatureInitResult 0x%08X\n", res);
        return false;
    }
    if (NVSDK_NGX_FAILED(NVSDK_NGX_VULKAN_AllocateParameters(&h.p)) || !h.p)
    { printf("FAIL: AllocateParameters\n"); return false; }
    return true;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);

    Scenario sc = {};
    if (argc > 1 && strstr(argv[1], ".txt") != nullptr)
    { if (!ScenarioLoad(&sc, argv[1])) { printf("FAIL: scenario\n"); return 2; } }
    else
    {
        ScenarioAdd(&sc, STEP_FRAMES, argc > 1 ? atoi(argv[1]) : 600);
        strncpy_s(sc.name, "smoke", _TRUNCATE);
    }
    printf("ngxhost-vk: scenario '%s', %d steps\n", sc.name, sc.count);

    Host h;
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ngxhostvk";
    RegisterClassExW(&wc);
    RECT r = { 0, 0, static_cast<LONG>(h.out_w), static_cast<LONG>(h.out_h) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    h.hwnd = CreateWindowExW(0, L"ngxhostvk", L"ngxhost-vk", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                             nullptr, nullptr, wc.hInstance, nullptr);
    if (h.hwnd == nullptr) { printf("FAIL: no window\n"); return 2; }
    ShowWindow(h.hwnd, SW_SHOW);

    if (!Setup(h)) return 3;
    if (!Rebuild(h, "start")) return 3;

    // How many frames the scenario asks for, so a run that ends early is a failure
    // rather than a short green one. Closing the window at frame 5 of 7600 used to
    // abandon the loop with rc still 0, print the counters, print ok and return 0.
    int rc = 0, want_frames = 0;
    for (int i = 0; i < sc.count; ++i)
        if (sc.steps[i].kind == STEP_FRAMES) want_frames += sc.steps[i].a;
    for (int s = 0; s < sc.count && rc == 0; ++s)
    {
        const Step &st = sc.steps[s];
        switch (st.kind)
        {
        case STEP_FRAMES:
            printf("[%d/%d] frames %d\n", s + 1, sc.count, st.a);
            for (int i = 0; i < st.a; ++i) if (!RenderFrame(h)) { s = sc.count; break; }
            break;
        case STEP_PRESET:
            printf("[%d/%d] preset %d\n", s + 1, sc.count, st.a);
            h.quality = st.a;
            if (!Rebuild(h, "preset change")) rc = 4;
            break;
        case STEP_RECREATE:
            printf("[%d/%d] recreate at an unchanged shape\n", s + 1, sc.count);
            if (!Rebuild(h, "recreate")) rc = 4;
            break;
        case STEP_DLSS:
            printf("[%d/%d] %s\n", s + 1, sc.count, StepName(st));
            h.dlss_on = st.a != 0;
            break;
        case STEP_TRANSPOSE:
            printf("[%d/%d] %s\n", s + 1, sc.count, StepName(st));
            h.transpose = st.a != 0;
            break;
        case STEP_OMIT:
            printf("[%d/%d] omit %d\n", s + 1, sc.count, st.a);
            h.omit = static_cast<Omit>(st.a);
            if (st.a == OMIT_FLAGS && !Rebuild(h, "omit flags")) rc = 4;
            break;
        // Not yet on this half, and said so rather than silently ignored: a verb the
        // parser accepts and the executor drops is a run that proves something other
        // than what its file says. The D3D11 host learned that the expensive way.
        case STEP_MODE:
        case STEP_RESIZE:
        case STEP_HDR:
        case STEP_EXPOSURE:
            printf("[%d/%d] FAIL: '%s' is not implemented on the Vulkan host yet\n",
                   s + 1, sc.count, StepName(st));
            rc = 5;
            break;
        default:
            printf("[%d/%d] FAIL: step kind %d is parsed but not executed\n",
                   s + 1, sc.count, static_cast<int>(st.kind));
            rc = 5;
            break;
        }
    }

    printf("frames %d, evaluates %d, succeeded %d\n", h.frame, h.evaluated, h.delivered);

    vkDeviceWaitIdle(h.dev);
    ReleaseFeat(h);
    if (h.p) NVSDK_NGX_VULKAN_DestroyParameters(h.p);
    NVSDK_NGX_VULKAN_Shutdown1(h.dev);
    if (h.pipe) vkDestroyPipeline(h.dev, h.pipe, nullptr);
    if (h.play) vkDestroyPipelineLayout(h.dev, h.play, nullptr);
    DestroyImg(h.dev, &h.color); DestroyImg(h.dev, &h.mv);
    DestroyImg(h.dev, &h.depth); DestroyImg(h.dev, &h.output);
    if (h.fence) vkDestroyFence(h.dev, h.fence, nullptr);
    for (VkSemaphore s2 : h.rendered) if (s2) vkDestroySemaphore(h.dev, s2, nullptr);
    for (VkSemaphore s2 : h.acquired) if (s2) vkDestroySemaphore(h.dev, s2, nullptr);
    if (h.pool) vkDestroyCommandPool(h.dev, h.pool, nullptr);
    if (h.swap) vkDestroySwapchainKHR(h.dev, h.swap, nullptr);
    if (h.dev) vkDestroyDevice(h.dev, nullptr);
    if (h.surf) vkDestroySurfaceKHR(h.inst, h.surf, nullptr);
    if (h.inst) vkDestroyInstance(h.inst, nullptr);
    DestroyWindow(h.hwnd);

    if (h.frame < want_frames)
    { printf("FAIL: ran %d of %d frames the scenario asked for\n", h.frame, want_frames); return 6; }
    if (rc != 0) { printf("FAIL: scenario stopped\n"); return rc; }
    if (h.evaluated > 0 && h.delivered == 0) { printf("FAIL: no evaluate succeeded\n"); return 4; }
    printf("ok\n");
    return 0;
}
