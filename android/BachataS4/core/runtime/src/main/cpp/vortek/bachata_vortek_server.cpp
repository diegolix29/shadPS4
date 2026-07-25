#include "bachata_vortek_server.h"
#include "bachata_vortek_logging.h"
#include "bachata_vortek_protocol.h"
#include "bachata_vortek_io.h"
#include "bachata_vk_context.h"
#include "ring_buffer.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <climits>
#include <cstdint>
#include <mutex>
#include <string>

namespace {

constexpr size_t kMaxSunPath = sizeof(sockaddr_un{}.sun_path) - 1;

struct ServerContext {
    std::mutex mu;
    std::atomic<int> state{BACHATA_VORTEK_STATE_STOPPED};
    std::atomic<int> last_error{BACHATA_VORTEK_OK};
    std::atomic<bool> stop_requested{false};

    pthread_t listen_thread{};
    bool listen_thread_started = false;
    pthread_t request_thread{};
    bool request_thread_started = false;

    std::string socket_path;
    std::string expected_client_build;
    std::string server_build;
    std::string host_api_version;

    int listen_fd = -1;
    int client_fd = -1;
    int server_ring_fd = -1;
    int client_ring_fd = -1;
    void* server_ring = nullptr;
    void* client_ring = nullptr;

    void* vulkan_lib = nullptr;
    using GetInstanceProcAddrFn = void* (*)(void*, const char*);
    GetInstanceProcAddrFn vkGetInstanceProcAddr = nullptr;
    // Typed loosely to avoid full vulkan headers dependency for loader probe.
    using EnumerateInstanceVersionFn = int (*)(uint32_t*);
    EnumerateInstanceVersionFn vkEnumerateInstanceVersion = nullptr;
    using CreateInstanceFn = int (*)(const void*, const void*, void**);
    CreateInstanceFn vkCreateInstance = nullptr;
    using DestroyInstanceFn = void (*)(void*, const void*);
    DestroyInstanceFn vkDestroyInstance = nullptr;
    void* vk_instance = nullptr;
};

ServerContext g_server;

void set_state(int s) { g_server.state.store(s); }
void set_error(int e) { g_server.last_error.store(e); }

void close_fd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

void free_rings_locked() {
    bachata_vk_stop_dispatch();
    if (g_server.server_ring) {
        bachata_vortek_ring_set_exit(g_server.server_ring);
        bachata_vortek_ring_free(g_server.server_ring);
        g_server.server_ring = nullptr;
    }
    if (g_server.client_ring) {
        bachata_vortek_ring_set_exit(g_server.client_ring);
        bachata_vortek_ring_free(g_server.client_ring);
        g_server.client_ring = nullptr;
    }
    close_fd(g_server.server_ring_fd);
    close_fd(g_server.client_ring_fd);
}

void destroy_vulkan_locked() {
    if (g_server.vk_instance && g_server.vkDestroyInstance) {
        g_server.vkDestroyInstance(g_server.vk_instance, nullptr);
        g_server.vk_instance = nullptr;
    }
    g_server.vkGetInstanceProcAddr = nullptr;
    g_server.vkEnumerateInstanceVersion = nullptr;
    g_server.vkCreateInstance = nullptr;
    g_server.vkDestroyInstance = nullptr;
    if (g_server.vulkan_lib) {
        dlclose(g_server.vulkan_lib);
        g_server.vulkan_lib = nullptr;
    }
}

void unlink_socket_if_owned() {
    if (g_server.socket_path.empty()) return;
    struct stat st {};
    if (lstat(g_server.socket_path.c_str(), &st) != 0) return;
    if (S_ISLNK(st.st_mode)) {
        BACHATA_VORTEK_ERR("socket=skip_unlink reason=symlink path_present");
        return;
    }
    if (!S_ISSOCK(st.st_mode)) {
        BACHATA_VORTEK_ERR("socket=skip_unlink reason=not_socket");
        return;
    }
    if (unlink(g_server.socket_path.c_str()) == 0) {
        BACHATA_VORTEK_LOG("socket=removed");
    }
}

int validate_socket_path(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        BACHATA_VORTEK_ERR("stage=validate error=socket_path_empty");
        return BACHATA_VORTEK_ERR_SOCKET_PATH;
    }
    if (path[0] != '/') {
        BACHATA_VORTEK_ERR("stage=validate error=socket_path_not_absolute");
        return BACHATA_VORTEK_ERR_SOCKET_PATH;
    }
    if (strstr(path, "com.winlator") != nullptr) {
        BACHATA_VORTEK_ERR("stage=validate error=winlator_path_forbidden");
        return BACHATA_VORTEK_ERR_SOCKET_PATH;
    }
    const size_t len = strlen(path);
    if (len > kMaxSunPath) {
        BACHATA_VORTEK_ERR("stage=validate error=socket_path_too_long len=%zu max=%zu", len, kMaxSunPath);
        return BACHATA_VORTEK_ERR_SOCKET_TOO_LONG;
    }

