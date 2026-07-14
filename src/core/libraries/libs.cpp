// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/elf_info.h"
#include "common/singleton.h"
#include "common/config.h"
#include "core/libraries/ajm/ajm.h"
#include "core/libraries/app_content/app_content.h"
#include "core/libraries/audio/audioin.h"
#include "core/libraries/audio/audioout.h"
#include "core/libraries/audio3d/audio3d.h"
#include "core/libraries/audio3d/audio3d_openal.h"
#include "core/libraries/avplayer/avplayer.h"
#include "core/libraries/camera/camera.h"
#include "core/libraries/companion/companion_httpd.h"
#include "core/libraries/companion/companion_util.h"
#include "core/libraries/disc_map/disc_map.h"
#include "core/libraries/game_live_streaming/gamelivestreaming.h"
#include "core/libraries/gnmdriver/gnmdriver.h"
#include "core/libraries/hmd/hmd.h"
#include "core/libraries/hmd/hmd_setup_dialog.h"
#include "core/libraries/ime/error_dialog.h"
#include "core/libraries/ime/ime.h"
#include "core/libraries/ime/ime_dialog.h"
#include "core/libraries/kernel/kernel.h"
#include "core/libraries/libc_internal/libc_internal.h"
#include "core/libraries/libpng/pngdec.h"
#include "core/libraries/libs.h"
#include "core/libraries/mouse/mouse.h"
#include "core/libraries/move/move.h"
#include "core/libraries/network/http.h"
#include "core/libraries/network/http2.h"
#include "core/libraries/network/net.h"
#include "core/libraries/network/netctl.h"
#include "core/libraries/network/ssl.h"
#include "core/libraries/network/ssl2.h"
#include "core/libraries/np/np_auth.h"
#include "core/libraries/np/np_commerce/np_commerce.h"
#include "core/libraries/np/np_common.h"
#include "core/libraries/np/np_manager.h"
#include "core/libraries/np/np_partner.h"
#include "core/libraries/np/np_party.h"
#include "core/libraries/np/np_profile_dialog/np_profile_dialog.h"
#include "core/libraries/np/np_score.h"
#include "core/libraries/np/np_signaling.h"
#include "core/libraries/np/np_sns_facebook_dialog.h"
#include "core/libraries/np/np_trophy.h"
#include "core/libraries/np/np_web_api.h"
#include "core/libraries/pad/pad.h"
#include "core/libraries/playgo/playgo.h"
#include "core/libraries/playgo/playgo_dialog.h"
#include "core/libraries/random/random.h"
#include "core/libraries/razor_cpu/razor_cpu.h"
#include "core/libraries/remote_play/remoteplay.h"
#include "core/libraries/rtc/rtc.h"
#include "core/libraries/save_data/dialog/savedatadialog.h"
#include "core/libraries/save_data/savedata.h"
#include "core/libraries/screenshot/screenshot.h"
#include "core/libraries/share_play/shareplay.h"
#include "core/libraries/signin_dialog/signindialog.h"
#include "core/libraries/system/commondialog.h"
#include "core/libraries/system/msgdialog.h"
#include "core/libraries/system/systemservice.h"
#include "core/libraries/system/userservice.h"
#include "core/libraries/ulobjmgr/ulobjmgr.h"
#include "core/libraries/usbd/usbd.h"
#include "core/libraries/videodec/videodec.h"
#include "core/libraries/videodec/videodec2.h"
#include "core/libraries/videoout/video_out.h"
#include "core/libraries/voice/voice.h"
#include "core/libraries/vr_tracker/vr_tracker.h"
#include "core/libraries/web_browser_dialog/webbrowserdialog.h"
#include "core/libraries/zlib/zlib_sce.h"
#include "emulator.h"

#include <array>

