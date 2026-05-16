// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <future>
#include <map>
#include <mutex>
#include <string>

#include <httplib.h>

#include "common/types.h"
#include "core/libraries/network/ssl.h"

namespace Core::Loader {
class SymbolsResolver;
}

namespace Libraries::Http {

enum OrbisUriBuild : s32 {
    ORBIS_HTTP_URI_BUILD_WITH_SCHEME = 0x01,
    ORBIS_HTTP_URI_BUILD_WITH_HOSTNAME = 0x02,
    ORBIS_HTTP_URI_BUILD_WITH_PORT = 0x04,
    ORBIS_HTTP_URI_BUILD_WITH_PATH = 0x08,
    ORBIS_HTTP_URI_BUILD_WITH_USERNAME = 0x10,
    ORBIS_HTTP_URI_BUILD_WITH_PASSWORD = 0x20,
    ORBIS_HTTP_URI_BUILD_WITH_QUERY = 0x40,
    ORBIS_HTTP_URI_BUILD_WITH_FRAGMENT = 0x80
};

struct OrbisHttpUriElement {
    bool opaque;
    char* scheme;
    char* username;
    char* password;
    char* hostname;
    char* path;
    char* query;
    char* fragment;
    u16 port;
    u8 reserved[10];
};

enum OrbisHttpRequestMethod : s32 {
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_GET = 0,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_POST = 1,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_HEAD = 2,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_OPTIONS = 3,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_PUT = 4,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_DELETE = 5,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_TRACE = 6,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_CONNECT = 7,
    ORBIS_INTERNAL_HTTP_REQUEST_METHOD_INVALID = 8,
};

class RequestTemplate {
public:
    int id;
    std::map<std::string, std::string> headers;
    std::string user_agent = {};
    bool is_async = false;

    void AddHeader(const char* name, const char* value) {

        headers[std::string(name)] = std::string(value);
    }

    RequestTemplate() : id(0) {}
    explicit RequestTemplate(int tmpl_id, std::string user_agent = "")
        : id(tmpl_id), user_agent(user_agent) {}
};

class RequestObj {
public:
    int id;
    RequestTemplate* req_template = nullptr;

    void SendRequest() {
        request_future = std::async(std::launch::async, [this] { _SendRequest(); });

        if (!req_template->is_async) {
            WaitForRequest();
        }
    }

    bool IsRequestComplete() {
        if (!request_future.valid())
            return true;
        return request_future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
    }

    void WaitForRequest() {
        if (request_future.valid()) {
            request_future.get();
        }
    }

    u32 ReadData(char* dest, u32 size) {

        if (result_body == nullptr || dest == nullptr || size == 0 || result_body_size == 0) {

            return 0;
        }

        u64 start_index = static_cast<u64>(current_result_read_chunk_index) * size;

        if (start_index >= result_body_size) {

            return 0;
        }

        u64 remaining_bytes = result_body_size - start_index;

        u64 bytes_to_copy = (remaining_bytes < size) ? remaining_bytes : size;

        std::memcpy(dest, result_body + start_index, bytes_to_copy);

        current_result_read_chunk_index++;

        return static_cast<u32>(bytes_to_copy);
    }

    std::string GetUrl() const {
        return url;
    }

    s32 GetMethod() const {
        return static_cast<s32>(method);
    }

    void SetUrl(const std::string& new_url) {

        if (new_url.empty()) {
            return;
        }

        // TODO checks
        url = new_url;

        u64 scheme_end = url.find("://");
        u64 path_start = url.find('/', scheme_end + 3);

        if (path_start == std::string::npos) {
            host = url;
            path = "/";
        } else {
            host = url.substr(0, path_start);
            path = url.substr(path_start);
        }
    }

    void SetPostData(const void* data, u64 size) {

        if (data == nullptr || size == 0) {

            post_data = nullptr;
            post_data_size = 0;
            return;
        }

        post_data_size = size;

        post_data = new u8[post_data_size];
        std::memcpy(post_data, data, post_data_size);
    }

    void AddHeader(const char* name, const char* value) {

        req_headers[std::string(name)] = std::string(value);
    }