    std::string parent(path);
    const auto slash = parent.find_last_of('/');
    if (slash == std::string::npos || slash == 0) {
        BACHATA_VORTEK_ERR("stage=validate error=parent_missing");
        return BACHATA_VORTEK_ERR_PARENT_MISSING;
    }
    parent.resize(slash);
    struct stat pst {};
    if (stat(parent.c_str(), &pst) != 0 || !S_ISDIR(pst.st_mode)) {
        BACHATA_VORTEK_ERR("stage=validate error=parent_missing errno=%d", errno);
        return BACHATA_VORTEK_ERR_PARENT_MISSING;
    }

    struct stat st {};
    if (lstat(path, &st) == 0) {
        if (S_ISLNK(st.st_mode)) {
            BACHATA_VORTEK_ERR("stage=validate error=unsafe_existing_socket_path reason=symlink");
            return BACHATA_VORTEK_ERR_UNSAFE_EXISTING;
        }
        if (S_ISREG(st.st_mode)) {
            BACHATA_VORTEK_ERR("stage=validate error=unsafe_existing_socket_path reason=regular_file");
            return BACHATA_VORTEK_ERR_UNSAFE_EXISTING;
        }
        if (S_ISSOCK(st.st_mode)) {
            // Safe to replace a prior session socket.
            unlink(path);
        } else {
            BACHATA_VORTEK_ERR("stage=validate error=unsafe_existing_socket_path reason=other");
            return BACHATA_VORTEK_ERR_UNSAFE_EXISTING;
        }
    }
    return BACHATA_VORTEK_OK;
}

int load_vulkan_loader() {
    BACHATA_VORTEK_LOG("state=starting");
    void* lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        BACHATA_VORTEK_ERR("stage=loader error=dlopen_failed detail=%s", dlerror());
        return BACHATA_VORTEK_ERR_LOADER;
    }
    g_server.vulkan_lib = lib;
    BACHATA_VORTEK_LOG("host_loader=libvulkan.so");

    auto gipa = reinterpret_cast<ServerContext::GetInstanceProcAddrFn>(dlsym(lib, "vkGetInstanceProcAddr"));
    if (!gipa) {
        BACHATA_VORTEK_ERR("stage=loader error=missing_vkGetInstanceProcAddr");
        return BACHATA_VORTEK_ERR_SYMBOL;
    }
    g_server.vkGetInstanceProcAddr = gipa;
    BACHATA_VORTEK_LOG("vkGetInstanceProcAddr=resolved");

    auto enum_ver = reinterpret_cast<ServerContext::EnumerateInstanceVersionFn>(
        gipa(nullptr, "vkEnumerateInstanceVersion"));
    if (!enum_ver) {
        // Fallback: some loaders expose only via dlsym.
        enum_ver = reinterpret_cast<ServerContext::EnumerateInstanceVersionFn>(
            dlsym(lib, "vkEnumerateInstanceVersion"));
    }
    if (!enum_ver) {
        BACHATA_VORTEK_ERR("stage=loader error=missing_vkEnumerateInstanceVersion");
        return BACHATA_VORTEK_ERR_SYMBOL;
    }
    g_server.vkEnumerateInstanceVersion = enum_ver;
    BACHATA_VORTEK_LOG("vkEnumerateInstanceVersion=resolved");

    uint32_t version = 0;
    const int vr = enum_ver(&version);
    if (vr == 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u.%u.%u",
                 (version >> 22) & 0x7fu,
                 (version >> 12) & 0x3ffu,
                 version & 0xfffu);
        g_server.host_api_version = buf;
        BACHATA_VORTEK_LOG("host_instance_version=%s", buf);
    } else {
        g_server.host_api_version = "unknown";
        BACHATA_VORTEK_LOG("host_instance_version=unknown vk_result=%d", vr);
    }

    g_server.vkCreateInstance = reinterpret_cast<ServerContext::CreateInstanceFn>(
        gipa(nullptr, "vkCreateInstance"));
    g_server.vkDestroyInstance = reinterpret_cast<ServerContext::DestroyInstanceFn>(
        gipa(nullptr, "vkDestroyInstance"));

    // Prefer creating a headless VkInstance when symbols exist (no WSI required).
    if (g_server.vkCreateInstance) {
        // Minimal VkApplicationInfo / VkInstanceCreateInfo layout (Vulkan 1.0 compatible).
        struct AppInfo {
            int32_t sType; // VK_STRUCTURE_TYPE_APPLICATION_INFO = 0
            const void* pNext;
            const char* pApplicationName;
            uint32_t applicationVersion;
            const char* pEngineName;
            uint32_t engineVersion;
            uint32_t apiVersion;
        } app{};
        app.sType = 0;
        app.pApplicationName = "bachata-vortek-server";
        // Bootstrap with host max (capped at 1.3) so physical-device 1.2/1.3
        // feature/property queries work during server-side diagnostics.
        {
            uint32_t host_ver = version ? version : ((1u << 22) | (1u << 12) | 0u);
            const uint32_t cap_1_3 = (1u << 22) | (3u << 12) | 0u; // 1.3.0
            app.apiVersion = host_ver < cap_1_3 ? host_ver : cap_1_3;
            if (app.apiVersion < ((1u << 22) | (1u << 12) | 0u)) {
                app.apiVersion = (1u << 22) | (1u << 12) | 0u;
            }
            BACHATA_VORTEK_LOG("host_bootstrap_api=%u.%u.%u",
                               (app.apiVersion >> 22) & 0x7fu,
                               (app.apiVersion >> 12) & 0x3ffu,
                               app.apiVersion & 0xfffu);
        }

        struct CreateInfo {
            int32_t sType; // VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO = 1
            const void* pNext;
            uint32_t flags;
            const AppInfo* pApplicationInfo;
            uint32_t enabledLayerCount;
            const char* const* ppEnabledLayerNames;
            uint32_t enabledExtensionCount;
            const char* const* ppEnabledExtensionNames;
        } ci{};
        ci.sType = 1;
        ci.pApplicationInfo = &app;

        void* instance = nullptr;
        const int cr = g_server.vkCreateInstance(&ci, nullptr, &instance);
        if (cr == 0 && instance) {
            g_server.vk_instance = instance;
            BACHATA_VORTEK_LOG("host_instance=created");
        } else {
            BACHATA_VORTEK_LOG("host_instance=skipped vk_result=%d", cr);
        }
    }

    set_state(BACHATA_VORTEK_STATE_LOADER_READY);
    return BACHATA_VORTEK_OK;
}