namespace Libraries {

void InitHLELibs(Core::Loader::SymbolsResolver* sym) {
    LOG_INFO(Lib_Kernel, "Initializing HLE libraries");
    Libraries::Kernel::RegisterLib(sym);
    Libraries::GnmDriver::RegisterLib(sym);
    Libraries::VideoOut::RegisterLib(sym);
    Libraries::UserService::RegisterLib(sym);
    Libraries::SystemService::RegisterLib(sym);
    Libraries::CommonDialog::RegisterLib(sym);
    Libraries::MsgDialog::RegisterLib(sym);
    Libraries::AudioOut::RegisterLib(sym);
    Libraries::Http::RegisterLib(sym);
    Libraries::Http2::RegisterLib(sym);
    Libraries::Net::RegisterLib(sym);
    Libraries::NetCtl::RegisterLib(sym);
    Libraries::SaveData::RegisterLib(sym);
    Libraries::SaveData::Dialog::RegisterLib(sym);
    Libraries::Ssl2::RegisterLib(sym);
    Libraries::SysModule::RegisterLib(sym);
    Libraries::Posix::RegisterLib(sym);
    Libraries::AudioIn::RegisterLib(sym);
    Libraries::Np::NpCommerce::RegisterLib(sym);
    Libraries::Np::NpCommon::RegisterLib(sym);
    Libraries::Np::NpManager::RegisterLib(sym);
    Libraries::Np::NpMatching2::RegisterLib(sym);
    Libraries::Np::NpSignaling::RegisterLib(sym);
    Libraries::Np::NpScore::RegisterLib(sym);
    Libraries::Np::NpTrophy::RegisterLib(sym);
    Libraries::Np::NpWebApi::RegisterLib(sym);
    Libraries::Np::NpWebApi2::RegisterLib(sym);
    Libraries::Np::NpProfileDialog::RegisterLib(sym);
    Libraries::Np::NpSnsFacebookDialog::RegisterLib(sym);
    Libraries::Np::NpAuth::RegisterLib(sym);
    Libraries::Np::NpParty::RegisterLib(sym);
    Libraries::Np::NpPartner::RegisterLib(sym);
    Libraries::Np::NpTus::RegisterLib(sym);
    Libraries::ScreenShot::RegisterLib(sym);
    Libraries::AppContent::RegisterLib(sym);
    Libraries::PngDec::RegisterLib(sym);
    Libraries::PlayGo::RegisterLib(sym);
    Libraries::PlayGo::Dialog::RegisterLib(sym);
    Libraries::Random::RegisterLib(sym);
    Libraries::Usbd::RegisterLib(sym);
    Libraries::Pad::RegisterLib(sym);
    Libraries::Ajm::RegisterLib(sym);
    Libraries::ErrorDialog::RegisterLib(sym);
    Libraries::ImeDialog::RegisterLib(sym);
    Libraries::AvPlayer::RegisterLib(sym);
    Libraries::Videodec::RegisterLib(sym);
    Libraries::Videodec2::RegisterLib(sym);
    if (EmulatorSettings.GetAudioBackend() == AudioBackend::OpenAL) {
        Libraries::Audio3dOpenAL::RegisterLib(sym);
}

void InitHLELibs(Core::Loader::SymbolsResolver* sym) {
    LOG_INFO(Lib_Kernel, "Initializing HLE libraries");

    auto* game_info = Common::Singleton<Common::ElfInfo>::Instance();
    const auto& sys_module_path = EmulatorSettings.GetSysModulesDir();
    const auto& game_specific_modules_path =
        sys_module_path /
        (game_info->GameSerial().empty() ? std::string_view("no_serial") : game_info->GameSerial());
    {
        constexpr auto ModulesToLoad = std::to_array<Core::SysModules>({
            {"libkernel.sprx", Libraries::Kernel::RegisterLib},
            {"libSceGnmDriver.sprx", Libraries::GnmDriver::RegisterLib},
            {"libSceVideoOut.sprx", Libraries::VideoOut::RegisterLib},
            {"libSceUserService.sprx", Libraries::UserService::RegisterLib},
            {"libSceSystemService.sprx", Libraries::SystemService::RegisterLib},
            {"libSceCommonDialog.sprx", Libraries::CommonDialog::RegisterLib},
            {"libSceMsgDialog.sprx", Libraries::MsgDialog::RegisterLib},
            {"libSceAudioOut.sprx", Libraries::AudioOut::RegisterLib},
            {"libSceAudioIn.sprx", Libraries::AudioIn::RegisterLib},
            {"libSceAudio3d.sprx", RegisterAudio3d},
            {"libSceHttp.sprx", Libraries::Http::RegisterLib},
            {"libSceHttp2.sprx", Libraries::Http2::RegisterLib},
            {"libSceNet.sprx", Libraries::Net::RegisterLib},
            {"libSceNetCtl.sprx", Libraries::NetCtl::RegisterLib},
            {"libSceSaveData.sprx", Libraries::SaveData::RegisterLib},
            {"libSceSaveDataDialog.sprx", Libraries::SaveData::Dialog::RegisterLib},
            {"libSceSsl2.sprx", Libraries::Ssl2::RegisterLib},
            {"libSceSysmodule.sprx", Libraries::SysModule::RegisterLib},
            {"libSceNpCommerce.sprx", Libraries::Np::NpCommerce::RegisterLib},
            {"libSceNpCommon.sprx", Libraries::Np::NpCommon::RegisterLib},
            {"libSceNpManager.sprx", Libraries::Np::NpManager::RegisterLib},
            {"libSceNpMatching2.sprx", Libraries::Np::NpMatching2::RegisterLib},
            {"libSceNpSignaling.sprx", Libraries::Np::NpSignaling::RegisterLib},
            {"libSceNpScore.sprx", Libraries::Np::NpScore::RegisterLib},
            {"libSceNpTrophy.sprx", Libraries::Np::NpTrophy::RegisterLib},
            {"libSceNpWebApi.sprx", Libraries::Np::NpWebApi::RegisterLib},
            {"libSceNpWebApi2.sprx", Libraries::Np::NpWebApi2::RegisterLib},
            {"libSceNpProfileDialog.sprx", Libraries::Np::NpProfileDialog::RegisterLib},
            {"libSceNpSnsFacebookDialog.sprx", Libraries::Np::NpSnsFacebookDialog::RegisterLib},
            {"libSceNpAuth.sprx", Libraries::Np::NpAuth::RegisterLib},
            {"libSceNpParty.sprx", Libraries::Np::NpParty::RegisterLib},
            {"libSceNpPartner.sprx", Libraries::Np::NpPartner::RegisterLib},
            {"libSceNpTus.sprx", Libraries::Np::NpTus::RegisterLib},
            {"libSceScreenShot.sprx", Libraries::ScreenShot::RegisterLib},
            {"libSceAppContent.sprx", Libraries::AppContent::RegisterLib},
            {"libScePngDec.sprx", Libraries::PngDec::RegisterLib},
            {"libScePlayGo.sprx", Libraries::PlayGo::RegisterLib},
            {"libScePlayGoDialog.sprx", Libraries::PlayGo::Dialog::RegisterLib},
            {"libSceRandom.sprx", Libraries::Random::RegisterLib},
            {"libSceUsbd.sprx", Libraries::Usbd::RegisterLib},
            {"libScePad.sprx", Libraries::Pad::RegisterLib},
            {"libSceAjm.sprx", Libraries::Ajm::RegisterLib},
            {"libSceErrorDialog.sprx", Libraries::ErrorDialog::RegisterLib},
            {"libSceImeDialog.sprx", Libraries::ImeDialog::RegisterLib},
            {"libSceAvPlayer.sprx", Libraries::AvPlayer::RegisterLib},
            {"libSceVideodec.sprx", Libraries::Videodec::RegisterLib},
            {"libSceVideodec2.sprx", Libraries::Videodec2::RegisterLib},
            {"libSceIme.sprx", Libraries::Ime::RegisterLib},
            {"libSceGameLiveStreaming.sprx", Libraries::GameLiveStreaming::RegisterLib},
            {"libSceSharePlay.sprx", Libraries::SharePlay::RegisterLib},
            {"libSceRemoteplay.sprx", Libraries::Remoteplay::RegisterLib},
            {"libSceRazorCpu.sprx", Libraries::RazorCpu::RegisterLib},
            {"libSceMove.sprx", Libraries::Move::RegisterLib},
            {"libSceMouse.sprx", Libraries::Mouse::RegisterLib},
            {"libSceWebBrowserDialog.sprx", Libraries::WebBrowserDialog::RegisterLib},
            {"libSceZlib.sprx", Libraries::Zlib::RegisterLib},
            {"libSceHmd.sprx", Libraries::Hmd::RegisterLib},
            {"libSceHmdSetupDialog.sprx", Libraries::HmdSetupDialog::RegisterLib},
            {"libSceDiscMap.sprx", Libraries::DiscMap::RegisterLib},
            {"ulobjmgr.sprx", Libraries::Ulobjmgr::RegisterLib},
            {"libSceSigninDialog.sprx", Libraries::SigninDialog::RegisterLib},
            {"libSceCamera.sprx", Libraries::Camera::RegisterLib},
            {"libSceCompanionHttpd.sprx", Libraries::CompanionHttpd::RegisterLib},
            {"libSceCompanionUtil.sprx", Libraries::CompanionUtil::RegisterLib},
            {"libSceVoice.sprx", Libraries::Voice::RegisterLib},
            {"libSceVrTracker.sprx", Libraries::VrTracker::RegisterLib},
            {"libSceContentExport.sprx", Libraries::ContentExport::RegisterLib},
            {"libSceVideoRecording.sprx", Libraries::VideoRecording::RegisterLib},
            {"libSceInvitationDialog.sprx", Libraries::InvitationDialog::RegisterLib},
#ifdef ARCH_X86_64
            {"libSceFiber.sprx", Libraries::Fiber::RegisterLib},
#endif
            // Loading libSceSsl is locked behind a title workaround that currently applies to
            // nothing.
            // {"libSceSsl.sprx", Libraries::Ssl::RegisterLib},
        });
    Libraries::ContentExport::RegisterLib(sym);
    Libraries::VideoRecording::RegisterLib(sym);
    Libraries::InvitationDialog::RegisterLib(sym);

        for (auto mod : ModulesToLoad) {
            if (mod.module_name == "libSceGnmDriver.sprx") {
                auto lle_file = Kernel::sceKernelIsNeoMode() ? "libSceGnmDriverForNeoMode.sprx"
                                                             : mod.module_name;
                if (std::filesystem::exists(game_specific_modules_path / lle_file)) {
                    LOG_WARNING(Loader, "{} LLEd", lle_file);
                } else {
                    mod.callback(sym);
                }
            } else if (std::filesystem::exists(game_specific_modules_path / mod.module_name)) {
                LOG_WARNING(Loader, "{} LLEd", mod.module_name);
            } else {
                mod.callback(sym);
            }
        }
    }
}

} // namespace Libraries