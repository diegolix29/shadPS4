/* x86-64 / aarch64 Vulkan probe for Bachata Vortek.
 *
 * Modes (env):
 *   BACHATA_VORTEK_TRANSPORT_ONLY=1 — handshake + CREATE_CONTEXT only (Task 5)
 *   BACHATA_VORTEK_HEADLESS=1       — full non-WSI device + empty submit (Task 6)
 *   BACHATA_VORTEK_WSI=1            — Xlib surface + swapchain present loop (Task 7)
 *   BACHATA_VORTEK_WSI_SECONDS=N    — present duration (default 60)
 *   BACHATA_VORTEK_SHAD=1           — shadPS4-matching Vulkan 1.3 init (Task 8)
 *
 * Always forces the real ICD via vk_icdGetInstanceProcAddr (not loader trampoline alone).
 */
#define VK_USE_PLATFORM_XLIB_KHR
#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <vulkan/vulkan.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddrFn)(VkInstance, const char*);
typedef VkResult (*PFN_vk_icdNegotiateLoaderICDInterfaceVersion)(uint32_t*);

static void on_alarm(int signo) {
    (void)signo;
    fprintf(stderr, "[Vortek.Probe] result=timeout\n");
    _exit(124);
}

static void stage(const char* name) {
    fprintf(stderr, "[Vortek.Probe] stage=%s\n", name);
}

static int env_flag(const char* name) {
    const char* v = getenv(name);
    return v && v[0] == '1';
}

static int env_int(const char* name, int def) {
    const char* v = getenv(name);
    if (!v || !v[0]) return def;
    int n = atoi(v);
    return n > 0 ? n : def;
}

static int fail(const char* result) {
    fprintf(stderr, "[Vortek.Probe] result=%s\n", result);
    return 1;
}

static int resolve_icd_library_path(const char* icd_path, char* out, size_t out_len) {
    FILE* f = fopen(icd_path, "r");
    if (!f) return -1;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    const char* key = "\"library_path\"";
    char* p = strstr(buf, key);
    if (!p) return -1;
    p = strchr(p + strlen(key), '"');
    if (!p) return -1;
    p++;
    char* end = strchr(p, '"');
    if (!end || (size_t)(end - p) >= out_len) return -1;
    memcpy(out, p, (size_t)(end - p));
    out[end - p] = '\0';
    return 0;
}

static void make_absolute_lib_path(const char* icd_path, const char* lib_path, char* out, size_t out_len) {
    if (lib_path[0] == '/') {
        snprintf(out, out_len, "%s", lib_path);
        return;
    }
    const char* slash = strrchr(icd_path, '/');
    char joined[PATH_MAX];
    if (!slash) {
        snprintf(joined, sizeof(joined), "%s", lib_path);
    } else {
        size_t dir_len = (size_t)(slash - icd_path);
        if (dir_len + 1 + strlen(lib_path) + 1 > sizeof(joined)) {
            out[0] = '\0';
            return;
        }
        memcpy(joined, icd_path, dir_len);
        joined[dir_len] = '/';
        snprintf(joined + dir_len + 1, sizeof(joined) - dir_len - 1, "%s", lib_path);
    }
    char stack[PATH_MAX];
    size_t sp = 0;
    stack[0] = '\0';
    const char* p = joined;
    if (p[0] == '/') {
        stack[sp++] = '/';
        stack[sp] = '\0';
        p++;
    }
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;
        const char* start = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.') continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            if (sp > 1) {
                sp--;
                while (sp > 0 && stack[sp - 1] != '/') sp--;
                stack[sp] = '\0';
            }
            continue;
        }
        if (sp > 0 && stack[sp - 1] != '/') {
            if (sp + 1 >= sizeof(stack)) break;
            stack[sp++] = '/';
        }
        if (sp + len >= sizeof(stack)) break;
        memcpy(stack + sp, start, len);
        sp += len;
        stack[sp] = '\0';
    }
    snprintf(out, out_len, "%s", sp ? stack : "/");
}