int handle_handshake(int client_fd, int body_len) {
    if (body_len != (int)sizeof(BachataVortekHandshakeRequest)) {
        BACHATA_VORTEK_ERR("handshake=rejected reason=bad_size size=%d", body_len);
        return BACHATA_VORTEK_ERR_PROTOCOL;
    }
    BachataVortekHandshakeRequest req{};
    if (bachata_vortek_sock_read(client_fd, reinterpret_cast<char*>(&req), body_len) != body_len) {
        BACHATA_VORTEK_ERR("handshake=rejected reason=client_disconnected stage=read_body");
        return BACHATA_VORTEK_ERR_CLIENT_DISCONNECTED;
    }

    BACHATA_VORTEK_LOG("handshake_client_build=%s", req.client_build_id);
    BACHATA_VORTEK_LOG("handshake_server_build=%s", g_server.server_build.c_str());
    BACHATA_VORTEK_LOG("protocol_client=%u.%u", req.proto_major, req.proto_minor);
    BACHATA_VORTEK_LOG("protocol_server=%u.%u", BACHATA_VORTEK_PROTO_MAJOR, BACHATA_VORTEK_PROTO_MINOR);

    uint16_t status = BACHATA_VORTEK_HANDSHAKE_OK;
    const char* reason = "ok";
    if (req.magic != BACHATA_VORTEK_MAGIC) {
        status = BACHATA_VORTEK_HANDSHAKE_MISMATCH;
        reason = "magic_mismatch";
    } else if (req.proto_major != BACHATA_VORTEK_PROTO_MAJOR) {
        status = BACHATA_VORTEK_HANDSHAKE_MISMATCH;
        reason = "protocol_major_mismatch";
    } else if (req.pointer_size != sizeof(void*)) {
        status = BACHATA_VORTEK_HANDSHAKE_MISMATCH;
        reason = "pointer_size_mismatch";
    } else if (req.endianness != BACHATA_VORTEK_ENDIAN_LITTLE) {
        status = BACHATA_VORTEK_HANDSHAKE_MISMATCH;
        reason = "endianness_mismatch";
    } else if (req.proto_minor != BACHATA_VORTEK_PROTO_MINOR) {
        BACHATA_VORTEK_LOG("protocol_minor_diff client=%u server=%u", req.proto_minor, BACHATA_VORTEK_PROTO_MINOR);
    }

    if (!g_server.expected_client_build.empty() &&
        g_server.expected_client_build != "*" &&
        g_server.expected_client_build != req.client_build_id) {
        // Soft: log only; do not reject on build id unless exact mismatch required later.
        BACHATA_VORTEK_LOG("handshake_client_build_note=diff expected=%s",
                           g_server.expected_client_build.c_str());
    }

    BachataVortekHandshakeResponse resp{};
    resp.magic = BACHATA_VORTEK_MAGIC;
    resp.proto_major = BACHATA_VORTEK_PROTO_MAJOR;
    resp.proto_minor = BACHATA_VORTEK_PROTO_MINOR;
    resp.status = status;

    char header[BACHATA_VORTEK_HEADER_SIZE];
    *reinterpret_cast<int*>(header + 0) = REQUEST_CODE_BACHATA_HANDSHAKE;
    *reinterpret_cast<int*>(header + 4) = (int)sizeof(resp);
    if (bachata_vortek_sock_write(client_fd, header, BACHATA_VORTEK_HEADER_SIZE) != BACHATA_VORTEK_HEADER_SIZE ||
        bachata_vortek_sock_write(client_fd, reinterpret_cast<char*>(&resp), (int)sizeof(resp)) != (int)sizeof(resp)) {
        BACHATA_VORTEK_ERR("handshake=rejected reason=write_failed");
        return BACHATA_VORTEK_ERR_CLIENT_DISCONNECTED;
    }

    if (status != BACHATA_VORTEK_HANDSHAKE_OK) {
        BACHATA_VORTEK_ERR("handshake=rejected reason=%s", reason);
        return BACHATA_VORTEK_ERR_PROTOCOL;
    }
    BACHATA_VORTEK_LOG("handshake=accepted");
    return BACHATA_VORTEK_OK;
}

