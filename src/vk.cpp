// ngxhost-vk -- the Vulkan half.
//
// PHASE 4a. Not yet a DLSS host: an instance, a device, a swapchain and three
// hundred presents, for the same reason the D3D11 side started that way. If
// ReShade's implicit layer does not attach to this executable, or the add-on does
// not register inside it, nothing else in the Vulkan plan is worth writing -- and
// finding that out costs three hundred lines rather than a week.
//
// THE BLOCKER THIS FILE EXISTS TO TEST. On Vulkan ReShade is not a DLL beside the
// executable; it is an implicit layer registered machine-wide and gated per
// executable by C:\ProgramData\ReShade\ReShadeApps.ini. A new .exe is invisible to
// it until its full path is in that list, which is why the Vulkan host has ONE fixed
// location -- run\vk -- rather than a folder per scenario the way the D3D11 host
// does. One registration instead of one per scenario.
//
//   ..\build.cmd

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstring>
#include <vector>

#define VKCHECK(x, what) do { \
    const VkResult r_ = (x); \
    if (r_ != VK_SUCCESS) { printf("FAIL: %s -> VkResult %d\n", (what), static_cast<int>(r_)); return 2; } \
} while (0)

static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(h, m, w, l);
}

int main(int argc, char **argv)
{
    // Unbuffered, for the same reason the D3D11 host is: a crash with redirected
    // stdout otherwise loses everything and reads as "it died before main".
    setvbuf(stdout, nullptr, _IONBF, 0);

    const int frames = argc > 1 ? atoi(argv[1]) : 300;
    printf("ngxhost-vk phase 4a: %d frames\n", frames);

    // Report whether the ReShade layer is even offered to us, before trying to use
    // it. "The layer is not enumerated" and "the layer is enumerated and did nothing"
    // are different problems and the log should not conflate them.
    {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        if (n != 0) vkEnumerateInstanceLayerProperties(&n, props.data());
        bool found = false;
        for (const VkLayerProperties &p : props)
            if (strcmp(p.layerName, "VK_LAYER_reshade") == 0) found = true;
        printf("VK_LAYER_reshade enumerated: %s (%u layers visible)\n",
               found ? "yes" : "NO", n);
        if (!found)
            printf("  it is an implicit layer gated per executable by\n"
                   "  C:\\ProgramData\\ReShade\\ReShadeApps.ini -- this exe's full path\n"
                   "  has to be in that Apps= list. Enumeration is not the gate, so a\n"
                   "  'yes' here is not yet proof it will attach.\n");
    }

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc); wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"ngxhostvk";
    RegisterClassExW(&wc);
    RECT r = { 0, 0, 1280, 720 };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, L"ngxhostvk", L"ngxhost-vk", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (hwnd == nullptr) { printf("FAIL: no window\n"); return 2; }
    ShowWindow(hwnd, SW_SHOW);

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "ngxhost";
    app.applicationVersion = 1;
    app.pEngineName = "ngxhost";
    app.engineVersion = 1;
    app.apiVersion = VK_API_VERSION_1_3;

    const char *inst_ext[] = { VK_KHR_SURFACE_EXTENSION_NAME,
                               VK_KHR_WIN32_SURFACE_EXTENSION_NAME };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_ext;

    VkInstance inst = VK_NULL_HANDLE;
    VKCHECK(vkCreateInstance(&ici, nullptr, &inst), "vkCreateInstance");

    VkWin32SurfaceCreateInfoKHR sci = { VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR };
    sci.hinstance = wc.hInstance; sci.hwnd = hwnd;
    VkSurfaceKHR surf = VK_NULL_HANDLE;
    VKCHECK(vkCreateWin32SurfaceKHR(inst, &sci, nullptr, &surf), "vkCreateWin32SurfaceKHR");

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(inst, &ndev, nullptr);
    if (ndev == 0) { printf("FAIL: no Vulkan physical device\n"); return 2; }
    std::vector<VkPhysicalDevice> devs(ndev);
    vkEnumeratePhysicalDevices(inst, &ndev, devs.data());

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    uint32_t qfam = 0;
    for (VkPhysicalDevice d : devs)
    {
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> qs(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(d, &nq, qs.data());
        for (uint32_t i = 0; i < nq; ++i)
        {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(d, i, surf, &present);
            if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present)
            { phys = d; qfam = i; break; }
        }
        if (phys != VK_NULL_HANDLE) break;
    }
    if (phys == VK_NULL_HANDLE) { printf("FAIL: no graphics+present queue\n"); return 2; }

    VkPhysicalDeviceProperties pd = {};
    vkGetPhysicalDeviceProperties(phys, &pd);
    printf("device: %s, queue family %u\n", pd.deviceName, qfam);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = qfam; qci.queueCount = 1; qci.pQueuePriorities = &prio;
    const char *dev_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1; dci.ppEnabledExtensionNames = dev_ext;

    VkDevice dev = VK_NULL_HANDLE;
    VKCHECK(vkCreateDevice(phys, &dci, nullptr, &dev), "vkCreateDevice");
    VkQueue queue = VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, qfam, 0, &queue);

    VkSurfaceCapabilitiesKHR caps = {};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps);
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == 0xFFFFFFFFu) { ext.width = 1280; ext.height = 720; }

    VkSwapchainCreateInfoKHR swci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    swci.surface = surf;
    swci.minImageCount = caps.minImageCount < 2 ? 2 : caps.minImageCount;
    swci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    swci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    swci.imageExtent = ext;
    swci.imageArrayLayers = 1;
    swci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    swci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    swci.preTransform = caps.currentTransform;
    swci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    swci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    swci.clipped = VK_TRUE;

    VkSwapchainKHR swap = VK_NULL_HANDLE;
    VKCHECK(vkCreateSwapchainKHR(dev, &swci, nullptr, &swap), "vkCreateSwapchainKHR");

    uint32_t nimg = 0;
    vkGetSwapchainImagesKHR(dev, swap, &nimg, nullptr);
    std::vector<VkImage> imgs(nimg);
    vkGetSwapchainImagesKHR(dev, swap, &nimg, imgs.data());
    printf("swapchain: %ux%u, %u images\n", ext.width, ext.height, nimg);

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = qfam;
    VkCommandPool pool = VK_NULL_HANDLE;
    VKCHECK(vkCreateCommandPool(dev, &pci, nullptr, &pool), "vkCreateCommandPool");

    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VKCHECK(vkAllocateCommandBuffers(dev, &cbi, &cmd), "vkAllocateCommandBuffers");

    VkSemaphoreCreateInfo semi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquired = VK_NULL_HANDLE, rendered = VK_NULL_HANDLE;
    VKCHECK(vkCreateSemaphore(dev, &semi, nullptr, &acquired), "vkCreateSemaphore");
    VKCHECK(vkCreateSemaphore(dev, &semi, nullptr, &rendered), "vkCreateSemaphore");
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fence = VK_NULL_HANDLE;
    VKCHECK(vkCreateFence(dev, &fci, nullptr, &fence), "vkCreateFence");

    printf("presenting\n");
    for (int i = 0; i < frames; ++i)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT) { i = frames; break; }
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }

        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fence);

        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(dev, swap, UINT64_MAX, acquired, VK_NULL_HANDLE, &idx);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) break;

        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);

        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = imgs[idx];
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);

        // Something that moves, so a human watching can tell it is alive.
        const float t = static_cast<float>(i) / static_cast<float>(frames);
        VkClearColorValue c = {};
        c.float32[0] = 0.05f; c.float32[1] = t; c.float32[2] = 1.0f - t; c.float32[3] = 1.0f;
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rng);

        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkEndCommandBuffer(cmd);

        const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = 1; si.pWaitSemaphores = &acquired; si.pWaitDstStageMask = &wait;
        si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1; si.pSignalSemaphores = &rendered;
        vkQueueSubmit(queue, 1, &si, fence);

        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &rendered;
        pi.swapchainCount = 1; pi.pSwapchains = &swap; pi.pImageIndices = &idx;
        vkQueuePresentKHR(queue, &pi);
    }

    vkDeviceWaitIdle(dev);
    vkDestroyFence(dev, fence, nullptr);
    vkDestroySemaphore(dev, rendered, nullptr);
    vkDestroySemaphore(dev, acquired, nullptr);
    vkDestroyCommandPool(dev, pool, nullptr);
    vkDestroySwapchainKHR(dev, swap, nullptr);
    vkDestroyDevice(dev, nullptr);
    vkDestroySurfaceKHR(inst, surf, nullptr);
    vkDestroyInstance(inst, nullptr);
    DestroyWindow(hwnd);

    printf("ok: %d frames presented\n", frames);
    return 0;
}