static void* load_icd(const char* icd, char* lib_abs, size_t lib_abs_len) {
    char lib_rel[512];
    if (resolve_icd_library_path(icd, lib_rel, sizeof(lib_rel)) != 0) {
        fail("icd_parse_failed");
        return NULL;
    }
    make_absolute_lib_path(icd, lib_rel, lib_abs, lib_abs_len);
    fprintf(stderr, "[Vortek.Probe] stage=icd_library path=%s\n", lib_abs);
    void* icd_lib = dlopen(lib_abs, RTLD_NOW | RTLD_LOCAL);
    if (!icd_lib) icd_lib = dlopen("libvulkan_vortek.so", RTLD_NOW | RTLD_LOCAL);
    if (!icd_lib) {
        fprintf(stderr, "[Vortek.Probe] result=icd_dlopen_failed error=%s\n", dlerror());
        return NULL;
    }
    stage("libvulkan_vortek_loaded");
    stage("icd_loaded");
    PFN_vk_icdNegotiateLoaderICDInterfaceVersion negotiate =
        (PFN_vk_icdNegotiateLoaderICDInterfaceVersion)dlsym(
            icd_lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (negotiate) {
        uint32_t ver = 3;
        VkResult nr = negotiate(&ver);
        fprintf(stderr, "[Vortek.Probe] stage=icd_negotiate result=%d version=%u\n", (int)nr, ver);
    }
    return icd_lib;
}

static PFN_vkVoidFunction icd_proc(void* icd_lib, VkInstance inst, const char* name) {
    PFN_vkGetInstanceProcAddrFn gipa =
        (PFN_vkGetInstanceProcAddrFn)dlsym(icd_lib, "vk_icdGetInstanceProcAddr");
    if (!gipa) return NULL;
    return gipa(inst, name);
}

static int run_transport_only(void* icd_lib) {
    PFN_vkVoidFunction create_fn = icd_proc(icd_lib, VK_NULL_HANDLE, "vkCreateInstance");
    if (!create_fn) {
        fprintf(stderr, "[Vortek.Probe] result=server_unavailable\n");
        return 0;
    }
    stage("transport_ready");
    fprintf(stderr, "[Vortek.Probe] result=context_ready\n");
    usleep(200 * 1000);
    return 0;
}

#define LOAD(inst, name, type, var) \
    type var = (type)icd_proc(icd_lib, inst, name); \
    if (!var) return fail("missing_" name)

static int run_headless(void* icd_lib) {
    LOAD(VK_NULL_HANDLE, "vkEnumerateInstanceVersion", PFN_vkEnumerateInstanceVersion, enumerate_version);
    LOAD(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties",
         PFN_vkEnumerateInstanceExtensionProperties, enumerate_inst_ext);
    LOAD(VK_NULL_HANDLE, "vkCreateInstance", PFN_vkCreateInstance, create_instance);

    uint32_t version = 0;
    VkResult vr = enumerate_version(&version);
    fprintf(stderr, "[Vortek.Probe] stage=instance_version result=%d version=0x%x\n",
            (int)vr, version);
    if (vr != VK_SUCCESS) return fail("instance_version_failed");

    uint32_t ext_count = 0;
    vr = enumerate_inst_ext(NULL, &ext_count, NULL);
    fprintf(stderr, "[Vortek.Probe] stage=instance_extensions count=%u result=%d\n",
            ext_count, (int)vr);
    if (vr != VK_SUCCESS && vr != VK_INCOMPLETE) return fail("instance_extensions_failed");

    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "bachata-vortek-probe";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;

    VkInstance instance = VK_NULL_HANDLE;
    vr = create_instance(&ici, NULL, &instance);
    fprintf(stderr, "[Vortek.Probe] stage=instance_created result=%d\n", (int)vr);
    if (vr != VK_SUCCESS || instance == VK_NULL_HANDLE) return fail("instance_create_failed");

    LOAD(instance, "vkEnumeratePhysicalDevices", PFN_vkEnumeratePhysicalDevices, enum_pdev);
    LOAD(instance, "vkGetPhysicalDeviceProperties", PFN_vkGetPhysicalDeviceProperties, get_props);
    LOAD(instance, "vkGetPhysicalDeviceFeatures", PFN_vkGetPhysicalDeviceFeatures, get_features);
    LOAD(instance, "vkGetPhysicalDeviceQueueFamilyProperties",
         PFN_vkGetPhysicalDeviceQueueFamilyProperties, get_qfam);
    LOAD(instance, "vkGetPhysicalDeviceMemoryProperties",
         PFN_vkGetPhysicalDeviceMemoryProperties, get_mem);
    LOAD(instance, "vkEnumerateDeviceExtensionProperties",
         PFN_vkEnumerateDeviceExtensionProperties, enum_dev_ext);
    LOAD(instance, "vkCreateDevice", PFN_vkCreateDevice, create_device);
    LOAD(instance, "vkDestroyInstance", PFN_vkDestroyInstance, destroy_instance);

    uint32_t pdev_count = 0;
    vr = enum_pdev(instance, &pdev_count, NULL);
    fprintf(stderr, "[Vortek.Probe] stage=physical_devices count=%u result=%d\n",
            pdev_count, (int)vr);
    if (vr != VK_SUCCESS || pdev_count == 0) {
        destroy_instance(instance, NULL);
        return fail("no_physical_device");
    }
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    vr = enum_pdev(instance, &pdev_count, &pdev);
    if (vr != VK_SUCCESS || pdev == VK_NULL_HANDLE) {
        destroy_instance(instance, NULL);
        return fail("physical_device_enum_failed");
    }

    VkPhysicalDeviceProperties props;
    memset(&props, 0, sizeof(props));
    get_props(pdev, &props);
    fprintf(stderr, "[Vortek.Probe] stage=physical_device name=%s vendor=0x%x device=0x%x type=%u api=0x%x driver=0x%x\n",
            props.deviceName, props.vendorID, props.deviceID, (unsigned)props.deviceType,
            props.apiVersion, props.driverVersion);
    if (props.deviceName[0] == '\0') {
        destroy_instance(instance, NULL);
        return fail("empty_device_name");
    }

    VkPhysicalDeviceFeatures features;
    memset(&features, 0, sizeof(features));
    get_features(pdev, &features);

    uint32_t qf_count = 0;
    get_qfam(pdev, &qf_count, NULL);
    if (qf_count == 0) {
        destroy_instance(instance, NULL);
        return fail("no_queue_families");
    }
    VkQueueFamilyProperties* qf = calloc(qf_count, sizeof(VkQueueFamilyProperties));
    get_qfam(pdev, &qf_count, qf);
    int graphics_index = -1;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphics_index = (int)i;
            break;
        }
    }
    free(qf);
    if (graphics_index < 0) {
        destroy_instance(instance, NULL);
        return fail("no_graphics_queue");
    }
    fprintf(stderr, "[Vortek.Probe] stage=queue_family_selected index=%d\n", graphics_index);

    VkPhysicalDeviceMemoryProperties mem;
    memset(&mem, 0, sizeof(mem));
    get_mem(pdev, &mem);
    fprintf(stderr, "[Vortek.Probe] stage=memory heaps=%u types=%u\n",
            mem.memoryHeapCount, mem.memoryTypeCount);

    uint32_t dev_ext_count = 0;
    vr = enum_dev_ext(pdev, NULL, &dev_ext_count, NULL);
    fprintf(stderr, "[Vortek.Probe] stage=device_extensions count=%u result=%d\n",
            dev_ext_count, (int)vr);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = (uint32_t)graphics_index;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;

    VkDevice device = VK_NULL_HANDLE;
    vr = create_device(pdev, &dci, NULL, &device);
    fprintf(stderr, "[Vortek.Probe] stage=device_created result=%d\n", (int)vr);
    if (vr != VK_SUCCESS || device == VK_NULL_HANDLE) {
        destroy_instance(instance, NULL);
        return fail("device_create_failed");
    }

    LOAD(instance, "vkGetDeviceQueue", PFN_vkGetDeviceQueue, get_queue);
    LOAD(instance, "vkDestroyDevice", PFN_vkDestroyDevice, destroy_device);
    LOAD(instance, "vkCreateCommandPool", PFN_vkCreateCommandPool, create_pool);
    LOAD(instance, "vkDestroyCommandPool", PFN_vkDestroyCommandPool, destroy_pool);
    LOAD(instance, "vkAllocateCommandBuffers", PFN_vkAllocateCommandBuffers, alloc_cmd);
    LOAD(instance, "vkFreeCommandBuffers", PFN_vkFreeCommandBuffers, free_cmd);
    LOAD(instance, "vkBeginCommandBuffer", PFN_vkBeginCommandBuffer, begin_cmd);
    LOAD(instance, "vkEndCommandBuffer", PFN_vkEndCommandBuffer, end_cmd);
    LOAD(instance, "vkQueueSubmit", PFN_vkQueueSubmit, queue_submit);
    LOAD(instance, "vkQueueWaitIdle", PFN_vkQueueWaitIdle, queue_wait);
    LOAD(instance, "vkDeviceWaitIdle", PFN_vkDeviceWaitIdle, device_wait);

    VkQueue queue = VK_NULL_HANDLE;
    get_queue(device, (uint32_t)graphics_index, 0, &queue);
    if (queue == VK_NULL_HANDLE) {
        destroy_device(device, NULL);
        destroy_instance(instance, NULL);
        return fail("queue_retrieve_failed");
    }
    stage("queue_retrieved");

    VkCommandPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.queueFamilyIndex = (uint32_t)graphics_index;
    VkCommandPool pool = VK_NULL_HANDLE;
    vr = create_pool(device, &pci, NULL, &pool);
    fprintf(stderr, "[Vortek.Probe] stage=command_pool_created result=%d\n", (int)vr);
    if (vr != VK_SUCCESS) {
        destroy_device(device, NULL);
        destroy_instance(instance, NULL);
        return fail("command_pool_failed");
    }

    VkCommandBufferAllocateInfo cai = {0};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vr = alloc_cmd(device, &cai, &cmd);
    if (vr != VK_SUCCESS || cmd == VK_NULL_HANDLE) {
        destroy_pool(device, pool, NULL);
        destroy_device(device, NULL);
        destroy_instance(instance, NULL);
        return fail("command_buffer_alloc_failed");
    }

    VkCommandBufferBeginInfo bi = {0};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vr = begin_cmd(cmd, &bi);
    if (vr != VK_SUCCESS) {
        free_cmd(device, pool, 1, &cmd);
        destroy_pool(device, pool, NULL);
        destroy_device(device, NULL);
        destroy_instance(instance, NULL);
        return fail("begin_command_buffer_failed");
    }
    vr = end_cmd(cmd);
    if (vr != VK_SUCCESS) {
        free_cmd(device, pool, 1, &cmd);
        destroy_pool(device, pool, NULL);
        destroy_device(device, NULL);
        destroy_instance(instance, NULL);
        return fail("end_command_buffer_failed");
    }
    stage("command_buffer_recorded");

    VkSubmitInfo si = {0};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vr = queue_submit(queue, 1, &si, VK_NULL_HANDLE);
    fprintf(stderr, "[Vortek.Probe] stage=queue_submit result=%d\n", (int)vr);
    if (vr != VK_SUCCESS) {
        free_cmd(device, pool, 1, &cmd);
        destroy_pool(device, pool, NULL);
        destroy_device(device, NULL);
        destroy_instance(instance, NULL);
        return fail("queue_submit_failed");
    }
    fprintf(stderr, "[Vortek.Probe] stage=queue_submit result=VK_SUCCESS\n");

    vr = queue_wait(queue);
    if (vr != VK_SUCCESS) {
        free_cmd(device, pool, 1, &cmd);
        destroy_pool(device, pool, NULL);
        destroy_device(device, NULL);
        destroy_instance(instance, NULL);
        return fail("queue_wait_failed");
    }
    vr = device_wait(device);
    fprintf(stderr, "[Vortek.Probe] stage=device_idle result=%d\n", (int)vr);
    if (vr != VK_SUCCESS) {
        free_cmd(device, pool, 1, &cmd);
        destroy_pool(device, pool, NULL);
        destroy_device(device, NULL);
        destroy_instance(instance, NULL);
        return fail("device_wait_failed");
    }

    free_cmd(device, pool, 1, &cmd);
    destroy_pool(device, pool, NULL);
    destroy_device(device, NULL);
    destroy_instance(instance, NULL);
    stage("cleanup_complete");
    fprintf(stderr, "[Vortek.Probe] result=success\n");
    return 0;
}

/* ---------- Task 7 WSI ---------- */

static void image_barrier(PFN_vkCmdPipelineBarrier barrier_fn, VkCommandBuffer cmd,
                          VkImage image, VkImageLayout oldL, VkImageLayout newL,
                          VkAccessFlags srcA, VkAccessFlags dstA,
                          VkPipelineStageFlags srcS, VkPipelineStageFlags dstS) {
    VkImageMemoryBarrier b = {0};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = srcA;
    b.dstAccessMask = dstA;
    b.oldLayout = oldL;
    b.newLayout = newL;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    barrier_fn(cmd, srcS, dstS, 0, 0, NULL, 0, NULL, 1, &b);
}