void* request_thread_main(void*) {
    // Task 6+: real dispatch lives in bachata_vk_start_dispatch (separate thread).
    // This thread only waits for EXIT/stop so free_rings can join cleanly if used.
    while (!g_server.stop_requested.load()) {
        if (g_server.server_ring && bachata_vortek_ring_has_exit(g_server.server_ring)) {
            break;
        }
        usleep(50 * 1000);
    }
    return nullptr;
}

int handle_create_context(int client_fd) {
    BACHATA_VORTEK_LOG("context=create_requested");
    free_rings_locked();

    int shmFds[2];
    shmFds[0] = bachata_vortek_ashmem_create(
        "vt-server-ring", bachata_vortek_ring_shm_size(BACHATA_VORTEK_SERVER_RING_BYTES));
    shmFds[1] = bachata_vortek_ashmem_create(
        "vt-client-ring", bachata_vortek_ring_shm_size(BACHATA_VORTEK_CLIENT_RING_BYTES));
    if (shmFds[0] < 0 || shmFds[1] < 0) {
        BACHATA_VORTEK_ERR("stage=rings error=ashmem_failed");
        close_fd(shmFds[0]);
        close_fd(shmFds[1]);
        return BACHATA_VORTEK_ERR_CONTEXT;
    }

    g_server.server_ring = bachata_vortek_ring_create(shmFds[0], BACHATA_VORTEK_SERVER_RING_BYTES);
    g_server.client_ring = bachata_vortek_ring_create(shmFds[1], BACHATA_VORTEK_CLIENT_RING_BYTES);
    if (!g_server.server_ring || !g_server.client_ring) {
        BACHATA_VORTEK_ERR("stage=rings error=map_failed");
        free_rings_locked();
        close(shmFds[0]);
        close(shmFds[1]);
        return BACHATA_VORTEK_ERR_CONTEXT;
    }

    g_server.server_ring_fd = shmFds[0];
    g_server.client_ring_fd = shmFds[1];

    BACHATA_VORTEK_LOG("rings=created server_bytes=%u client_bytes=%u",
                       BACHATA_VORTEK_SERVER_RING_BYTES, BACHATA_VORTEK_CLIENT_RING_BYTES);

    const int sent = bachata_vortek_send_fds(client_fd, shmFds, 2);
    if (sent < 0) {
        BACHATA_VORTEK_ERR("stage=fds error=send_failed errno=%d", errno);
        free_rings_locked();
        return BACHATA_VORTEK_ERR_CLIENT_DISCONNECTED;
    }
    BACHATA_VORTEK_LOG("fds=sent count=2");

    // Stop any prior dispatch before starting a new request-handler thread.
    bachata_vk_stop_dispatch();
    if (g_server.request_thread_started) {
        pthread_join(g_server.request_thread, nullptr);
        g_server.request_thread_started = false;
    }

    if (bachata_vk_start_dispatch(
            static_cast<RingBuffer*>(g_server.server_ring),
            static_cast<RingBuffer*>(g_server.client_ring),
            client_fd) != 0) {
        BACHATA_VORTEK_ERR("stage=dispatch error=start_failed");
        free_rings_locked();
        return BACHATA_VORTEK_ERR_INTERNAL;
    }

    // Keep a lightweight watcher thread for EXIT signaling / lifecycle join.
    if (pthread_create(&g_server.request_thread, nullptr, request_thread_main, nullptr) != 0) {
        BACHATA_VORTEK_ERR("stage=request_thread error=create_failed");
        bachata_vk_stop_dispatch();
        free_rings_locked();
        return BACHATA_VORTEK_ERR_INTERNAL;
    }
    g_server.request_thread_started = true;
    set_state(BACHATA_VORTEK_STATE_CONTEXT_READY);
    BACHATA_VORTEK_LOG("state=context_ready");
    return BACHATA_VORTEK_OK;
}