    bool IsSuccessful() const {
        return status_code / 100 == 2;
    }

    u32 GetStatusCode() const {
        return status_code;
    }

    u64 GetContentLength() const {
        return static_cast<u64>(result_body_size);
    }

    bool IsSent() const {
        return is_sent;
    }

    bool IsCompleted() {
        return status_code != -1;
    }

    void DebugPrint() const {
        LOG_DEBUG(Lib_Http, "===== HTTP Request Debug Info =====");

        LOG_DEBUG(Lib_Http, "Is Sent:        {}", (is_sent ? "true" : "false"));
        LOG_DEBUG(Lib_Http, "Future Valid:   {}", (request_future.valid() ? "true" : "false"));
        LOG_DEBUG(Lib_Http, "Status Code:    {}", status_code);
        LOG_DEBUG(Lib_Http, "Method (Enum):  {}", static_cast<int>(method));

        LOG_DEBUG(Lib_Http, "URL:            {}", (url.empty() ? "[Empty]" : url));
        LOG_DEBUG(Lib_Http, "Host:           {}", (host.empty() ? "[Empty]" : host));
        LOG_DEBUG(Lib_Http, "Path:           {}", (path.empty() ? "[Empty]" : path));

        LOG_DEBUG(Lib_Http, "Post Data Size: {} bytes", post_data_size);
        LOG_DEBUG(Lib_Http, "Post Data Ptr:  {}", post_data);

        if (post_data && post_data_size > 0) {
            std::string preview_str;
            const char* preview = static_cast<const char*>(post_data);
            for (u64 i = 0; i < std::min<u64>(post_data_size, 20); ++i) {
                char c = preview[i];
                preview_str += (std::isprint(static_cast<unsigned char>(c)) ? c : '.');
            }
            LOG_DEBUG(Lib_Http, "  -> Preview: {}...", preview_str);
        }

        LOG_DEBUG(Lib_Http, "Content Length: {}", content_length);
        LOG_DEBUG(Lib_Http, "Body Size:      {} bytes", result_body_size);
        LOG_DEBUG(Lib_Http, "Read Chunk Idx: {}", current_result_read_chunk_index);
        LOG_DEBUG(Lib_Http, "Body Pointer:   {}", (void*)result_body);

        if (result_body) {
            std::string body_preview;
            for (u64 i = 0; i < std::min<u64>(result_body_size, 50); ++i) {
                char c = result_body[i];
                body_preview += (std::isprint(static_cast<unsigned char>(c)) ? c : '.');
            }
            LOG_DEBUG(Lib_Http, "  -> Body Preview: {}...", body_preview);
        }

        LOG_DEBUG(Lib_Http, "Request Headers ({})", req_headers.size());
        if (req_headers.empty()) {
            LOG_DEBUG(Lib_Http, "  [None]");
        } else {
            for (const auto& pair : req_headers) {
                LOG_DEBUG(Lib_Http, "  [{}] : {}", pair.first, pair.second);
            }
        }

        LOG_DEBUG(Lib_Http, "===================================");
    }

    RequestObj()
        : id(0), req_template(nullptr), method(ORBIS_INTERNAL_HTTP_REQUEST_METHOD_INVALID), url(""),
          content_length(0), status_code(-1), result_body(nullptr), result_body_size(-1),
          current_result_read_chunk_index(0), post_data(nullptr), is_sent(false) {}
    explicit RequestObj(s32 req_id, RequestTemplate* req_template, s32 method, std::string url_str,
                        u64 cntLen)
        : id(req_id), req_template(req_template),
          method(static_cast<OrbisHttpRequestMethod>(method)), content_length(cntLen),
          status_code(-1), result_body(nullptr), result_body_size(-1),
          current_result_read_chunk_index(0), post_data(nullptr), is_sent(false) {

        SetUrl(url_str);
    }

private:
    std::future<void> request_future = {};

    char* result_body = nullptr;
    u32 current_result_read_chunk_index = 0;
    u32 result_body_size = 0;

    OrbisHttpRequestMethod method = ORBIS_INTERNAL_HTTP_REQUEST_METHOD_INVALID;
    std::string host = {};
    std::string path = {};
    u64 content_length = 0;