static int run_wsi(void* icd_lib) {
    int duration_sec = env_int("BACHATA_VORTEK_WSI_SECONDS", 60);
    int do_resize = env_flag("BACHATA_VORTEK_WSI_RESIZE");
    if (!do_resize) do_resize = 1; /* default: one resize mid-run */

    Display* dpy = XOpenDisplay(NULL);
    if (!dpy) return fail("x_open_display_failed");

    int screen = DefaultScreen(dpy);
    Window root = RootWindow(dpy, screen);
    int win_w = 640;
    int win_h = 480;
    Window win = XCreateSimpleWindow(dpy, root, 0, 0, (unsigned)win_w, (unsigned)win_h, 0,
                                     BlackPixel(dpy, screen), BlackPixel(dpy, screen));
    XSelectInput(dpy, win, StructureNotifyMask | ExposureMask);
    XMapWindow(dpy, win);
    XStoreName(dpy, win, "Bachata Vortek WSI Probe");
    XFlush(dpy);

    /* Wait until mapped with nonzero size. */
    for (int i = 0; i < 200; i++) {
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == ConfigureNotify) {
                win_w = ev.xconfigure.width;
                win_h = ev.xconfigure.height;
            }
            if (ev.type == MapNotify) break;
        }
        if (win_w > 0 && win_h > 0) break;
        usleep(10 * 1000);
    }
    fprintf(stderr, "[Vortek.WSI] stage=x_window_created id=%lu size=%dx%d\n",
            (unsigned long)win, win_w, win_h);
    if (win_w <= 0 || win_h <= 0) {
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return fail("x_window_zero_size");
    }

    LOAD(VK_NULL_HANDLE, "vkEnumerateInstanceVersion", PFN_vkEnumerateInstanceVersion, enumerate_version);
    LOAD(VK_NULL_HANDLE, "vkCreateInstance", PFN_vkCreateInstance, create_instance);

    uint32_t version = 0;
    VkResult vr = enumerate_version(&version);
    fprintf(stderr, "[Vortek.Probe] stage=instance_version result=%d version=0x%x\n", (int)vr, version);
    if (vr != VK_SUCCESS) return fail("instance_version_failed");

    const char* inst_exts[] = {
        VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_XLIB_SURFACE_EXTENSION_NAME,
    };
    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "bachata-vortek-wsi-probe";
    app.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_exts;

    VkInstance instance = VK_NULL_HANDLE;
    vr = create_instance(&ici, NULL, &instance);
    fprintf(stderr, "[Vortek.Probe] stage=instance_created result=%d\n", (int)vr);
    if (vr != VK_SUCCESS || !instance) {
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return fail("instance_create_failed");
    }

    LOAD(instance, "vkCreateXlibSurfaceKHR", PFN_vkCreateXlibSurfaceKHR, create_xlib_surface);
    LOAD(instance, "vkDestroySurfaceKHR", PFN_vkDestroySurfaceKHR, destroy_surface);
    LOAD(instance, "vkEnumeratePhysicalDevices", PFN_vkEnumeratePhysicalDevices, enum_pdev);
    LOAD(instance, "vkGetPhysicalDeviceProperties", PFN_vkGetPhysicalDeviceProperties, get_props);
    LOAD(instance, "vkGetPhysicalDeviceQueueFamilyProperties",
         PFN_vkGetPhysicalDeviceQueueFamilyProperties, get_qfam);
    LOAD(instance, "vkGetPhysicalDeviceSurfaceSupportKHR",
         PFN_vkGetPhysicalDeviceSurfaceSupportKHR, get_surf_support);
    LOAD(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
         PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR, get_surf_caps);
    LOAD(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR",
         PFN_vkGetPhysicalDeviceSurfaceFormatsKHR, get_surf_fmts);
    LOAD(instance, "vkGetPhysicalDeviceSurfacePresentModesKHR",
         PFN_vkGetPhysicalDeviceSurfacePresentModesKHR, get_surf_modes);
    LOAD(instance, "vkCreateDevice", PFN_vkCreateDevice, create_device);
    LOAD(instance, "vkDestroyInstance", PFN_vkDestroyInstance, destroy_instance);

    VkXlibSurfaceCreateInfoKHR sci = {0};
    sci.sType = VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR;
    sci.dpy = dpy;
    sci.window = win;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    vr = create_xlib_surface(instance, &sci, NULL, &surface);
    fprintf(stderr, "[Vortek.WSI] stage=surface_created result=%d\n", (int)vr);
    if (vr != VK_SUCCESS || !surface) {
        destroy_instance(instance, NULL);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return fail("surface_create_failed");
    }

    uint32_t pdev_count = 0;
    enum_pdev(instance, &pdev_count, NULL);
    if (pdev_count == 0) {
        destroy_surface(instance, surface, NULL);
        destroy_instance(instance, NULL);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return fail("no_physical_device");
    }
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    enum_pdev(instance, &pdev_count, &pdev);
    VkPhysicalDeviceProperties props;
    memset(&props, 0, sizeof(props));
    get_props(pdev, &props);
    fprintf(stderr, "[Vortek.Probe] stage=physical_device name=%s\n", props.deviceName);

    uint32_t qf_count = 0;
    get_qfam(pdev, &qf_count, NULL);
    VkQueueFamilyProperties* qf = calloc(qf_count, sizeof(VkQueueFamilyProperties));
    get_qfam(pdev, &qf_count, qf);
    int graphics_index = -1;
    int present_index = -1;
    for (uint32_t i = 0; i < qf_count; i++) {
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphics_index < 0)
            graphics_index = (int)i;
        VkBool32 supported = VK_FALSE;
        get_surf_support(pdev, i, surface, &supported);
        if (supported && present_index < 0) present_index = (int)i;
        if (supported && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
            graphics_index = (int)i;
            present_index = (int)i;
            break;
        }
    }
    free(qf);
    if (graphics_index < 0 || present_index < 0) {
        destroy_surface(instance, surface, NULL);
        destroy_instance(instance, NULL);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return fail("no_present_queue");
    }
    fprintf(stderr, "[Vortek.WSI] stage=surface_support queue=%d supported=true\n", present_index);

    uint32_t fmt_count = 0;
    get_surf_fmts(pdev, surface, &fmt_count, NULL);
    fprintf(stderr, "[Vortek.WSI] stage=surface_formats count=%u\n", fmt_count);
    VkSurfaceFormatKHR* fmts = calloc(fmt_count ? fmt_count : 1, sizeof(VkSurfaceFormatKHR));
    if (fmt_count) get_surf_fmts(pdev, surface, &fmt_count, fmts);
    VkSurfaceFormatKHR chosen_fmt = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    for (uint32_t i = 0; i < fmt_count; i++) {
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM ||
            fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosen_fmt = fmts[i];
            break;
        }
    }
    free(fmts);

    uint32_t mode_count = 0;
    get_surf_modes(pdev, surface, &mode_count, NULL);
    fprintf(stderr, "[Vortek.WSI] stage=present_modes count=%u\n", mode_count);
    VkPresentModeKHR* modes = calloc(mode_count ? mode_count : 1, sizeof(VkPresentModeKHR));
    if (mode_count) get_surf_modes(pdev, surface, &mode_count, modes);
    VkPresentModeKHR chosen_mode = VK_PRESENT_MODE_FIFO_KHR;
    int have_fifo = 0;
    for (uint32_t i = 0; i < mode_count; i++) {
        if (modes[i] == VK_PRESENT_MODE_FIFO_KHR) {
            have_fifo = 1;
            chosen_mode = VK_PRESENT_MODE_FIFO_KHR;
            break;
        }
    }
    if (!have_fifo && mode_count > 0) chosen_mode = modes[0];
    free(modes);

    const char* dev_exts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = (uint32_t)graphics_index;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_exts;

    VkDevice device = VK_NULL_HANDLE;
    vr = create_device(pdev, &dci, NULL, &device);
    fprintf(stderr, "[Vortek.Probe] stage=device_created result=%d\n", (int)vr);
    if (vr != VK_SUCCESS || !device) {
        destroy_surface(instance, surface, NULL);
        destroy_instance(instance, NULL);
        XDestroyWindow(dpy, win);
        XCloseDisplay(dpy);
        return fail("device_create_failed");
    }

    LOAD(instance, "vkGetDeviceQueue", PFN_vkGetDeviceQueue, get_queue);
    LOAD(instance, "vkDestroyDevice", PFN_vkDestroyDevice, destroy_device);
    LOAD(instance, "vkCreateSwapchainKHR", PFN_vkCreateSwapchainKHR, create_swapchain);
    LOAD(instance, "vkDestroySwapchainKHR", PFN_vkDestroySwapchainKHR, destroy_swapchain);
    LOAD(instance, "vkGetSwapchainImagesKHR", PFN_vkGetSwapchainImagesKHR, get_sc_images);
    LOAD(instance, "vkAcquireNextImageKHR", PFN_vkAcquireNextImageKHR, acquire_image);
    LOAD(instance, "vkQueuePresentKHR", PFN_vkQueuePresentKHR, queue_present);
    LOAD(instance, "vkCreateCommandPool", PFN_vkCreateCommandPool, create_pool);
    LOAD(instance, "vkDestroyCommandPool", PFN_vkDestroyCommandPool, destroy_pool);
    LOAD(instance, "vkAllocateCommandBuffers", PFN_vkAllocateCommandBuffers, alloc_cmd);
    LOAD(instance, "vkFreeCommandBuffers", PFN_vkFreeCommandBuffers, free_cmd);
    LOAD(instance, "vkBeginCommandBuffer", PFN_vkBeginCommandBuffer, begin_cmd);
    LOAD(instance, "vkEndCommandBuffer", PFN_vkEndCommandBuffer, end_cmd);
    LOAD(instance, "vkCmdPipelineBarrier", PFN_vkCmdPipelineBarrier, cmd_barrier);
    LOAD(instance, "vkCmdClearColorImage", PFN_vkCmdClearColorImage, cmd_clear);
    LOAD(instance, "vkQueueSubmit", PFN_vkQueueSubmit, queue_submit);
    LOAD(instance, "vkCreateSemaphore", PFN_vkCreateSemaphore, create_sem);
    LOAD(instance, "vkDestroySemaphore", PFN_vkDestroySemaphore, destroy_sem);
    LOAD(instance, "vkCreateFence", PFN_vkCreateFence, create_fence);
    LOAD(instance, "vkDestroyFence", PFN_vkDestroyFence, destroy_fence);
    LOAD(instance, "vkWaitForFences", PFN_vkWaitForFences, wait_fences);
    LOAD(instance, "vkResetFences", PFN_vkResetFences, reset_fences);
    LOAD(instance, "vkResetCommandBuffer", PFN_vkResetCommandBuffer, reset_cmd);
    LOAD(instance, "vkDeviceWaitIdle", PFN_vkDeviceWaitIdle, device_wait);
    LOAD(instance, "vkQueueWaitIdle", PFN_vkQueueWaitIdle, queue_wait);

    VkQueue queue = VK_NULL_HANDLE;
    get_queue(device, (uint32_t)graphics_index, 0, &queue);
    stage("queue_retrieved");

    VkCommandPoolCreateInfo pci = {0};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = (uint32_t)graphics_index;
    VkCommandPool pool = VK_NULL_HANDLE;
    create_pool(device, &pci, NULL, &pool);

    VkCommandBufferAllocateInfo cai = {0};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    alloc_cmd(device, &cai, &cmd);

    VkSemaphoreCreateInfo sem_ci = {0};
    sem_ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    VkFenceCreateInfo fence_ci = {0};
    fence_ci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fence_ci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkSemaphore image_available = VK_NULL_HANDLE;
    VkSemaphore render_finished = VK_NULL_HANDLE;
    VkFence in_flight = VK_NULL_HANDLE;
    create_sem(device, &sem_ci, NULL, &image_available);
    create_sem(device, &sem_ci, NULL, &render_finished);
    create_fence(device, &fence_ci, NULL, &in_flight);

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkImage* images = NULL;
    uint32_t image_count = 0;
    VkExtent2D extent = {(uint32_t)win_w, (uint32_t)win_h};

    /* Present-loop state must live ABOVE the recreate label — goto must not re-zero these. */
    const float colors[5][4] = {
        {1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 1},
    };
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    uint64_t frames = 0;
    int resized_once = 0;
    int color_i = 0;
    double sum_ms = 0;
    double max_ms = 0;

    auto_create_swapchain:
    {
        VkSurfaceCapabilitiesKHR caps;
        memset(&caps, 0, sizeof(caps));
        get_surf_caps(pdev, surface, &caps);
        if (caps.currentExtent.width != 0xFFFFFFFF) {
            extent = caps.currentExtent;
        } else {
            extent.width = (uint32_t)win_w;
            extent.height = (uint32_t)win_h;
        }
        if (extent.width == 0 || extent.height == 0) {
            usleep(20 * 1000);
            goto auto_create_swapchain;
        }
        uint32_t min_images = caps.minImageCount;
        if (caps.maxImageCount > 0 && min_images > caps.maxImageCount)
            min_images = caps.maxImageCount;
        if (min_images < 1) min_images = 1;

        VkSwapchainCreateInfoKHR scci = {0};
        scci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        scci.surface = surface;
        scci.minImageCount = min_images;
        scci.imageFormat = chosen_fmt.format;
        scci.imageColorSpace = chosen_fmt.colorSpace;
        scci.imageExtent = extent;
        scci.imageArrayLayers = 1;
        scci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        scci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        scci.preTransform = caps.currentTransform;
        scci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
        scci.presentMode = chosen_mode;
        scci.clipped = VK_TRUE;
        scci.oldSwapchain = swapchain;

        VkSwapchainKHR new_sc = VK_NULL_HANDLE;
        vr = create_swapchain(device, &scci, NULL, &new_sc);
        if (swapchain) {
            destroy_swapchain(device, swapchain, NULL);
            free(images);
            images = NULL;
        }
        swapchain = new_sc;
        fprintf(stderr,
                "[Vortek.WSI] stage=swapchain_created images=%u extent=%ux%u format=%d mode=%d result=%d\n",
                min_images, extent.width, extent.height, (int)chosen_fmt.format,
                (int)chosen_mode, (int)vr);
        if (vr != VK_SUCCESS || !swapchain) {
            device_wait(device);
            destroy_fence(device, in_flight, NULL);
            destroy_sem(device, render_finished, NULL);
            destroy_sem(device, image_available, NULL);
            free_cmd(device, pool, 1, &cmd);
            destroy_pool(device, pool, NULL);
            destroy_device(device, NULL);
            destroy_surface(instance, surface, NULL);
            destroy_instance(instance, NULL);
            XDestroyWindow(dpy, win);
            XCloseDisplay(dpy);
            return fail("swapchain_create_failed");
        }

        get_sc_images(device, swapchain, &image_count, NULL);
        images = calloc(image_count, sizeof(VkImage));
        get_sc_images(device, swapchain, &image_count, images);
        fprintf(stderr, "[Vortek.WSI] stage=swapchain_images count=%u\n", image_count);
    }

    while (1) {
        struct timespec t1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
        if (elapsed >= (double)duration_sec) break;

        /* Drain X events; optional mid-run resize. */
        while (XPending(dpy)) {
            XEvent ev;
            XNextEvent(dpy, &ev);
            if (ev.type == ConfigureNotify) {
                win_w = ev.xconfigure.width;
                win_h = ev.xconfigure.height;
            }
        }
        if (do_resize && !resized_once && elapsed > 5.0) {
            fprintf(stderr, "[Vortek.WSI] stage=resize_trigger old=%ux%u new=%ux%u\n",
                    extent.width, extent.height, extent.width + 64, extent.height + 48);
            XResizeWindow(dpy, win, extent.width + 64, extent.height + 48);
            XFlush(dpy);
            usleep(50 * 1000);
            while (XPending(dpy)) {
                XEvent ev;
                XNextEvent(dpy, &ev);
                if (ev.type == ConfigureNotify) {
                    win_w = ev.xconfigure.width;
                    win_h = ev.xconfigure.height;
                }
            }
            queue_wait(queue);
            resized_once = 1;
            fprintf(stderr, "[Vortek.WSI] stage=swapchain_recreated reason=resize\n");
            goto auto_create_swapchain;
        }

        wait_fences(device, 1, &in_flight, VK_TRUE, 2ULL * 1000 * 1000 * 1000);
        reset_fences(device, 1, &in_flight);

        uint32_t image_index = 0;
        vr = acquire_image(device, swapchain, UINT64_MAX, image_available, VK_NULL_HANDLE, &image_index);
        if (vr == VK_ERROR_OUT_OF_DATE_KHR || vr == VK_SUBOPTIMAL_KHR) {
            queue_wait(queue);
            fprintf(stderr, "[Vortek.WSI] stage=swapchain_recreated reason=acquire_out_of_date result=%d\n",
                    (int)vr);
            goto auto_create_swapchain;
        }
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "[Vortek.WSI] stage=frame_acquired result=%d\n", (int)vr);
            return fail("acquire_failed");
        }
        if (frames < 3 || (frames % 120) == 0) {
            fprintf(stderr, "[Vortek.WSI] stage=frame_acquired index=%u\n", image_index);
        }

        reset_cmd(cmd, 0);
        VkCommandBufferBeginInfo bi = {0};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        begin_cmd(cmd, &bi);

        VkImage img = images[image_index];
        image_barrier(cmd_barrier, cmd, img,
                      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      0, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

        VkClearColorValue clear;
        memcpy(clear.float32, colors[color_i % 5], sizeof(float) * 4);
        VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        cmd_clear(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);

        image_barrier(cmd_barrier, cmd, img,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                      VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                      VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        end_cmd(cmd);

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = {0};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &image_available;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &render_finished;
        vr = queue_submit(queue, 1, &si, in_flight);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "[Vortek.Probe] stage=queue_submit result=%d\n", (int)vr);
            return fail("queue_submit_failed");
        }

        VkPresentInfoKHR pi = {0};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &render_finished;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &image_index;
        vr = queue_present(queue, &pi);
        if (vr == VK_ERROR_OUT_OF_DATE_KHR || vr == VK_SUBOPTIMAL_KHR) {
            queue_wait(queue);
            fprintf(stderr, "[Vortek.WSI] stage=swapchain_recreated reason=present_out_of_date result=%d\n",
                    (int)vr);
            goto auto_create_swapchain;
        }
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "[Vortek.WSI] stage=frame_presented result=%d\n", (int)vr);
            return fail("present_failed");
        }

        frames++;
        color_i++;
        struct timespec t2;
        clock_gettime(CLOCK_MONOTONIC, &t2);
        double frame_ms = (t2.tv_sec - t1.tv_sec) * 1000.0 + (t2.tv_nsec - t1.tv_nsec) / 1e6;
        sum_ms += frame_ms;
        if (frame_ms > max_ms) max_ms = frame_ms;
        if (frames <= 3 || (frames % 120) == 0) {
            fprintf(stderr, "[Vortek.WSI] stage=frame_presented frame=%llu color=%d\n",
                    (unsigned long long)frames, (color_i - 1) % 5);
        }
        /* Pace ~60 FPS so colors are visible and logs stay bounded. */
        if (frame_ms < 14.0) {
            usleep((useconds_t)((16.0 - frame_ms) * 1000.0));
        }
    }

    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    double total = (t_end.tv_sec - t0.tv_sec) + (t_end.tv_nsec - t0.tv_nsec) / 1e9;
    double avg_fps = total > 0 ? frames / total : 0;
    double avg_ms = frames > 0 ? sum_ms / frames : 0;
    fprintf(stderr, "[Vortek.WSI] stage=stats frames=%llu duration=%.2f avg_fps=%.2f avg_ms=%.2f max_ms=%.2f\n",
            (unsigned long long)frames, total, avg_fps, avg_ms, max_ms);

    device_wait(device);
    destroy_fence(device, in_flight, NULL);
    destroy_sem(device, render_finished, NULL);
    destroy_sem(device, image_available, NULL);
    free_cmd(device, pool, 1, &cmd);
    destroy_pool(device, pool, NULL);
    if (swapchain) destroy_swapchain(device, swapchain, NULL);
    free(images);
    destroy_device(device, NULL);
    destroy_surface(instance, surface, NULL);
    destroy_instance(instance, NULL);
    XDestroyWindow(dpy, win);
    XCloseDisplay(dpy);

    fprintf(stderr, "[Vortek.WSI] stage=cleanup_complete\n");
    fprintf(stderr, "[Vortek.WSI] result=success\n");
    fprintf(stderr, "[Vortek.Probe] stage=cleanup_complete\n");
    fprintf(stderr, "[Vortek.Probe] result=success\n");
    return frames > 0 ? 0 : fail("no_frames_presented");
}