int serve_client(int client_fd) {
    g_server.client_fd = client_fd;
    set_state(BACHATA_VORTEK_STATE_CLIENT_CONNECTED);
    BACHATA_VORTEK_LOG("state=client_connected");

    bool handshake_done = false;
    bool context_done = false;

    while (!g_server.stop_requested.load() && !context_done) {
        char header[BACHATA_VORTEK_HEADER_SIZE];
        const int n = bachata_vortek_sock_read(client_fd, header, BACHATA_VORTEK_HEADER_SIZE);
        if (n != BACHATA_VORTEK_HEADER_SIZE) {
            BACHATA_VORTEK_ERR("stage=client error=client_disconnected errno=%d", errno);
            return BACHATA_VORTEK_ERR_CLIENT_DISCONNECTED;
        }
        const int code = *reinterpret_cast<int*>(header + 0);
        const int length = *reinterpret_cast<int*>(header + 4);

        if (code == REQUEST_CODE_BACHATA_HANDSHAKE) {
            const int hr = handle_handshake(client_fd, length);
            if (hr != BACHATA_VORTEK_OK) return hr;
            handshake_done = true;
            continue;
        }
        if (code == REQUEST_CODE_CREATE_CONTEXT) {
            if (!handshake_done) {
                // Allow CREATE_CONTEXT without handshake when client disables it.
                BACHATA_VORTEK_LOG("handshake=skipped");
            }
            if (length != 0) {
                // Drain unexpected body.
                std::string drain(static_cast<size_t>(length), '\0');
                bachata_vortek_sock_read(client_fd, drain.data(), length);
            }
            const int cr = handle_create_context(client_fd);
            if (cr != BACHATA_VORTEK_OK) return cr;
            context_done = true;
            // Fall through: keep this client session open while ring dispatch runs.
            break;
        }

        BACHATA_VORTEK_ERR("stage=client error=unexpected_request code=%d", code);
        // Drain body if any.
        if (length > 0) {
            std::string drain(static_cast<size_t>(length), '\0');
            bachata_vortek_sock_read(client_fd, drain.data(), length);
        }
        return BACHATA_VORTEK_ERR_PROTOCOL;
    }

    if (context_done && !g_server.stop_requested.load()) {
        // Control socket remains open for SEND_EXTRA_DATA (large Vulkan payloads).
        // Ring-based Vulkan dispatch continues on the dedicated request thread.
        BACHATA_VORTEK_LOG("session=active waiting_for_client_exit");
        while (!g_server.stop_requested.load()) {
            char header[BACHATA_VORTEK_HEADER_SIZE];
            const int n = bachata_vortek_sock_read(client_fd, header, BACHATA_VORTEK_HEADER_SIZE);
            if (n == 0) {
                BACHATA_VORTEK_LOG("session=client_closed");
                break;
            }
            if (n != BACHATA_VORTEK_HEADER_SIZE) {
                if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                    usleep(20 * 1000);
                    continue;
                }
                if (n < 0 && errno == 0) {
                    BACHATA_VORTEK_LOG("session=client_closed");
                } else {
                    BACHATA_VORTEK_LOG("session=client_recv_err n=%d errno=%d", n, errno);
                }
                break;
            }
            const int packed = *reinterpret_cast<int*>(header + 0);
            const int length = *reinterpret_cast<int*>(header + 4);
            // PACK16(SEND_EXTRA_DATA, requestId) from matched client vt_send.
            if (packed > INT16_MAX && (packed >> 16) == REQUEST_CODE_SEND_EXTRA_DATA) {
                const uint16_t requestId = static_cast<uint16_t>(packed & 0xffff);
                if (!bachata_vk_handle_extra_data(requestId, length)) {
                    BACHATA_VORTEK_ERR("stage=extra_data error=failed id=%u size=%d",
                                      static_cast<unsigned>(requestId), length);
                    return BACHATA_VORTEK_ERR_PROTOCOL;
                }
                continue;
            }
            BACHATA_VORTEK_ERR("stage=client error=unexpected_post_context code=%d length=%d",
                              packed, length);
            if (length > 0) {
                std::string drain(static_cast<size_t>(length), '\0');
                bachata_vortek_sock_read(client_fd, drain.data(), length);
            }
            return BACHATA_VORTEK_ERR_PROTOCOL;
        }
    }
    return BACHATA_VORTEK_OK;
}