    u32 status_code = -1; // check
    bool is_sent = false;

    std::map<std::string, std::string> req_headers;
    std::string url = {};
    void* post_data = nullptr;
    u64 post_data_size = 0;

    void _SendRequest() {

        httplib::Client cli(host);

        httplib::Result response = {};

        auto templ_headers = req_template->headers;

        httplib::Headers headers;
        for (const auto& pair : templ_headers) {
            headers.emplace(pair.first, pair.second);
        }

        for (const auto& pair : req_headers) {
            headers.emplace(pair.first, pair.second);
        }

        std::string content_type = "application/json";

        auto it = headers.find("Content-Type");
        if (it != headers.end()) {
            content_type = it->second;
        }

        is_sent = true;

        switch (method) {
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_GET:

            response = cli.Get(path, headers);
            break;
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_POST:

            response = cli.Post(path, headers, static_cast<char*>(post_data),
                                static_cast<u64>(post_data_size), content_type);
            break;
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_HEAD:
            response = cli.Head(path, headers);
            break;
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_OPTIONS:
            response = cli.Options(path, headers);
            break;
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_PUT:
            response = cli.Put(path, headers, static_cast<char*>(post_data),
                               static_cast<u64>(post_data_size), content_type);
            break;
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_DELETE:
            response = cli.Delete(path, headers);
            break;
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_TRACE:
            LOG_ERROR(Lib_Http, "TRACE HTTP method not implemented");
            return;
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_CONNECT:
            LOG_ERROR(Lib_Http, "CONNECT HTTP method not implemented");
            return;

        default:
        case ORBIS_INTERNAL_HTTP_REQUEST_METHOD_INVALID:
            LOG_ERROR(Lib_Http, "Invalid HTTP method");
            return;
        }

        if (!response) {

            return;
        }

        status_code = response->status;

        if (response && response->status / 100 == 2) {

            result_body_size = static_cast<u32>(response->body.size());
            result_body = new char[result_body_size];
            std::memcpy(result_body, response->body.data(), result_body_size);
        }
    }
};

struct HttpRequestInternal {
    int state;          // +0x20
    int errorCode;      // +0x28
    int httpStatusCode; // +0x20C
    std::mutex m_mutex;
};

// Memory pool statistics
struct OrbisHttpMemoryPoolStats {
    u64 poolSize;
    u64 maxInuseSize;
    u64 currentInuseSize;
    s32 reserved;
};

// Cookie statistics
struct OrbisHttpCookieStats {
    u64 currentInuseSize;
    u32 currentInuseNum;
    u64 maxInuseSize;
    u32 maxInuseNum;
    u32 removedNum;
    s32 reserved;
};

using OrbisHttpEpollHandle = void*;

// Non-blocking event reported by sceHttpWaitRequest.
struct OrbisHttpNBEvent {
    u32 events;
    u32 eventDetail;
    int id;
    void* userArg;
};

// Callbacks
using OrbisHttpAuthInfoCallback = int(PS4_SYSV_ABI*)(int request, int authType, const char* realm,
                                                     char* username, char* password,
                                                     int isNeedEntity, u8** entityBody,
                                                     u64* entitySize, int* isSave, void* userArg);

using OrbisHttpRedirectCallback = int(PS4_SYSV_ABI*)(int request, s32 statusCode, s32* method,
                                                     const char* location, void* userArg);

using OrbisHttpRequestStatusCallback = void(PS4_SYSV_ABI*)(int request, int requestStatus,
                                                           void* userArg);

using OrbisHttpCookieRecvCallback = int(PS4_SYSV_ABI*)(int request, const char* url,
                                                       const char* cookieHeader, u64 headerLen,
                                                       void* userArg);

using OrbisHttpCookieSendCallback = int(PS4_SYSV_ABI*)(int request, const char* url,
                                                       const char* cookieHeader, void* userArg);

using OrbisHttpsCallback = int(PS4_SYSV_ABI*)(int libsslCtxId, u32 verifyErr, void* const sslCert[],
                                              int certNum, void* userArg);

using OrbisHttpsCaList = Libraries::Ssl::OrbisSslCaList;

// Functions
int PS4_SYSV_ABI sceHttpAbortRequest(int reqId);
int PS4_SYSV_ABI sceHttpAbortRequestForce(int reqId);
int PS4_SYSV_ABI sceHttpAbortWaitRequest(OrbisHttpEpollHandle eh);
int PS4_SYSV_ABI sceHttpAddCookie(int libhttpCtxId, const char* url, const char* cookie,
                                  u64 cookieLength);
int PS4_SYSV_ABI sceHttpAddQuery();
int PS4_SYSV_ABI sceHttpAddRequestHeader(int id, const char* name, const char* value, s32 mode);
int PS4_SYSV_ABI sceHttpAddRequestHeaderRaw();
int PS4_SYSV_ABI sceHttpAuthCacheExport();
int PS4_SYSV_ABI sceHttpAuthCacheFlush(int libhttpCtxId);
int PS4_SYSV_ABI sceHttpAuthCacheImport();
int PS4_SYSV_ABI sceHttpCacheRedirectedConnectionEnabled(int id, int isEnable);
int PS4_SYSV_ABI sceHttpCookieExport(int libhttpCtxId, void* buffer, u64 bufferSize,
                                     u64* exportSize);
int PS4_SYSV_ABI sceHttpCookieFlush(int libhttpCtxId);
int PS4_SYSV_ABI sceHttpCookieImport(int libhttpCtxId, const void* buffer, u64 bufferSize);
int PS4_SYSV_ABI sceHttpCreateConnection(int tmplId, const char* serverName, const char* scheme,
                                         u16 port, int isEnableKeepalive);
int PS4_SYSV_ABI sceHttpCreateConnectionWithURL(int tmplId, const char* url, bool enableKeepalive);
int PS4_SYSV_ABI sceHttpCreateEpoll();
int PS4_SYSV_ABI sceHttpCreateRequest();
int PS4_SYSV_ABI sceHttpCreateRequest2();
int PS4_SYSV_ABI sceHttpCreateRequestWithURL(s32 conn_id, s32 method, const char* url,
                                             u64 content_length);
int PS4_SYSV_ABI sceHttpCreateRequestWithURL2();
int PS4_SYSV_ABI sceHttpCreateTemplate(s32 conn_id, const char* user_agent, s32 http_v, s32 flags);
int PS4_SYSV_ABI sceHttpDbgEnableProfile();
int PS4_SYSV_ABI sceHttpDbgGetConnectionStat();
int PS4_SYSV_ABI sceHttpDbgGetRequestStat();
int PS4_SYSV_ABI sceHttpDbgSetPrintf();
int PS4_SYSV_ABI sceHttpDbgShowConnectionStat();
int PS4_SYSV_ABI sceHttpDbgShowMemoryPoolStat();
int PS4_SYSV_ABI sceHttpDbgShowRequestStat();
int PS4_SYSV_ABI sceHttpDbgShowStat();
int PS4_SYSV_ABI sceHttpDeleteConnection();
int PS4_SYSV_ABI sceHttpDeleteRequest(s32 req_id);
int PS4_SYSV_ABI sceHttpDeleteTemplate();
int PS4_SYSV_ABI sceHttpDestroyEpoll();
int PS4_SYSV_ABI sceHttpGetAcceptEncodingGZIPEnabled();
int PS4_SYSV_ABI sceHttpGetAllResponseHeaders(int reqId, char** header, u64* headerSize);
int PS4_SYSV_ABI sceHttpGetAuthEnabled(int id, int* isEnable);
int PS4_SYSV_ABI sceHttpGetAutoRedirect(int id, int* isEnable);
int PS4_SYSV_ABI sceHttpGetConnectionStat();
int PS4_SYSV_ABI sceHttpGetCookie(int libhttpCtxId, const char* url, char* cookie, u64* required,
                                  u64 prepared, int isSecure);
int PS4_SYSV_ABI sceHttpGetCookieEnabled(int id, int* isEnable);
int PS4_SYSV_ABI sceHttpGetCookieStats(int libhttpCtxId, OrbisHttpCookieStats* stats);
int PS4_SYSV_ABI sceHttpGetEpoll(int id, OrbisHttpEpollHandle* eh, void** userArg);
int PS4_SYSV_ABI sceHttpGetEpollId();
int PS4_SYSV_ABI sceHttpGetLastErrno(int reqId, int* errNum);
int PS4_SYSV_ABI sceHttpGetMemoryPoolStats(int libhttpCtxId, OrbisHttpMemoryPoolStats* currentStat);
int PS4_SYSV_ABI sceHttpGetNonblock(int id, int* isEnable);
int PS4_SYSV_ABI sceHttpGetRegisteredCtxIds();
int PS4_SYSV_ABI sceHttpGetResponseContentLength(u32 req_id, u64* out_content_length, u32* _flag);
int PS4_SYSV_ABI sceHttpGetStatusCode(s32 req_id, s32* status_code);
int PS4_SYSV_ABI sceHttpInit(int libnetMemId, int libsslCtxId, u64 poolSize);
int PS4_SYSV_ABI sceHttpParseResponseHeader(const char* header, u64 headerLen, const char* fieldStr,
                                            const char** fieldValue, u64* valueLen);
int PS4_SYSV_ABI sceHttpParseStatusLine(const char* statusLine, u64 lineLen, int32_t* httpMajorVer,
                                        int32_t* httpMinorVer, int32_t* responseCode,
                                        const char** reasonPhrase, u64* phraseLen);
int PS4_SYSV_ABI sceHttpReadData(u32 req_id, char* dest, u32 size);
int PS4_SYSV_ABI sceHttpRedirectCacheFlush();
int PS4_SYSV_ABI sceHttpRemoveRequestHeader();
int PS4_SYSV_ABI sceHttpRequestGetAllHeaders();
int PS4_SYSV_ABI sceHttpsDisableOption();
int PS4_SYSV_ABI sceHttpsDisableOptionPrivate();
int PS4_SYSV_ABI sceHttpsEnableOption(u32 options);
int PS4_SYSV_ABI sceHttpsEnableOptionPrivate();
int PS4_SYSV_ABI sceHttpSendRequest(int reqId, const void* postData, u64 size);
int PS4_SYSV_ABI sceHttpSetAcceptEncodingGZIPEnabled(int id, int isEnable);
int PS4_SYSV_ABI sceHttpSetAuthEnabled(int id, int isEnable);
int PS4_SYSV_ABI sceHttpSetAuthInfoCallback(int id, OrbisHttpAuthInfoCallback cbfunc,
                                            void* userArg);
int PS4_SYSV_ABI sceHttpSetAutoRedirect(int id, int isEnable);
int PS4_SYSV_ABI sceHttpSetChunkedTransferEnabled(int id, int isEnable);
int PS4_SYSV_ABI sceHttpSetConnectTimeOut(int id, u32 usec);
int PS4_SYSV_ABI sceHttpSetCookieEnabled(int id, int isEnable);
int PS4_SYSV_ABI sceHttpSetCookieMaxNum(int libhttpCtxId, u32 num);
int PS4_SYSV_ABI sceHttpSetCookieMaxNumPerDomain(int libhttpCtxId, u32 num);
int PS4_SYSV_ABI sceHttpSetCookieMaxSize(int libhttpCtxId, u32 size);
int PS4_SYSV_ABI sceHttpSetCookieRecvCallback(int id, OrbisHttpCookieRecvCallback cbfunc,
                                              void* userArg);
int PS4_SYSV_ABI sceHttpSetCookieSendCallback(int id, OrbisHttpCookieSendCallback cbfunc,
                                              void* userArg);
int PS4_SYSV_ABI sceHttpSetCookieTotalMaxSize(int libhttpCtxId, u32 size);
int PS4_SYSV_ABI sceHttpSetDefaultAcceptEncodingGZIPEnabled(int libhttpCtxId, int isEnable);
int PS4_SYSV_ABI sceHttpSetDelayBuildRequestEnabled(int id, int isEnable);
int PS4_SYSV_ABI sceHttpSetEpoll(int id, OrbisHttpEpollHandle eh, void* userArg);
int PS4_SYSV_ABI sceHttpSetEpollId();
int PS4_SYSV_ABI sceHttpSetHttp09Enabled();
int PS4_SYSV_ABI sceHttpSetInflateGZIPEnabled();
int PS4_SYSV_ABI sceHttpSetNonblock(s32 tmpl_id, bool enable);
int PS4_SYSV_ABI sceHttpSetPolicyOption();
int PS4_SYSV_ABI sceHttpSetPriorityOption();
int PS4_SYSV_ABI sceHttpSetProxy();
int PS4_SYSV_ABI sceHttpSetRecvBlockSize(int id, u32 blockSize);
int PS4_SYSV_ABI sceHttpSetRecvTimeOut(int id, u32 usec);
int PS4_SYSV_ABI sceHttpSetRedirectCallback(int id, OrbisHttpRedirectCallback cbfunc,
                                            void* userArg);
int PS4_SYSV_ABI sceHttpSetRequestContentLength(int id, u64 contentLength);
int PS4_SYSV_ABI sceHttpSetRequestStatusCallback(int id, OrbisHttpRequestStatusCallback cbfunc,
                                                 void* userArg);
int PS4_SYSV_ABI sceHttpSetResolveRetry(int id, int retry);
int PS4_SYSV_ABI sceHttpSetResolveTimeOut(int id, u32 usec);
int PS4_SYSV_ABI sceHttpSetResponseHeaderMaxSize(int id, u64 headerSize);
int PS4_SYSV_ABI sceHttpSetSendTimeOut(int id, u32 usec);
int PS4_SYSV_ABI sceHttpSetSocketCreationCallback();
int PS4_SYSV_ABI sceHttpsFreeCaList(int libhttpCtxId, OrbisHttpsCaList* caList);
int PS4_SYSV_ABI sceHttpsGetCaList(int httpCtxId, OrbisHttpsCaList* list);
int PS4_SYSV_ABI sceHttpsGetSslError(int id, int* errNum, u32* detail);
int PS4_SYSV_ABI sceHttpsLoadCert(int libhttpCtxId, int caCertNum, const void** caList,
                                  const void* cert, const void* privKey);
int PS4_SYSV_ABI sceHttpsSetMinSslVersion(int id, int version);
int PS4_SYSV_ABI sceHttpsSetSslCallback(int id, OrbisHttpsCallback cbfunc, void* userArg);
int PS4_SYSV_ABI sceHttpsSetSslVersion(int id, int version);
int PS4_SYSV_ABI sceHttpsUnloadCert(int libhttpCtxId);
int PS4_SYSV_ABI sceHttpTerm(int libhttpCtxId);
int PS4_SYSV_ABI sceHttpTryGetNonblock(int id, int* isEnable);
int PS4_SYSV_ABI sceHttpTrySetNonblock(int id, int isEnable);
int PS4_SYSV_ABI sceHttpUnsetEpoll(int id);
int PS4_SYSV_ABI sceHttpWaitRequest(OrbisHttpEpollHandle eh, OrbisHttpNBEvent* nbev, int maxevents,
                                    int timeout);
int PS4_SYSV_ABI sceHttpUriBuild(char* out, u64* require, u64 prepare,
                                 const OrbisHttpUriElement* srcElement, u32 option);
int PS4_SYSV_ABI sceHttpUriCopy();
//***********************************
// URI functions
//***********************************
int PS4_SYSV_ABI sceHttpUriEscape(char* out, u64* require, u64 prepare, const char* in);
int PS4_SYSV_ABI sceHttpUriMerge(char* mergedUrl, char* url, char* relativeUri, u64* require,
                                 u64 prepare, u32 option);
int PS4_SYSV_ABI sceHttpUriParse(OrbisHttpUriElement* out, const char* srcUri, void* pool,
                                 u64* require, u64 prepare);
int PS4_SYSV_ABI sceHttpUriSweepPath(char* dst, const char* src, u64 srcSize);
int PS4_SYSV_ABI sceHttpUriUnescape(char* out, u64* require, u64 prepare, const char* in);

void RegisterLib(Core::Loader::SymbolsResolver* sym);
} // namespace Libraries::Http