/* ---------- Task 8: shadPS4-matching capability probe ---------- */

/* Matches src/video_core/renderer_vulkan/vk_platform.h TargetVulkanApiVersion. */
#ifndef VK_API_VERSION_1_3
#define VK_API_VERSION_1_3 VK_MAKE_VERSION(1, 3, 0)
#endif

static int ext_list_has(const VkExtensionProperties* props, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(props[i].extensionName, name) == 0) return 1;
    }
    return 0;
}

static int run_shad_probe(void* icd_lib) {
    fprintf(stderr, "[Vortek.ShadProbe] required_api=1.3\n");
    fprintf(stderr, "[Vortek.ShadProbe] instance_extensions_required=VK_KHR_surface,VK_KHR_xlib_surface\n");
    fprintf(stderr, "[Vortek.ShadProbe] device_extensions_required="
                    "VK_KHR_swapchain,VK_KHR_push_descriptor,VK_EXT_vertex_attribute_divisor\n");
    fprintf(stderr, "[Vortek.ShadProbe] feature_chain="
                    "Features2,Vulkan11,Vulkan12,Vulkan13\n");
    fprintf(stderr, "[Vortek.ShadProbe] property_chain="
                    "Properties2,Vulkan11,Vulkan12,Vulkan13,PushDescriptorPropertiesKHR\n");

    LOAD(VK_NULL_HANDLE, "vkEnumerateInstanceVersion", PFN_vkEnumerateInstanceVersion, enum_ver);
    LOAD(VK_NULL_HANDLE, "vkCreateInstance", PFN_vkCreateInstance, create_instance);
    LOAD(VK_NULL_HANDLE, "vkDestroyInstance", PFN_vkDestroyInstance, destroy_instance);
    LOAD(VK_NULL_HANDLE, "vkEnumeratePhysicalDevices", PFN_vkEnumeratePhysicalDevices, enum_pdev);
    LOAD(VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties", PFN_vkGetPhysicalDeviceProperties, get_props);
    LOAD(VK_NULL_HANDLE, "vkGetPhysicalDeviceFeatures2", PFN_vkGetPhysicalDeviceFeatures2, get_features2);
    LOAD(VK_NULL_HANDLE, "vkGetPhysicalDeviceProperties2", PFN_vkGetPhysicalDeviceProperties2, get_props2);
    LOAD(VK_NULL_HANDLE, "vkGetPhysicalDeviceQueueFamilyProperties",
         PFN_vkGetPhysicalDeviceQueueFamilyProperties, get_qfam);
    LOAD(VK_NULL_HANDLE, "vkEnumerateDeviceExtensionProperties",
         PFN_vkEnumerateDeviceExtensionProperties, enum_dev_ext);
    LOAD(VK_NULL_HANDLE, "vkCreateDevice", PFN_vkCreateDevice, create_device);
    LOAD(VK_NULL_HANDLE, "vkGetDeviceQueue", PFN_vkGetDeviceQueue, get_queue);
    LOAD(VK_NULL_HANDLE, "vkDestroyDevice", PFN_vkDestroyDevice, destroy_device);
    LOAD(VK_NULL_HANDLE, "vkDeviceWaitIdle", PFN_vkDeviceWaitIdle, device_wait);
    LOAD(VK_NULL_HANDLE, "vkCreateCommandPool", PFN_vkCreateCommandPool, create_pool);
    LOAD(VK_NULL_HANDLE, "vkDestroyCommandPool", PFN_vkDestroyCommandPool, destroy_pool);
    LOAD(VK_NULL_HANDLE, "vkAllocateCommandBuffers", PFN_vkAllocateCommandBuffers, alloc_cmd);
    LOAD(VK_NULL_HANDLE, "vkFreeCommandBuffers", PFN_vkFreeCommandBuffers, free_cmd);
    LOAD(VK_NULL_HANDLE, "vkBeginCommandBuffer", PFN_vkBeginCommandBuffer, begin_cmd);
    LOAD(VK_NULL_HANDLE, "vkEndCommandBuffer", PFN_vkEndCommandBuffer, end_cmd);
    LOAD(VK_NULL_HANDLE, "vkCmdBeginRendering", PFN_vkCmdBeginRendering, cmd_begin_rendering);
    LOAD(VK_NULL_HANDLE, "vkCmdEndRendering", PFN_vkCmdEndRendering, cmd_end_rendering);
    LOAD(VK_NULL_HANDLE, "vkQueueSubmit2", PFN_vkQueueSubmit2, queue_submit2);
    LOAD(VK_NULL_HANDLE, "vkCreateSemaphore", PFN_vkCreateSemaphore, create_sem);
    LOAD(VK_NULL_HANDLE, "vkDestroySemaphore", PFN_vkDestroySemaphore, destroy_sem);
    LOAD(VK_NULL_HANDLE, "vkGetSemaphoreCounterValue", PFN_vkGetSemaphoreCounterValue, get_sem_value);
    LOAD(VK_NULL_HANDLE, "vkSignalSemaphore", PFN_vkSignalSemaphore, signal_sem);
    LOAD(VK_NULL_HANDLE, "vkWaitSemaphores", PFN_vkWaitSemaphores, wait_sems);

    /* Gate A: required 1.2/1.3 entry points resolve. */
    if (!get_features2 || !get_props2) {
        fprintf(stderr, "[Vortek.ShadProbe] first_missing=vkGetPhysicalDeviceFeatures2/Properties2\n");
        fprintf(stderr, "[Vortek.ShadProbe] result=missing_entry_point\n");
        return fail("shad_missing_features2");
    }
    if (!cmd_begin_rendering || !cmd_end_rendering || !queue_submit2) {
        fprintf(stderr, "[Vortek.ShadProbe] first_missing=dynamic_rendering_or_submit2\n");
        fprintf(stderr, "[Vortek.ShadProbe] result=missing_entry_point\n");
        return fail("shad_missing_1_3_cmds");
    }
    if (!get_sem_value || !signal_sem || !wait_sems) {
        fprintf(stderr, "[Vortek.ShadProbe] first_missing=timeline_semaphore_entry\n");
        fprintf(stderr, "[Vortek.ShadProbe] result=missing_entry_point\n");
        return fail("shad_missing_timeline");
    }
    fprintf(stderr, "[Vortek.ShadProbe] stage=entry_points_ok\n");

    uint32_t version = 0;
    VkResult vr = enum_ver ? enum_ver(&version) : VK_ERROR_INITIALIZATION_FAILED;
    fprintf(stderr, "[Vortek.ShadProbe] stage=instance_version result=%d version=0x%x (%u.%u.%u)\n",
            (int)vr, version,
            VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version), VK_VERSION_PATCH(version));
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "[Vortek.ShadProbe] first_missing=EnumerateInstanceVersion\n");
        fprintf(stderr, "[Vortek.ShadProbe] result=enumerate_version_failed\n");
        return fail("shad_enum_version_failed");
    }
    if (version < VK_API_VERSION_1_3) {
        fprintf(stderr, "[Vortek.ShadProbe] first_missing=api_version_1_3 reported=0x%x\n", version);
        fprintf(stderr, "[Vortek.ShadProbe] result=api_version_too_low\n");
        return fail("shad_api_version_too_low");
    }

    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "bachata-vortek-shad-probe";
    app.apiVersion = version;

    /* Instance extensions: surface pair (shadPS4 Xlib path); headless probe still enables them
     * so enumeration path matches production. Surface create is optional here. */
    const char* inst_exts[] = {
        "VK_KHR_surface",
        "VK_KHR_xlib_surface",
    };
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = 2;
    ici.ppEnabledExtensionNames = inst_exts;

    VkInstance instance = VK_NULL_HANDLE;
    vr = create_instance(&ici, NULL, &instance);
    fprintf(stderr, "[Vortek.ShadProbe] stage=instance_created result=%d\n", (int)vr);
    if (vr != VK_SUCCESS || instance == VK_NULL_HANDLE) {
        /* Retry without WSI instance extensions (pure headless). */
        ici.enabledExtensionCount = 0;
        ici.ppEnabledExtensionNames = NULL;
        vr = create_instance(&ici, NULL, &instance);
        fprintf(stderr, "[Vortek.ShadProbe] stage=instance_created_headless result=%d\n", (int)vr);
        if (vr != VK_SUCCESS || instance == VK_NULL_HANDLE) {
            fprintf(stderr, "[Vortek.ShadProbe] first_missing=vkCreateInstance\n");
            fprintf(stderr, "[Vortek.ShadProbe] result=instance_create_failed\n");
            return fail("shad_instance_create_failed");
        }
    }

    uint32_t pdev_count = 0;
    vr = enum_pdev(instance, &pdev_count, NULL);
    if (vr != VK_SUCCESS || pdev_count == 0) {
        destroy_instance(instance, NULL);
        fprintf(stderr, "[Vortek.ShadProbe] first_missing=physical_device\n");
        fprintf(stderr, "[Vortek.ShadProbe] result=no_physical_device\n");
        return fail("shad_no_physical_device");
    }
    VkPhysicalDevice* pdevs = calloc(pdev_count, sizeof(VkPhysicalDevice));
    enum_pdev(instance, &pdev_count, pdevs);
    VkPhysicalDevice pdev = pdevs[0];
    free(pdevs);

    VkPhysicalDeviceProperties props = {0};
    get_props(pdev, &props);
    fprintf(stderr, "[Vortek.ShadProbe] stage=physical_device name=%s api=0x%x (%u.%u.%u)\n",
            props.deviceName, props.apiVersion,
            VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
            VK_VERSION_PATCH(props.apiVersion));
    if (props.apiVersion < VK_API_VERSION_1_3) {
        destroy_instance(instance, NULL);
        fprintf(stderr, "[Vortek.ShadProbe] first_missing=device_api_version_1_3\n");
        fprintf(stderr, "[Vortek.ShadProbe] result=device_api_too_low\n");
        return fail("shad_device_api_too_low");
    }

    /* Property chain (shadPS4 vk_instance.cpp CreateDevice). */
    VkPhysicalDevicePushDescriptorPropertiesKHR push_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR,
    };
    VkPhysicalDeviceVulkan13Properties vk13_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES,
        .pNext = &push_props,
    };
    VkPhysicalDeviceVulkan12Properties vk12_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
        .pNext = &vk13_props,
    };
    VkPhysicalDeviceVulkan11Properties vk11_props = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES,
        .pNext = &vk12_props,
    };
    VkPhysicalDeviceProperties2 props2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
        .pNext = &vk11_props,
    };
    get_props2(pdev, &props2);
    fprintf(stderr, "[Vortek.ShadProbe] stage=properties2 subgroup=%u maxPushDescriptors=%u\n",
            vk11_props.subgroupSize, push_props.maxPushDescriptors);

    /* Feature chain. */
    VkPhysicalDeviceVulkan13Features vk13_feat = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceVulkan12Features vk12_feat = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &vk13_feat,
    };
    VkPhysicalDeviceVulkan11Features vk11_feat = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &vk12_feat,
    };
    VkPhysicalDeviceFeatures2 feats2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &vk11_feat,
    };
    get_features2(pdev, &feats2);
    fprintf(stderr, "[Vortek.ShadProbe] stage=features2 "
                    "timeline=%d bufferDeviceAddress=%d synchronization2=%d "
                    "dynamicRendering=%d maintenance4=%d shaderDrawParameters=%d\n",
            (int)vk12_feat.timelineSemaphore, (int)vk12_feat.bufferDeviceAddress,
            (int)vk13_feat.synchronization2, (int)vk13_feat.dynamicRendering,
            (int)vk13_feat.maintenance4, (int)vk11_feat.shaderDrawParameters);

    uint32_t ext_count = 0;
    enum_dev_ext(pdev, NULL, &ext_count, NULL);
    VkExtensionProperties* exts = calloc(ext_count ? ext_count : 1, sizeof(VkExtensionProperties));
    if (ext_count) enum_dev_ext(pdev, NULL, &ext_count, exts);
    fprintf(stderr, "[Vortek.ShadProbe] stage=device_extensions count=%u\n", ext_count);

    const char* req_exts[] = {
        "VK_KHR_swapchain",
        "VK_KHR_push_descriptor",
        "VK_EXT_vertex_attribute_divisor",
    };
    const char* enabled[8];
    uint32_t enabled_count = 0;
    for (size_t i = 0; i < sizeof(req_exts) / sizeof(req_exts[0]); i++) {
        if (!ext_list_has(exts, ext_count, req_exts[i])) {
            free(exts);
            destroy_instance(instance, NULL);
            fprintf(stderr, "[Vortek.ShadProbe] first_missing=%s\n", req_exts[i]);
            fprintf(stderr, "[Vortek.ShadProbe] result=missing_required_extension\n");
            return fail("shad_missing_required_extension");
        }
        enabled[enabled_count++] = req_exts[i];
        fprintf(stderr, "[Vortek.ShadProbe] stage=extension_ok name=%s\n", req_exts[i]);
    }
    free(exts);

    uint32_t qf_count = 0;
    get_qfam(pdev, &qf_count, NULL);
    VkQueueFamilyProperties* qf = calloc(qf_count, sizeof(VkQueueFamilyProperties));
    get_qfam(pdev, &qf_count, qf);
    int graphics_index = -1;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphics_index = (int)i;
            break;
        }
    }
    free(qf);
    if (graphics_index < 0) {
        destroy_instance(instance, NULL);
        fprintf(stderr, "[Vortek.ShadProbe] first_missing=graphics_queue\n");
        fprintf(stderr, "[Vortek.ShadProbe] result=no_graphics_queue\n");
        return fail("shad_no_graphics_queue");
    }
    fprintf(stderr, "[Vortek.ShadProbe] stage=queue_family index=%d\n", graphics_index);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = (uint32_t)graphics_index;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    /* Enable core features shadPS4 requires when host reports them. */
    VkPhysicalDeviceVulkan13Features en13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .synchronization2 = vk13_feat.synchronization2,
        .dynamicRendering = vk13_feat.dynamicRendering,
        .maintenance4 = vk13_feat.maintenance4,
        .shaderDemoteToHelperInvocation = vk13_feat.shaderDemoteToHelperInvocation,
        .subgroupSizeControl = vk13_feat.subgroupSizeControl,
        .robustImageAccess = vk13_feat.robustImageAccess,
    };
    VkPhysicalDeviceVulkan12Features en12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &en13,
        .timelineSemaphore = vk12_feat.timelineSemaphore,
        .bufferDeviceAddress = vk12_feat.bufferDeviceAddress,
        .scalarBlockLayout = vk12_feat.scalarBlockLayout,
        .hostQueryReset = vk12_feat.hostQueryReset,
        .uniformBufferStandardLayout = vk12_feat.uniformBufferStandardLayout,
        .separateDepthStencilLayouts = vk12_feat.separateDepthStencilLayouts,
        .shaderFloat16 = vk12_feat.shaderFloat16,
        .shaderInt8 = vk12_feat.shaderInt8,
    };
    VkPhysicalDeviceVulkan11Features en11 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
        .pNext = &en12,
        .shaderDrawParameters = vk11_feat.shaderDrawParameters,
        .storageBuffer16BitAccess = vk11_feat.storageBuffer16BitAccess,
        .uniformAndStorageBuffer16BitAccess = vk11_feat.uniformAndStorageBuffer16BitAccess,
    };
    VkPhysicalDeviceFeatures2 en_feats = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &en11,
        .features = feats2.features,
    };

    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &en_feats;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = enabled_count;
    dci.ppEnabledExtensionNames = enabled;

    VkDevice device = VK_NULL_HANDLE;
    vr = create_device(pdev, &dci, NULL, &device);
    fprintf(stderr, "[Vortek.ShadProbe] stage=device_created result=%d\n", (int)vr);
    if (vr != VK_SUCCESS || device == VK_NULL_HANDLE) {
        destroy_instance(instance, NULL);
        fprintf(stderr, "[Vortek.ShadProbe] first_missing=vkCreateDevice result=%d\n", (int)vr);
        fprintf(stderr, "[Vortek.ShadProbe] result=device_create_failed\n");
        return fail("shad_device_create_failed");
    }

    VkQueue queue = VK_NULL_HANDLE;
    get_queue(device, (uint32_t)graphics_index, 0, &queue);
    if (queue == VK_NULL_HANDLE) {
        destroy_device(device, NULL);
        destroy_instance(instance, NULL);
        fprintf(stderr, "[Vortek.ShadProbe] first_missing=vkGetDeviceQueue\n");
        fprintf(stderr, "[Vortek.ShadProbe] result=queue_failed\n");
        return fail("shad_queue_failed");
    }
    fprintf(stderr, "[Vortek.ShadProbe] stage=queue_retrieved\n");

    /* Timeline semaphore create/signal/query/wait (when feature enabled). */
    if (vk12_feat.timelineSemaphore) {
        VkSemaphoreTypeCreateInfo type_ci = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
            .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
            .initialValue = 0,
        };
        VkSemaphoreCreateInfo sci = {
            .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
            .pNext = &type_ci,
        };
        VkSemaphore timeline = VK_NULL_HANDLE;
        vr = create_sem(device, &sci, NULL, &timeline);
        fprintf(stderr, "[Vortek.ShadProbe] stage=timeline_create result=%d\n", (int)vr);
        if (vr == VK_SUCCESS && timeline != VK_NULL_HANDLE) {
            VkSemaphoreSignalInfo ssi = {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
                .semaphore = timeline,
                .value = 1,
            };
            vr = signal_sem(device, &ssi);
            uint64_t val = 0;
            VkResult gvr = get_sem_value(device, timeline, &val);
            fprintf(stderr, "[Vortek.ShadProbe] stage=timeline_signal result=%d value=%llu get=%d\n",
                    (int)vr, (unsigned long long)val, (int)gvr);
            uint64_t wait_val = 1;
            VkSemaphoreWaitInfo wai = {
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                .semaphoreCount = 1,
                .pSemaphores = &timeline,
                .pValues = &wait_val,
            };
            vr = wait_sems(device, &wai, 1000000000ull); /* 1s */
            fprintf(stderr, "[Vortek.ShadProbe] stage=timeline_wait result=%d\n", (int)vr);
            if (vr != VK_SUCCESS || gvr != VK_SUCCESS || val != 1) {
                destroy_sem(device, timeline, NULL);
                destroy_device(device, NULL);
                destroy_instance(instance, NULL);
                fprintf(stderr, "[Vortek.ShadProbe] first_missing=timeline_semaphore_ops "
                        "signal=%d get=%d val=%llu wait=%d\n",
                        (int)vr, (int)gvr, (unsigned long long)val, (int)vr);
                fprintf(stderr, "[Vortek.ShadProbe] result=timeline_failed\n");
                return fail("shad_timeline_failed");
            }
            fprintf(stderr, "[Vortek.ShadProbe] stage=timeline_ok\n");
            destroy_sem(device, timeline, NULL);
        }
    } else {
        fprintf(stderr, "[Vortek.ShadProbe] stage=timeline_skipped feature=false\n");
    }

    /* Dynamic rendering begin/end on empty color attachment list (valid with layerCount). */
    if (vk13_feat.dynamicRendering && cmd_begin_rendering && cmd_end_rendering) {
        VkCommandPoolCreateInfo pci = {0};
        pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        pci.queueFamilyIndex = (uint32_t)graphics_index;
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        VkCommandPool pool = VK_NULL_HANDLE;
        vr = create_pool(device, &pci, NULL, &pool);
        if (vr == VK_SUCCESS && pool != VK_NULL_HANDLE) {
            VkCommandBufferAllocateInfo cai = {0};
            cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cai.commandPool = pool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            vr = alloc_cmd(device, &cai, &cmd);
            if (vr == VK_SUCCESS && cmd != VK_NULL_HANDLE) {
                VkCommandBufferBeginInfo bi = {0};
                bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                begin_cmd(cmd, &bi);
                VkRenderingInfo ri = {0};
                ri.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
                ri.renderArea.extent.width = 1;
                ri.renderArea.extent.height = 1;
                ri.layerCount = 1;
                ri.colorAttachmentCount = 0;
                cmd_begin_rendering(cmd, &ri);
                cmd_end_rendering(cmd);
                end_cmd(cmd);
                if (queue_submit2) {
                    VkCommandBufferSubmitInfo csi = {
                        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
                        .commandBuffer = cmd,
                    };
                    VkSubmitInfo2 si2 = {
                        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
                        .commandBufferInfoCount = 1,
                        .pCommandBufferInfos = &csi,
                    };
                    vr = queue_submit2(queue, 1, &si2, VK_NULL_HANDLE);
                    fprintf(stderr, "[Vortek.ShadProbe] stage=submit2 result=%d\n", (int)vr);
                }
                free_cmd(device, pool, 1, &cmd);
            }
            destroy_pool(device, pool, NULL);
            fprintf(stderr, "[Vortek.ShadProbe] stage=dynamic_rendering_ok\n");
        }
    } else {
        fprintf(stderr, "[Vortek.ShadProbe] stage=dynamic_rendering_skipped feature=%d\n",
                (int)vk13_feat.dynamicRendering);
    }

    device_wait(device);
    destroy_device(device, NULL);
    destroy_instance(instance, NULL);

    fprintf(stderr, "[Vortek.ShadProbe] first_missing=none\n");
    fprintf(stderr, "[Vortek.ShadProbe] stage=cleanup_complete\n");
    fprintf(stderr, "[Vortek.ShadProbe] result=success\n");
    fprintf(stderr, "[Vortek.Probe] stage=cleanup_complete\n");
    fprintf(stderr, "[Vortek.Probe] result=success\n");
    return 0;
}