void* listen_thread_main(void*) {
    while (!g_server.stop_requested.load()) {
        const int client = accept(g_server.listen_fd, nullptr, nullptr);
        if (client < 0) {
            if (g_server.stop_requested.load()) break;
            if (errno == EINTR) continue;
            BACHATA_VORTEK_ERR("stage=accept error=failed errno=%d", errno);
            set_error(BACHATA_VORTEK_ERR_LISTEN);
            set_state(BACHATA_VORTEK_STATE_FAILED);
            break;
        }
        // One client at a time for Task 4.
        const int result = serve_client(client);
        close(client);
        g_server.client_fd = -1;
        if (result != BACHATA_VORTEK_OK && result != BACHATA_VORTEK_ERR_CLIENT_DISCONNECTED) {
            set_error(result);
            // Stay SOCKET_READY for sequential sessions unless stop requested.
            if (!g_server.stop_requested.load() && g_server.state.load() != BACHATA_VORTEK_STATE_FAILED) {
                set_state(BACHATA_VORTEK_STATE_SOCKET_READY);
            }
        } else if (!g_server.stop_requested.load()) {
            // After disconnect, tear request thread/rings for next session.
            if (g_server.request_thread_started) {
                if (g_server.server_ring) bachata_vortek_ring_set_exit(g_server.server_ring);
                if (g_server.client_ring) bachata_vortek_ring_set_exit(g_server.client_ring);
                pthread_join(g_server.request_thread, nullptr);
                g_server.request_thread_started = false;
            }
            free_rings_locked();
            set_state(BACHATA_VORTEK_STATE_SOCKET_READY);
            BACHATA_VORTEK_LOG("state=socket_ready note=ready_for_next_client");
        }
    }
    return nullptr;
}

void full_cleanup_locked() {
    g_server.stop_requested.store(true);
    if (g_server.listen_fd >= 0) {
        shutdown(g_server.listen_fd, SHUT_RDWR);
    }
    if (g_server.client_fd >= 0) {
        shutdown(g_server.client_fd, SHUT_RDWR);
        close_fd(g_server.client_fd);
    }
    if (g_server.server_ring) bachata_vortek_ring_set_exit(g_server.server_ring);
    if (g_server.client_ring) bachata_vortek_ring_set_exit(g_server.client_ring);

    if (g_server.request_thread_started) {
        pthread_join(g_server.request_thread, nullptr);
        g_server.request_thread_started = false;
    }
    if (g_server.listen_thread_started) {
        pthread_join(g_server.listen_thread, nullptr);
        g_server.listen_thread_started = false;
    }

    free_rings_locked();
    close_fd(g_server.listen_fd);
    unlink_socket_if_owned();
    destroy_vulkan_locked();
    g_server.stop_requested.store(false);
    set_state(BACHATA_VORTEK_STATE_STOPPED);
    BACHATA_VORTEK_LOG("resources=released");
    BACHATA_VORTEK_LOG("state=stopped");
}

} // namespace

