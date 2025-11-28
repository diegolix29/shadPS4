// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cmath>

#include "app_content.h"
#include "common/assert.h"
#include "common/config.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/file_format/psf.h"
#include "core/file_sys/fs.h"
#include "core/libraries/app_content/app_content_error.h"
#include "core/libraries/libs.h"
#include "core/libraries/system/systemservice.h"

namespace Libraries::AppContent {

struct AddContInfo {
    char entitlement_label[ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE];
    OrbisAppContentAddcontDownloadStatus status;
    OrbisAppContentGetEntitlementKey key;
};

static std::array<AddContInfo, ORBIS_APP_CONTENT_INFO_LIST_MAX_SIZE> addcont_info = {{
    {"0000000000000000",
     OrbisAppContentAddcontDownloadStatus::Installed,
     {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00}},
}};

static s32 addcont_count = 0;
static std::string title_id;

int PS4_SYSV_ABI _Z5dummyv() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAddcontDelete() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAddcontEnqueueDownload() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAddcontEnqueueDownloadSp() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAddcontMount(u32 service_label,
                                           const OrbisNpUnifiedEntitlementLabel* entitlement_label,
                                           OrbisAppContentMountPoint* mount_point) {
    LOG_INFO(Lib_AppContent, "called");

    const auto& addon_path = Config::getAddonDirectory() / title_id;
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();

    s32 i = -1;
    for (int idx = 0; idx < addcont_count; ++idx) {
        if (std::strncmp(entitlement_label->data, addcont_info[idx].entitlement_label,
                         ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE) == 0) {
            i = idx;
            break;
        }
    }
    if (i == -1) {
        return ORBIS_APP_CONTENT_ERROR_NOT_FOUND;
    }

    std::snprintf(mount_point->data, ORBIS_APP_CONTENT_MOUNTPOINT_DATA_MAXSIZE, "/addcont%d", i);
    mount_point->data[ORBIS_APP_CONTENT_MOUNTPOINT_DATA_MAXSIZE - 1] = '\0';

    std::string requested_entitlement(
        entitlement_label->data,
        strnlen(entitlement_label->data, ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE));

    for (const auto& entry : std::filesystem::directory_iterator(addon_path)) {
        if (!entry.is_directory())
            continue;

        const auto param_sfo_path = entry.path() / "sce_sys/param.sfo";
        if (!std::filesystem::exists(param_sfo_path))
            continue;

        PSF dlc_params;
        if (!dlc_params.Open(param_sfo_path))
            continue;

        auto cid_opt = dlc_params.GetString("CONTENT_ID");
        if (!cid_opt.has_value())
            continue;

        std::string cid = std::string{cid_opt.value()};
        std::string folder_entitlement;

        auto pos = cid.find_last_of('-');
        if (pos != std::string::npos && pos + 1 < cid.size()) {
            folder_entitlement = cid.substr(pos + 1);
        } else if (cid.size() > ORBIS_APP_CONTENT_ENTITLEMENT_LABEL_OFFSET) {
            folder_entitlement = cid.substr(ORBIS_APP_CONTENT_ENTITLEMENT_LABEL_OFFSET);
        } else {
            LOG_WARNING(Lib_AppContent, "Additional content folder {} malformed CONTENT_ID: {}",
                        entry.path().filename().string(), cid);
            continue;
        }

        bool match = false;

        if (requested_entitlement == folder_entitlement) {
            match = true;
        } else if (requested_entitlement.size() >= folder_entitlement.size()) {
            if (requested_entitlement.compare(requested_entitlement.size() -
                                                  folder_entitlement.size(),
                                              folder_entitlement.size(), folder_entitlement) == 0) {
                match = true;
            }
        } else if (folder_entitlement.size() >= requested_entitlement.size()) {
            if (folder_entitlement.compare(folder_entitlement.size() - requested_entitlement.size(),
                                           requested_entitlement.size(),
                                           requested_entitlement) == 0) {
                match = true;
            }
        }

        if (match) {
            mnt->Mount(entry.path(), mount_point->data);
            return ORBIS_OK;
        }
    }

    LOG_WARNING(Lib_AppContent, "Entitlement {} was registered but no matching folder exists",
                entitlement_label->data);

    return ORBIS_APP_CONTENT_ERROR_NOT_FOUND;
}

int PS4_SYSV_ABI sceAppContentAddcontShrink() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAddcontUnmount() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAppParamGetInt(OrbisAppContentAppParamId paramId, s32* out_value) {
    if (out_value == nullptr)
        return ORBIS_APP_CONTENT_ERROR_PARAMETER;
    auto* param_sfo = Common::Singleton<PSF>::Instance();
    std::optional<s32> value;
    switch (paramId) {
    case ORBIS_APP_CONTENT_APPPARAM_ID_SKU_FLAG:
        value = ORBIS_APP_CONTENT_APPPARAM_SKU_FLAG_FULL;
        break;
    case ORBIS_APP_CONTENT_APPPARAM_ID_USER_DEFINED_PARAM_1:
        value = param_sfo->GetInteger("USER_DEFINED_PARAM_1");
        break;
    case ORBIS_APP_CONTENT_APPPARAM_ID_USER_DEFINED_PARAM_2:
        value = param_sfo->GetInteger("USER_DEFINED_PARAM_2");
        break;
    case ORBIS_APP_CONTENT_APPPARAM_ID_USER_DEFINED_PARAM_3:
        value = param_sfo->GetInteger("USER_DEFINED_PARAM_3");
        break;
    case ORBIS_APP_CONTENT_APPPARAM_ID_USER_DEFINED_PARAM_4:
        value = param_sfo->GetInteger("USER_DEFINED_PARAM_4");
        break;
    default:
        LOG_ERROR(Lib_AppContent, " paramId = {} paramId is not valid", paramId);
        return ORBIS_APP_CONTENT_ERROR_PARAMETER;
    }
    *out_value = value.value_or(0);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAppParamGetString() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentDownload0Expand() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentDownload0Shrink() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentDownload1Expand() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentDownload1Shrink() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentDownloadDataFormat() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentDownloadDataGetAvailableSpaceKb(OrbisAppContentMountPoint* mountPoint,
                                                              u64* availableSpaceKb) {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    *availableSpaceKb = 1048576;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentGetAddcontDownloadProgress() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentGetAddcontInfo(u32 service_label,
                                             const OrbisNpUnifiedEntitlementLabel* entitlementLabel,
                                             OrbisAppContentAddcontInfo* info) {
    LOG_INFO(Lib_AppContent, "called");

    if (entitlementLabel == nullptr || info == nullptr) {
        return ORBIS_APP_CONTENT_ERROR_PARAMETER;
    }

    for (auto i = 0; i < addcont_count; i++) {
        if (strncmp(entitlementLabel->data, addcont_info[i].entitlement_label,
                    ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE - 1) != 0) {
            continue;
        }

        LOG_INFO(Lib_AppContent, "found DLC {}", entitlementLabel->data);

        std::memset(info->entitlement_label.data, 0, ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE);
        std::strncpy(info->entitlement_label.data, addcont_info[i].entitlement_label,
                     ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE - 1);
        info->status = addcont_info[i].status;

        return ORBIS_OK;
    }

    return ORBIS_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
}

int PS4_SYSV_ABI sceAppContentGetAddcontInfoList(u32 service_label,
                                                 OrbisAppContentAddcontInfo* list, u32 list_num,
                                                 u32* hit_num) {
    LOG_INFO(Lib_AppContent, "called");

    if (list_num == 0 || list == nullptr) {
        if (hit_num == nullptr) {
            return ORBIS_APP_CONTENT_ERROR_PARAMETER;
        }

        *hit_num = addcont_count;
        return ORBIS_OK;
    }

    int dlcs_to_list = addcont_count < list_num ? addcont_count : list_num;
    for (int i = 0; i < dlcs_to_list; i++) {
        strncpy(list[i].entitlement_label.data, addcont_info[i].entitlement_label,
                ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE);
        list[i].status = addcont_info[i].status;
    }

    if (hit_num != nullptr) {
        *hit_num = dlcs_to_list;
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentGetEntitlementKey(
    u32 service_label, const OrbisNpUnifiedEntitlementLabel* entitlement_label,
    OrbisAppContentGetEntitlementKey* key) {
    LOG_INFO(Lib_AppContent, "called");

    if (entitlement_label == nullptr || key == nullptr) {
        return ORBIS_APP_CONTENT_ERROR_PARAMETER;
    }

    for (int i = 0; i < addcont_count; i++) {
        if (strncmp(entitlement_label->data, addcont_info[i].entitlement_label,
                    ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE - 1) != 0) {
            continue;
        }

        memcpy(key->data, addcont_info[i].key.data, ORBIS_APP_CONTENT_ENTITLEMENT_KEY_SIZE);
        return ORBIS_OK;
    }

    return ORBIS_APP_CONTENT_ERROR_DRM_NO_ENTITLEMENT;
}

int PS4_SYSV_ABI sceAppContentGetRegion() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentInitialize(const OrbisAppContentInitParam* initParam,
                                         OrbisAppContentBootParam* bootParam) {
    LOG_ERROR(Lib_AppContent, "(DUMMY) called");
    auto* param_sfo = Common::Singleton<PSF>::Instance();

    const auto addons_dir = Config::getAddonDirectory();
    if (const auto value = param_sfo->GetString("TITLE_ID"); value.has_value()) {
        title_id = *value;
    } else {
        UNREACHABLE_MSG("Failed to get TITLE_ID");
    }
    const auto addon_path = addons_dir / title_id;
    if (!std::filesystem::exists(addon_path)) {
        return ORBIS_OK;
    }

    for (const auto& entry : std::filesystem::directory_iterator(addon_path)) {
        if (entry.is_directory()) {
            // Look for a param.sfo in the additional content directory.
            const auto& param_sfo_path = entry.path() / "sce_sys/param.sfo";
            if (!std::filesystem::exists(param_sfo_path)) {
                LOG_WARNING(Lib_AppContent, "Additonal content folder {} has no param.sfo",
                            entry.path().filename().string());
                continue;
            }

            // Open the param.sfo, make sure it's actually for additional content.
            std::unique_ptr<PSF> dlc_params = std::make_unique<PSF>();
            dlc_params->Open(param_sfo_path);

            auto category = dlc_params->GetString("CATEGORY");
            if (category.has_value() && strncmp(category.value().data(), "ac", 2) == 0) {
                // We've located additional content. Find the entitlement id from the content id.
                auto content_id = dlc_params->GetString("CONTENT_ID");
                if (!content_id.has_value()) {
                    LOG_WARNING(Lib_AppContent,
                                "Additonal content {} param.sfo is missing CONTENT_ID",
                                entry.path().filename().string());
                    continue;
                }

                std::string entitlement_id_str;
                {
                    std::string cid = std::string{content_id.value()};
                    auto pos = cid.find_last_of('-');

                    if (pos != std::string::npos && pos + 1 < cid.size()) {
                        entitlement_id_str = cid.substr(pos + 1);
                    } else if (cid.length() > ORBIS_APP_CONTENT_ENTITLEMENT_LABEL_OFFSET) {
                        entitlement_id_str = cid.substr(ORBIS_APP_CONTENT_ENTITLEMENT_LABEL_OFFSET);
                    } else {
                        LOG_WARNING(Lib_AppContent,
                                    "Additional content {} param.sfo has malformed CONTENT_ID: {}",
                                    entry.path().filename().string(), cid);
                        continue;
                    }
                }

                LOG_INFO(Lib_AppContent, "Entitlement {} found", entitlement_id_str);

                // Save the additional content info in addcont_info.
                if (addcont_count >= static_cast<int>(addcont_info.size())) {
                    LOG_WARNING(Lib_AppContent, "Too many addcont entries, skipping {}",
                                entry.path().filename().string());
                } else {
                    auto& info = addcont_info[addcont_count];
                    const size_t maxlen = ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE - 1;
                    const size_t copylen = std::min(entitlement_id_str.size(), maxlen);

                    std::memset(info.entitlement_label, 0, ORBIS_NP_UNIFIED_ENTITLEMENT_LABEL_SIZE);
                    std::memcpy(info.entitlement_label, entitlement_id_str.data(), copylen);
                    info.entitlement_label[copylen] = '\0';

                    info.status = OrbisAppContentAddcontDownloadStatus::Installed;
                    ++addcont_count;
                }

            } else {
                LOG_WARNING(Lib_AppContent, "Additonal content folder {} is not additional content",
                            entry.path().filename().string());
                continue;
            }
        }
    }

    if (addcont_count > 0) {
        SystemService::OrbisSystemServiceEvent event{};
        event.event_type = SystemService::OrbisSystemServiceEventType::EntitlementUpdate;
        event.service_entitlement_update.user_id = 0;
        event.service_entitlement_update.np_service_label = 0;
        SystemService::PushSystemServiceEvent(event);
    }

    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentRequestPatchInstall() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentSmallSharedDataFormat() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentSmallSharedDataGetAvailableSpaceKb() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentSmallSharedDataMount() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentSmallSharedDataUnmount() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentTemporaryDataFormat() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentTemporaryDataGetAvailableSpaceKb(
    const OrbisAppContentMountPoint* mountPoint, u64* availableSpaceKb) {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    *availableSpaceKb = 1048576;
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentTemporaryDataMount() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentTemporaryDataMount2(OrbisAppContentTemporaryDataOption option,
                                                  OrbisAppContentMountPoint* mountPoint) {
    if (mountPoint == nullptr) {
        return ORBIS_APP_CONTENT_ERROR_PARAMETER;
    }
    static constexpr std::string_view TmpMount = "/temp0";
    TmpMount.copy(mountPoint->data, TmpMount.size());
    LOG_INFO(Lib_AppContent, "sceAppContentTemporaryDataMount2: option = {}, mountPoint = {}",
             option, mountPoint->data);
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentTemporaryDataUnmount() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentGetPftFlag() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI Func_C59A36FF8D7C59DA() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAddcontEnqueueDownloadByEntitlementId() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentAddcontMountByEntitlementId() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentGetAddcontInfoByEntitlementId() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentGetAddcontInfoListByIroTag() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

int PS4_SYSV_ABI sceAppContentGetDownloadedStoreCountry() {
    LOG_ERROR(Lib_AppContent, "(STUBBED) called");
    return ORBIS_OK;
}

void RegisterLib(Core::Loader::SymbolsResolver* sym) {
    LIB_FUNCTION("AS45QoYHjc4", "libSceAppContent", 1, "libSceAppContentUtil", _Z5dummyv);
    LIB_FUNCTION("ZiATpP9gEkA", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAddcontDelete);
    LIB_FUNCTION("7gxh+5QubhY", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAddcontEnqueueDownload);
    LIB_FUNCTION("TVM-aYIsG9k", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAddcontEnqueueDownloadSp);
    LIB_FUNCTION("VANhIWcqYak", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAddcontMount);
    LIB_FUNCTION("D3H+cjfzzFY", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAddcontShrink);
    LIB_FUNCTION("3rHWaV-1KC4", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAddcontUnmount);
    LIB_FUNCTION("99b82IKXpH4", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAppParamGetInt);
    LIB_FUNCTION("+OlXCu8qxUk", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentAppParamGetString);
    LIB_FUNCTION("gpGZDB4ZlrI", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentDownload0Expand);
    LIB_FUNCTION("S5eMvWnbbXg", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentDownload0Shrink);
    LIB_FUNCTION("B5gVeVurdUA", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentDownload1Expand);
    LIB_FUNCTION("kUeYucqnb7o", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentDownload1Shrink);
    LIB_FUNCTION("CN7EbEV7MFU", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentDownloadDataFormat);
    LIB_FUNCTION("Gl6w5i0JokY", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentDownloadDataGetAvailableSpaceKb);
    LIB_FUNCTION("5bvvbUSiFs4", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentGetAddcontDownloadProgress);
    LIB_FUNCTION("m47juOmH0VE", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentGetAddcontInfo);
    LIB_FUNCTION("xnd8BJzAxmk", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentGetAddcontInfoList);
    LIB_FUNCTION("XTWR0UXvcgs", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentGetEntitlementKey);
    LIB_FUNCTION("74-1x3lyZK8", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentGetRegion);
    LIB_FUNCTION("R9lA82OraNs", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentInitialize);
    LIB_FUNCTION("bVtF7v2uqT0", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentRequestPatchInstall);
    LIB_FUNCTION("9Gq5rOkWzNU", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentSmallSharedDataFormat);
    LIB_FUNCTION("xhb-r8etmAA", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentSmallSharedDataGetAvailableSpaceKb);
    LIB_FUNCTION("QuApZnMo9MM", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentSmallSharedDataMount);
    LIB_FUNCTION("EqMtBHWu-5M", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentSmallSharedDataUnmount);
    LIB_FUNCTION("a5N7lAG0y2Q", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentTemporaryDataFormat);
    LIB_FUNCTION("SaKib2Ug0yI", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentTemporaryDataGetAvailableSpaceKb);
    LIB_FUNCTION("7bOLX66Iz-U", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentTemporaryDataMount);
    LIB_FUNCTION("buYbeLOGWmA", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentTemporaryDataMount2);
    LIB_FUNCTION("bcolXMmp6qQ", "libSceAppContent", 1, "libSceAppContentUtil",
                 sceAppContentTemporaryDataUnmount);
    LIB_FUNCTION("xmhnAoxN3Wk", "libSceAppContentPft", 1, "libSceAppContent",
                 sceAppContentGetPftFlag);
    LIB_FUNCTION("xZo2-418Wdo", "libSceAppContentBundle", 1, "libSceAppContent",
                 Func_C59A36FF8D7C59DA);
    LIB_FUNCTION("kJmjt81mXKQ", "libSceAppContentIro", 1, "libSceAppContent",
                 sceAppContentAddcontEnqueueDownloadByEntitlementId);
    LIB_FUNCTION("efX3lrPwdKA", "libSceAppContentIro", 1, "libSceAppContent",
                 sceAppContentAddcontMountByEntitlementId);
    LIB_FUNCTION("z9hgjLd1SGA", "libSceAppContentIro", 1, "libSceAppContent",
                 sceAppContentGetAddcontInfoByEntitlementId);
    LIB_FUNCTION("3wUaDTGmjcQ", "libSceAppContentIro", 1, "libSceAppContent",
                 sceAppContentGetAddcontInfoListByIroTag);
    LIB_FUNCTION("TCqT7kPuGx0", "libSceAppContentSc", 1, "libSceAppContent",
                 sceAppContentGetDownloadedStoreCountry);
};

} // namespace Libraries::AppContent