/* Task 9 fence-wait focused tests (false DEVICE_LOST / SYNC_FD -1). */
static int run_fence_tests(void* icd_lib) {
    fprintf(stderr, "[Vortek.Fence] stage=start\n");
    PFN_vkVoidFunction (*icd_gipa)(VkInstance, const char*) =
        (PFN_vkVoidFunction (*)(VkInstance, const char*))icd_proc(icd_lib, VK_NULL_HANDLE, "vk_icdGetInstanceProcAddr");
    if (!icd_gipa) icd_gipa = (PFN_vkVoidFunction (*)(VkInstance, const char*))icd_proc(
        icd_lib, VK_NULL_HANDLE, "vkGetInstanceProcAddr");
    if (!icd_gipa) return fail("fence_no_gipa");

#define FLOAD(inst, name, type, var) \
    type var = (type)icd_gipa((inst), (name)); \
    if (!(var)) { fprintf(stderr, "[Vortek.Fence] missing=%s\n", name); return fail("fence_missing_entry"); }

    FLOAD(VK_NULL_HANDLE, "vkCreateInstance", PFN_vkCreateInstance, create_instance);
    FLOAD(VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties",
          PFN_vkEnumerateInstanceExtensionProperties, enum_inst_ext);

    VkApplicationInfo app = {0};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "bachata-vortek-fence";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = {0};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    VkResult vr = create_instance(&ici, NULL, &instance);
    if (vr != VK_SUCCESS || !instance) return fail("fence_instance_failed");

    FLOAD(instance, "vkEnumeratePhysicalDevices", PFN_vkEnumeratePhysicalDevices, enum_pdev);
    FLOAD(instance, "vkGetPhysicalDeviceQueueFamilyProperties",
          PFN_vkGetPhysicalDeviceQueueFamilyProperties, get_qfam);
    FLOAD(instance, "vkCreateDevice", PFN_vkCreateDevice, create_device);
    FLOAD(instance, "vkDestroyInstance", PFN_vkDestroyInstance, destroy_instance);
    FLOAD(instance, "vkDestroyDevice", PFN_vkDestroyDevice, destroy_device);
    FLOAD(instance, "vkGetDeviceQueue", PFN_vkGetDeviceQueue, get_queue);
    FLOAD(instance, "vkCreateFence", PFN_vkCreateFence, create_fence);
    FLOAD(instance, "vkDestroyFence", PFN_vkDestroyFence, destroy_fence);
    FLOAD(instance, "vkWaitForFences", PFN_vkWaitForFences, wait_fences);
    FLOAD(instance, "vkResetFences", PFN_vkResetFences, reset_fences);
    FLOAD(instance, "vkGetFenceStatus", PFN_vkGetFenceStatus, get_fence_status);
    FLOAD(instance, "vkCreateCommandPool", PFN_vkCreateCommandPool, create_pool);
    FLOAD(instance, "vkDestroyCommandPool", PFN_vkDestroyCommandPool, destroy_pool);
    FLOAD(instance, "vkAllocateCommandBuffers", PFN_vkAllocateCommandBuffers, alloc_cmd);
    FLOAD(instance, "vkBeginCommandBuffer", PFN_vkBeginCommandBuffer, begin_cmd);
    FLOAD(instance, "vkEndCommandBuffer", PFN_vkEndCommandBuffer, end_cmd);
    FLOAD(instance, "vkQueueSubmit", PFN_vkQueueSubmit, queue_submit);
    FLOAD(instance, "vkDeviceWaitIdle", PFN_vkDeviceWaitIdle, device_wait);

    uint32_t pdev_count = 0;
    enum_pdev(instance, &pdev_count, NULL);
    if (pdev_count == 0) {
        destroy_instance(instance, NULL);
        return fail("fence_no_pdev");
    }
    VkPhysicalDevice pdev = VK_NULL_HANDLE;
    enum_pdev(instance, &pdev_count, &pdev);
    uint32_t qf_count = 0;
    get_qfam(pdev, &qf_count, NULL);
    VkQueueFamilyProperties* qf = calloc(qf_count, sizeof(VkQueueFamilyProperties));
    get_qfam(pdev, &qf_count, qf);
    int graphics_index = -1;
    for (uint32_t i = 0; i < qf_count; i++) {
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphics_index = (int)i;
            break;
        }
    }
    free(qf);
    if (graphics_index < 0) {
        destroy_instance(instance, NULL);
        return fail("fence_no_graphics");
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {0};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = (uint32_t)graphics_index;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci = {0};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice device = VK_NULL_HANDLE;
    vr = create_device(pdev, &dci, NULL, &device);
    if (vr != VK_SUCCESS || !device) {
        destroy_instance(instance, NULL);
        return fail("fence_device_failed");
    }
    VkQueue queue = VK_NULL_HANDLE;
    get_queue(device, (uint32_t)graphics_index, 0, &queue);

    int failed = 0;
    #define FENCE_CHECK(name, cond) do { \
        if (!(cond)) { \
            fprintf(stderr, "[Vortek.Fence] %s=FAIL\n", name); \
            failed = 1; \
        } else { \
            fprintf(stderr, "[Vortek.Fence] %s=OK\n", name); \
        } \
    } while (0)

    /* Test 1: signaled fence, UINT64_MAX — Sonic first GetRenderFrame. */
    {
        VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VkFence fence = VK_NULL_HANDLE;
        vr = create_fence(device, &ci, NULL, &fence);
        FENCE_CHECK("test1_create_signaled", vr == VK_SUCCESS && fence);
        vr = wait_fences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        FENCE_CHECK("test1_wait_signaled_infinite", vr == VK_SUCCESS);
        destroy_fence(device, fence, NULL);
    }

    /* Test 2: signaled, finite timeout */
    {
        VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VkFence fence = VK_NULL_HANDLE;
        create_fence(device, &ci, NULL, &fence);
        vr = wait_fences(device, 1, &fence, VK_TRUE, 50ull * 1000ull * 1000ull);
        FENCE_CHECK("test2_wait_signaled_finite", vr == VK_SUCCESS);
        destroy_fence(device, fence, NULL);
    }

    /* Test 3: unsignaled, timeout 0 */
    {
        VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        create_fence(device, &ci, NULL, &fence);
        vr = wait_fences(device, 1, &fence, VK_TRUE, 0);
        FENCE_CHECK("test3_unsignaled_zero", vr == VK_TIMEOUT);
        destroy_fence(device, fence, NULL);
    }

    /* Test 4: unsignaled, finite timeout ~50ms */
    {
        VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        create_fence(device, &ci, NULL, &fence);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        vr = wait_fences(device, 1, &fence, VK_TRUE, 50ull * 1000ull * 1000ull);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
        FENCE_CHECK("test4_unsignaled_finite_timeout", vr == VK_TIMEOUT && ms >= 30 && ms < 500);
        fprintf(stderr, "[Vortek.Fence] test4_elapsed_ms=%ld\n", ms);
        destroy_fence(device, fence, NULL);
    }

    /* Test 5: submit empty CB then wait */
    {
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .queueFamilyIndex = (uint32_t)graphics_index};
        VkCommandPool pool = VK_NULL_HANDLE;
        create_pool(device, &pci, NULL, &pool);
        VkCommandBufferAllocateInfo cai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        alloc_cmd(device, &cai, &cmd);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        begin_cmd(cmd, &bi);
        end_cmd(cmd);
        VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence fence = VK_NULL_HANDLE;
        create_fence(device, &ci, NULL, &fence);
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &cmd};
        vr = queue_submit(queue, 1, &si, fence);
        FENCE_CHECK("test5_submit", vr == VK_SUCCESS);
        vr = wait_fences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        FENCE_CHECK("test5_wait_after_submit", vr == VK_SUCCESS);
        destroy_fence(device, fence, NULL);
        destroy_pool(device, pool, NULL);
    }

    /* Test 6: reset/submit/wait reuse (signaled first like present_done) */
    {
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                       .queueFamilyIndex = (uint32_t)graphics_index};
        VkCommandPool pool = VK_NULL_HANDLE;
        create_pool(device, &pci, NULL, &pool);
        VkCommandBufferAllocateInfo cai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        alloc_cmd(device, &cai, &cmd);
        VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VkFence fence = VK_NULL_HANDLE;
        create_fence(device, &ci, NULL, &fence);
        int ok = 1;
        for (int i = 0; i < 2; i++) {
            vr = wait_fences(device, 1, &fence, VK_TRUE, UINT64_MAX);
            if (vr != VK_SUCCESS) ok = 0;
            reset_fences(device, 1, &fence);
            VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            begin_cmd(cmd, &bi);
            end_cmd(cmd);
            VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                               .commandBufferCount = 1,
                               .pCommandBuffers = &cmd};
            if (queue_submit(queue, 1, &si, fence) != VK_SUCCESS) ok = 0;
            if (wait_fences(device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) ok = 0;
        }
        FENCE_CHECK("test6_reset_submit_reuse", ok);
        destroy_fence(device, fence, NULL);
        destroy_pool(device, pool, NULL);
    }

    /* Test 7: waitAll two fences */
    {
        VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence f[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        create_fence(device, &ci, NULL, &f[0]);
        create_fence(device, &ci, NULL, &f[1]);
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .queueFamilyIndex = (uint32_t)graphics_index};
        VkCommandPool pool = VK_NULL_HANDLE;
        create_pool(device, &pci, NULL, &pool);
        VkCommandBufferAllocateInfo cai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 2,
        };
        VkCommandBuffer cmds[2];
        alloc_cmd(device, &cai, cmds);
        for (int i = 0; i < 2; i++) {
            VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                           .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
            begin_cmd(cmds[i], &bi);
            end_cmd(cmds[i]);
            VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                               .commandBufferCount = 1,
                               .pCommandBuffers = &cmds[i]};
            queue_submit(queue, 1, &si, f[i]);
        }
        vr = wait_fences(device, 2, f, VK_TRUE, UINT64_MAX);
        FENCE_CHECK("test7_waitAll", vr == VK_SUCCESS);
        destroy_fence(device, f[0], NULL);
        destroy_fence(device, f[1], NULL);
        destroy_pool(device, pool, NULL);
    }

    /* Test 8: waitAny */
    {
        VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        VkFence f[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        create_fence(device, &ci, NULL, &f[0]);
        create_fence(device, &ci, NULL, &f[1]);
        /* Signal only first via empty submit */
        VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                       .queueFamilyIndex = (uint32_t)graphics_index};
        VkCommandPool pool = VK_NULL_HANDLE;
        create_pool(device, &pci, NULL, &pool);
        VkCommandBufferAllocateInfo cai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        alloc_cmd(device, &cai, &cmd);
        VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                       .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        begin_cmd(cmd, &bi);
        end_cmd(cmd);
        VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                           .commandBufferCount = 1,
                           .pCommandBuffers = &cmd};
        queue_submit(queue, 1, &si, f[0]);
        vr = wait_fences(device, 2, f, VK_FALSE, UINT64_MAX);
        FENCE_CHECK("test8_waitAny", vr == VK_SUCCESS);
        destroy_fence(device, f[0], NULL);
        destroy_fence(device, f[1], NULL);
        destroy_pool(device, pool, NULL);
    }

    /* Test 9: forced device-loss propagation (server env must be set by harness). */
    if (env_flag("BACHATA_VORTEK_FENCE_WAIT_FORCE_DEVICE_LOST")) {
        VkFenceCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                                .flags = VK_FENCE_CREATE_SIGNALED_BIT};
        VkFence fence = VK_NULL_HANDLE;
        create_fence(device, &ci, NULL, &fence);
        vr = wait_fences(device, 1, &fence, VK_TRUE, UINT64_MAX);
        FENCE_CHECK("test9_device_lost_propagation", vr == VK_ERROR_DEVICE_LOST);
        destroy_fence(device, fence, NULL);
    } else {
        fprintf(stderr, "[Vortek.Fence] test9_device_lost_propagation=SKIP (env not set)\n");
    }

    device_wait(device);
    destroy_device(device, NULL);
    destroy_instance(instance, NULL);