extern "C" int bachata_vortek_server_start(
    const char* socket_path,
    const char* expected_client_build,
    const char* server_build) {
    std::lock_guard<std::mutex> lock(g_server.mu);
    const int st = g_server.state.load();
    if (st != BACHATA_VORTEK_STATE_STOPPED && st != BACHATA_VORTEK_STATE_FAILED) {
        BACHATA_VORTEK_ERR("stage=start error=already_running state=%d", st);
        return BACHATA_VORTEK_ALREADY_RUNNING;
    }
    // Ensure prior failed state cleaned.
    full_cleanup_locked();
    set_error(BACHATA_VORTEK_OK);
    set_state(BACHATA_VORTEK_STATE_STARTING);

    const int v = validate_socket_path(socket_path);
    if (v != BACHATA_VORTEK_OK) {
        set_error(v);
        set_state(BACHATA_VORTEK_STATE_FAILED);
        return v;
    }

    g_server.socket_path = socket_path;
    g_server.expected_client_build = expected_client_build ? expected_client_build : "";
    g_server.server_build = server_build ? server_build : "bachata-vortek-server";

    const int lr = load_vulkan_loader();
    if (lr != BACHATA_VORTEK_OK) {
        set_error(lr);
        set_state(BACHATA_VORTEK_STATE_FAILED);
        destroy_vulkan_locked();
        return lr;
    }

    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        BACHATA_VORTEK_ERR("stage=socket error=create_failed errno=%d", errno);
        set_error(BACHATA_VORTEK_ERR_BIND);
        set_state(BACHATA_VORTEK_STATE_FAILED);
        destroy_vulkan_locked();
        return BACHATA_VORTEK_ERR_BIND;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        BACHATA_VORTEK_ERR("stage=socket error=bind_failed errno=%d", errno);
        close(fd);
        set_error(BACHATA_VORTEK_ERR_BIND);
        set_state(BACHATA_VORTEK_STATE_FAILED);
        destroy_vulkan_locked();
        return BACHATA_VORTEK_ERR_BIND;
    }
    // Restrictive permissions (owner only).
    chmod(socket_path, 0600);

    if (listen(fd, 1) != 0) {
        BACHATA_VORTEK_ERR("stage=socket error=listen_failed errno=%d", errno);
        close(fd);
        unlink_socket_if_owned();
        set_error(BACHATA_VORTEK_ERR_LISTEN);
        set_state(BACHATA_VORTEK_STATE_FAILED);
        destroy_vulkan_locked();
        return BACHATA_VORTEK_ERR_LISTEN;
    }

    g_server.listen_fd = fd;
    g_server.stop_requested.store(false);
    if (pthread_create(&g_server.listen_thread, nullptr, listen_thread_main, nullptr) != 0) {
        BACHATA_VORTEK_ERR("stage=listen_thread error=create_failed");
        close_fd(g_server.listen_fd);
        unlink_socket_if_owned();
        destroy_vulkan_locked();
        set_error(BACHATA_VORTEK_ERR_INTERNAL);
        set_state(BACHATA_VORTEK_STATE_FAILED);
        return BACHATA_VORTEK_ERR_INTERNAL;
    }
    g_server.listen_thread_started = true;
    set_state(BACHATA_VORTEK_STATE_SOCKET_READY);
    BACHATA_VORTEK_LOG("socket=ready");
    return BACHATA_VORTEK_OK;
}

extern "C" int bachata_vortek_server_stop(void) {
    std::lock_guard<std::mutex> lock(g_server.mu);
    BACHATA_VORTEK_LOG("state=stopping");
    full_cleanup_locked();
    return BACHATA_VORTEK_OK;
}

extern "C" int bachata_vortek_server_state(void) {
    return g_server.state.load();
}

extern "C" int bachata_vortek_server_last_error(void) {
    return g_server.last_error.load();
}

extern "C" int bachata_vortek_server_wait_socket_ready(int timeout_ms) {
    const int64_t start = static_cast<int64_t>(time(nullptr)) * 1000;
    for (;;) {
        const int st = g_server.state.load();
        if (st == BACHATA_VORTEK_STATE_SOCKET_READY ||
            st == BACHATA_VORTEK_STATE_CLIENT_CONNECTED ||
            st == BACHATA_VORTEK_STATE_CONTEXT_READY) {
            return BACHATA_VORTEK_OK;
        }
        if (st == BACHATA_VORTEK_STATE_FAILED || st == BACHATA_VORTEK_STATE_STOPPED) {
            return g_server.last_error.load() ? g_server.last_error.load() : BACHATA_VORTEK_ERR_INTERNAL;
        }
        if (timeout_ms > 0) {
            const int64_t now = static_cast<int64_t>(time(nullptr)) * 1000;
            if (now - start >= timeout_ms) return BACHATA_VORTEK_ERR_INTERNAL;
        }
        usleep(10 * 1000);
    }
}

extern "C" int bachata_vortek_server_wait_context_ready(int timeout_ms) {
    const int64_t start = static_cast<int64_t>(time(nullptr)) * 1000;
    for (;;) {
        const int st = g_server.state.load();
        if (st == BACHATA_VORTEK_STATE_CONTEXT_READY) return BACHATA_VORTEK_OK;
        if (st == BACHATA_VORTEK_STATE_FAILED || st == BACHATA_VORTEK_STATE_STOPPED) {
            return g_server.last_error.load() ? g_server.last_error.load() : BACHATA_VORTEK_ERR_INTERNAL;
        }
        if (timeout_ms > 0) {
            const int64_t now = static_cast<int64_t>(time(nullptr)) * 1000;
            if (now - start >= timeout_ms) return BACHATA_VORTEK_ERR_INTERNAL;
        }
        usleep(10 * 1000);
    }
}

extern "C" const char* bachata_vortek_server_host_api_version(void) {
    return g_server.host_api_version.c_str();
}

// ---- Protocol self-test client (identical wire format to Task 3 client) ----

extern "C" int bachata_vortek_protocol_self_test(const char* socket_path, const char* client_build) {
    if (!socket_path || !*socket_path) return BACHATA_VORTEK_ERR_SOCKET_PATH;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return BACHATA_VORTEK_ERR_INTERNAL;

    timeval tv{};
    tv.tv_sec = 3;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close(fd);
        return BACHATA_VORTEK_ERR_CLIENT_DISCONNECTED;
    }

    BachataVortekHandshakeRequest req{};
    req.magic = BACHATA_VORTEK_MAGIC;
    req.proto_major = BACHATA_VORTEK_PROTO_MAJOR;
    req.proto_minor = BACHATA_VORTEK_PROTO_MINOR;
    req.pointer_size = (uint16_t)sizeof(void*);
    req.endianness = BACHATA_VORTEK_ENDIAN_LITTLE;
    req.vulkan_header_version = 0;
    strncpy(req.client_build_id, client_build ? client_build : "self-test", sizeof(req.client_build_id) - 1);

    char header[BACHATA_VORTEK_HEADER_SIZE];
    *reinterpret_cast<int*>(header + 0) = REQUEST_CODE_BACHATA_HANDSHAKE;
    *reinterpret_cast<int*>(header + 4) = (int)sizeof(req);
    if (bachata_vortek_sock_write(fd, header, BACHATA_VORTEK_HEADER_SIZE) != BACHATA_VORTEK_HEADER_SIZE ||
        bachata_vortek_sock_write(fd, reinterpret_cast<char*>(&req), (int)sizeof(req)) != (int)sizeof(req)) {
        close(fd);
        return BACHATA_VORTEK_ERR_CLIENT_DISCONNECTED;
    }

    char rh[BACHATA_VORTEK_HEADER_SIZE];
    if (bachata_vortek_sock_read(fd, rh, BACHATA_VORTEK_HEADER_SIZE) != BACHATA_VORTEK_HEADER_SIZE) {
        close(fd);
        return BACHATA_VORTEK_ERR_PROTOCOL;
    }
    BachataVortekHandshakeResponse resp{};
    if (bachata_vortek_sock_read(fd, reinterpret_cast<char*>(&resp), (int)sizeof(resp)) != (int)sizeof(resp) ||
        resp.magic != BACHATA_VORTEK_MAGIC || resp.status != BACHATA_VORTEK_HANDSHAKE_OK) {
        close(fd);
        return BACHATA_VORTEK_ERR_PROTOCOL;
    }

    *reinterpret_cast<int*>(header + 0) = REQUEST_CODE_CREATE_CONTEXT;
    *reinterpret_cast<int*>(header + 4) = 0;
    if (bachata_vortek_sock_write(fd, header, BACHATA_VORTEK_HEADER_SIZE) != BACHATA_VORTEK_HEADER_SIZE) {
        close(fd);
        return BACHATA_VORTEK_ERR_CLIENT_DISCONNECTED;
    }

    int shmFds[2] = {-1, -1};
    int numFds = 0;
    if (bachata_vortek_recv_fds(fd, shmFds, &numFds) < 0 || numFds != 2) {
        close(fd);
        return BACHATA_VORTEK_ERR_CONTEXT;
    }

    void* serverRing = bachata_vortek_ring_create(shmFds[0], BACHATA_VORTEK_SERVER_RING_BYTES);
    void* clientRing = bachata_vortek_ring_create(shmFds[1], BACHATA_VORTEK_CLIENT_RING_BYTES);
    if (!serverRing || !clientRing) {
        if (serverRing) bachata_vortek_ring_free(serverRing);
        if (clientRing) bachata_vortek_ring_free(clientRing);
        close(shmFds[0]);
        close(shmFds[1]);
        close(fd);
        return BACHATA_VORTEK_ERR_CONTEXT;
    }

    BACHATA_VORTEK_LOG("self_test=rings_mapped server_bytes=%u client_bytes=%u",
                       BACHATA_VORTEK_SERVER_RING_BYTES, BACHATA_VORTEK_CLIENT_RING_BYTES);

    bachata_vortek_ring_set_exit(serverRing);
    bachata_vortek_ring_set_exit(clientRing);
    bachata_vortek_ring_free(serverRing);
    bachata_vortek_ring_free(clientRing);
    close(shmFds[0]);
    close(shmFds[1]);
    close(fd);
    BACHATA_VORTEK_LOG("self_test=ok");
    return BACHATA_VORTEK_OK;
}