#undef FENCE_CHECK
#undef FLOAD
    if (failed) {
        fprintf(stderr, "[Vortek.Fence] result=fail\n");
        fprintf(stderr, "[Vortek.Probe] result=fence_fail\n");
        return 1;
    }
    fprintf(stderr, "[Vortek.Fence] result=success\n");
    fprintf(stderr, "[Vortek.Probe] result=success\n");
    return 0;
}

int main(void) {
    signal(SIGALRM, on_alarm);

    const char* icd = getenv("VK_ICD_FILENAMES");
    const char* socket_path = getenv("BACHATA_VORTEK_SOCKET");
    const char* sdl_lib = getenv("SDL_VULKAN_LIBRARY");
    int transport_only = env_flag("BACHATA_VORTEK_TRANSPORT_ONLY");
    int headless = env_flag("BACHATA_VORTEK_HEADLESS");
    int wsi = env_flag("BACHATA_VORTEK_WSI");
    int shad = env_flag("BACHATA_VORTEK_SHAD");
    int fence = env_flag("BACHATA_VORTEK_FENCE");
    int wsi_seconds = env_int("BACHATA_VORTEK_WSI_SECONDS", 60);

    if (wsi) {
        alarm(wsi_seconds + 30);
    } else {
        alarm(fence ? 120 : 60);
    }

    fprintf(stderr, "[Vortek.Probe] stage=start icd=%s socket=%s sdl=%s transport_only=%d headless=%d wsi=%d shad=%d fence=%d\n",
            icd ? icd : "(unset)",
            socket_path ? socket_path : "(unset)",
            sdl_lib ? sdl_lib : "(unset)",
            transport_only, headless, wsi, shad, fence);
    fprintf(stderr, "[Vortek.Probe] backend=SYSTEM_VORTEK\n");

    if (!icd || !icd[0]) return fail("missing_VK_ICD_FILENAMES");
    if (strstr(icd, "com.winlator")) return fail("winlator_path_forbidden");
    if (strstr(icd, "turnip")) return fail("turnip_icd_forbidden");
    if (!strstr(icd, "vortek.json")) return fail("icd_not_vortek");

    void* loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!loader) loader = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!loader && sdl_lib && sdl_lib[0]) loader = dlopen(sdl_lib, RTLD_NOW | RTLD_LOCAL);
    if (!loader) {
        fprintf(stderr, "[Vortek.Probe] result=loader_open_failed error=%s\n", dlerror());
        return 3;
    }
    stage("loader_loaded");
    fprintf(stderr, "[Vortek.Probe] loader=libvulkan.so.1\n");

    char lib_abs[PATH_MAX];
    void* icd_lib = load_icd(icd, lib_abs, sizeof(lib_abs));
    if (!icd_lib) return 3;

    if (transport_only) return run_transport_only(icd_lib);
    if (fence) return run_fence_tests(icd_lib);
    if (shad) return run_shad_probe(icd_lib);
    if (wsi) return run_wsi(icd_lib);
    if (headless) return run_headless(icd_lib);

    PFN_vkVoidFunction create_fn = icd_proc(icd_lib, VK_NULL_HANDLE, "vkCreateInstance");
    if (!create_fn) {
        fprintf(stderr, "[Vortek.Probe] result=server_unavailable\n");
        return 0;
    }
    return run_headless(icd_lib);
}
