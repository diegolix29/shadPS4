// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <zarchive/zarchivewriter.h>

#include "common/io_file.h"
#include "common/overlay_fs.h"
#include "common/path_util.h"
#include "common/string_util.h"
#include "common/zar_fs.h"
#include "core/file_format/psf.h"
#include "core/file_sys/game_content.h"

namespace Common::FS::Zar {
namespace {

namespace fs = std::filesystem;

struct PackContext {
    fs::path path;
    std::ofstream output;
    bool failed{};
};

void NewOutputFile(s32 part_index, void* opaque) {
    auto* context = static_cast<PackContext*>(opaque);
    if (part_index != -1) {
        context->failed = true;
        return;
    }
    context->output.open(context->path, std::ios::binary | std::ios::trunc);
    context->failed = !context->output;
}

void WriteOutputData(const void* data, size_t length, void* opaque) {
    auto* context = static_cast<PackContext*>(opaque);
    context->output.write(static_cast<const char*>(data), static_cast<std::streamsize>(length));
    context->failed = context->failed || !context->output;
}

bool AddFile(ZArchiveWriter& writer, const char* path, std::string_view contents) {
    if (!writer.StartNewFile(path)) {
        return false;
    }
    writer.AppendData(contents.data(), contents.size());
    return true;
}

bool CreateArchiveWithFiles(
    const fs::path& path,
    std::initializer_list<std::pair<std::string_view, std::string_view>> files) {
    PackContext context{.path = path};
    {
        ZArchiveWriter writer{NewOutputFile, WriteOutputData, &context};
        std::set<std::string> directories;
        for (const auto& [file_path, contents] : files) {
            const auto parent = fs::path{file_path}.parent_path().generic_string();
            const std::string path_string{file_path};
            if ((!parent.empty() && directories.emplace(parent).second &&
                 !writer.MakeDir(parent.c_str(), true)) ||
                !AddFile(writer, path_string.c_str(), contents)) {
                return false;
            }
        }
        if (context.failed) {
            return false;
        }
        writer.Finalize();
    }
    context.output.close();
    return !context.failed;
}

bool CreateArchive(const fs::path& path) {
    return CreateArchiveWithFiles(path, {{"sce_sys/Param.SFO", "parameter data"},
                                         {"eboot.bin", "executable data"},
                                         {"data/part.zar", "nested extension"}});
}

void WriteFile(const fs::path& path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream file{path, std::ios::binary | std::ios::trunc};
    file << contents;
}

std::string MakeParam(std::string category, std::string title_id, std::string app_version,
                      std::string content_id = {}) {
    PSF param;
    param.AddString("CATEGORY", std::move(category));
    param.AddString("TITLE_ID", std::move(title_id));
    param.AddString("APP_VER", std::move(app_version));
    if (!content_id.empty()) {
        param.AddString("CONTENT_ID", std::move(content_id));
    }
    const auto encoded = param.Encode();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

class ZarFsTest : public testing::Test {
protected:
    void SetUp() override {
        original_temp_dir = GetUserPath(PathType::TempDataDir);
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        test_dir = fs::temp_directory_path() / ("shadps4_zar_test_" + std::to_string(unique));
        ASSERT_TRUE(fs::create_directories(test_dir));
        archive_path = test_dir / "CUSA00001.zar";
        ASSERT_TRUE(CreateArchive(archive_path));
    }

    void TearDown() override {
        ClearCache();
        SetUserPath(PathType::TempDataDir, original_temp_dir);
        std::error_code ec;
        fs::remove_all(test_dir, ec);
    }

    fs::path test_dir;
    fs::path archive_path;
    fs::path original_temp_dir;
};

TEST_F(ZarFsTest, QueriesAndIteratesArchive) {
    EXPECT_TRUE(IsZarArchive(archive_path));
    EXPECT_TRUE(Exists(archive_path));
    EXPECT_TRUE(IsDirectory(archive_path));
    EXPECT_FALSE(IsZarInnerPath(archive_path));

    const auto param_path = archive_path / "SCE_SYS" / "param.sfo";
    EXPECT_TRUE(IsZarInnerPath(param_path));
    EXPECT_TRUE(Exists(param_path));
    EXPECT_TRUE(IsRegularFile(param_path));
    EXPECT_EQ(GetFileSize(param_path), 14);
    EXPECT_TRUE(GetLastWriteTime(param_path).has_value());

    std::set<std::string> entries;
    ASSERT_TRUE(IterateDirectory(archive_path, [&](const fs::path& path, bool is_file) {
        entries.emplace(path.filename().string() + (is_file ? ":file" : ":dir"));
    }));
    EXPECT_EQ(entries, (std::set<std::string>{"data:dir", "eboot.bin:file", "sce_sys:dir"}));
}

TEST_F(ZarFsTest, ReadsSeeksAndCopiesFiles) {
    const auto nested_zar_path = archive_path / "data" / "part.zar";
    EXPECT_TRUE(IsZarInnerPath(nested_zar_path));

    IOFile file{nested_zar_path, FileAccessMode::Read};
    ASSERT_TRUE(file.IsOpen());
    EXPECT_TRUE(file.IsZarBacked());
    ASSERT_TRUE(file.Seek(-9, SeekOrigin::End));

    std::string suffix(9, '\0');
    ASSERT_EQ(file.ReadRaw<char>(suffix.data(), suffix.size()), suffix.size());
    EXPECT_EQ(suffix, "extension");

    const auto copied_path = test_dir / "copied.bin";
    ASSERT_TRUE(CopyFile(archive_path / "eboot.bin", copied_path));
    std::ifstream copied{copied_path, std::ios::binary};
    const std::string copied_contents{std::istreambuf_iterator<char>{copied}, {}};
    EXPECT_EQ(copied_contents, "executable data");

    SetUserPath(PathType::TempDataDir, test_dir);
    IOFile materialized{archive_path / "eboot.bin", FileAccessMode::Read};
    ASSERT_TRUE(materialized.MaterializeToHost());
    EXPECT_FALSE(materialized.IsZarBacked());
    EXPECT_TRUE(fs::is_regular_file(materialized.GetPath()));
    EXPECT_EQ(materialized.ReadString(15), "executable data");
}

TEST_F(ZarFsTest, FindsArchivedGameById) {
    const auto found = FindGameByID(test_dir, "CUSA00001", 0);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(*found, archive_path / "eboot.bin");
}

TEST_F(ZarFsTest, ResolvesDirectoryAndArchiveCompanions) {
    const auto update_path = test_dir / "CUSA00001-UPDATE";
    ASSERT_TRUE(fs::create_directory(update_path));
    EXPECT_EQ(ResolveCompanionPath(archive_path, "-UPDATE"), update_path);

    const auto patch_path = test_dir / "CUSA00001-patch.zar";
    ASSERT_TRUE(CreateArchive(patch_path));
    EXPECT_EQ(ResolveCompanionPath(archive_path, "-patch"), patch_path);

    EXPECT_EQ(ResolveCompanionPath(archive_path, "-mods"), test_dir / "CUSA00001-mods");

    const auto legacy_mods_path = test_dir / "CUSA00001.zar-mods";
    ASSERT_TRUE(fs::create_directory(legacy_mods_path));
    EXPECT_EQ(ResolveCompanionPath(archive_path, "-mods"), legacy_mods_path);
}

TEST_F(ZarFsTest, OverlayFallsThroughBetweenLooseAndArchiveSources) {
    const auto base_path = test_dir / "base";
    ASSERT_TRUE(fs::create_directory(base_path));
    WriteFile(base_path / "base_only.bin", "base");

    const auto patch_path = test_dir / "patch.zar";
    ASSERT_TRUE(CreateArchiveWithFiles(
        patch_path, {{"patch_only.bin", "patch"}, {"Folder/ArchiveOnly.bin", "archive"}}));

    const std::array sources{patch_path, base_path};
    const OverlayView overlay{sources};

    EXPECT_EQ(overlay.Resolve("base_only.bin"), base_path / "base_only.bin");
    EXPECT_EQ(overlay.Resolve("PATCH_ONLY.BIN"), patch_path / "PATCH_ONLY.BIN");
    EXPECT_EQ(overlay.Resolve("folder/archiveonly.BIN"), patch_path / "folder/archiveonly.BIN");
    EXPECT_FALSE(overlay.Resolve("missing.bin").has_value());
    EXPECT_FALSE(overlay.Resolve("../base/base_only.bin").has_value());
    EXPECT_FALSE(overlay.Resolve(fs::path{"/base_only.bin"}).has_value());
}

TEST_F(ZarFsTest, OverlayMergesSameNamedLooseAndArchiveSources) {
    const auto loose_path = test_dir / "patch";
    ASSERT_TRUE(fs::create_directory(loose_path));
    WriteFile(loose_path / "shared.bin", "loose");
    WriteFile(loose_path / "loose_only.bin", "loose");
    WriteFile(loose_path / "MiXeD.bin", "mixed");
    ASSERT_TRUE(fs::create_directory(loose_path / "type_conflict"));

    const auto packed_path = test_dir / "patch.zar";
    ASSERT_TRUE(CreateArchiveWithFiles(
        packed_path,
        {{"shared.bin", "archive"}, {"archive_only.bin", "archive"}, {"type_conflict", "file"}}));

    const std::array sources{loose_path, packed_path};
    const OverlayView overlay{sources};

    EXPECT_EQ(overlay.Resolve("shared.bin"), loose_path / "shared.bin");
    EXPECT_EQ(overlay.Resolve("archive_only.bin"), packed_path / "archive_only.bin");
    EXPECT_EQ(overlay.Resolve("mixed.BIN"), loose_path / "MiXeD.bin");
    EXPECT_TRUE(overlay.IsDirectory("type_conflict"));

    std::set<std::string> entries;
    ASSERT_TRUE(overlay.IterateDirectory({}, [&](const fs::path& path, bool is_file) {
        entries.emplace(Common::ToLower(path.filename().string()) + (is_file ? ":file" : ":dir"));
    }));
    EXPECT_EQ(entries,
              (std::set<std::string>{"archive_only.bin:file", "loose_only.bin:file",
                                     "mixed.bin:file", "shared.bin:file", "type_conflict:dir"}));
}

TEST_F(ZarFsTest, CatalogMergesLooseAndArchiveUpdatesOfTheNewestVersion) {
    const auto base_path = test_dir / "CUSA00001";
    WriteFile(base_path / "sce_sys/param.sfo", MakeParam("gd", "CUSA00001", "01.00"));
    WriteFile(base_path / "eboot.bin", "base");
    WriteFile(base_path / "base_only.bin", "base");

    const auto loose_update = test_dir / "CUSA00001-UPDATE";
    WriteFile(loose_update / "sce_sys/param.sfo", MakeParam("gp", "CUSA00001", "01.10"));
    WriteFile(loose_update / "shared.bin", "loose");

    const auto packed_update = test_dir / "CUSA00001-UPDATE.zar";
    const auto update_param = MakeParam("gp", "CUSA00001", "01.10");
    ASSERT_TRUE(CreateArchiveWithFiles(packed_update, {{"sce_sys/param.sfo", update_param},
                                                       {"shared.bin", "archive"},
                                                       {"archive_only.bin", "archive"}}));

    const auto catalog = Core::FileSys::GameContentCatalog::Discover(base_path);
    ASSERT_TRUE(catalog.has_value());
    EXPECT_EQ(catalog->GetPatchRoots(), (std::vector<fs::path>{loose_update, packed_update}));
    EXPECT_EQ(catalog->ResolveAppPath("shared.bin"), loose_update / "shared.bin");
    EXPECT_EQ(catalog->ResolveAppPath("archive_only.bin"), packed_update / "archive_only.bin");
    EXPECT_EQ(catalog->ResolveAppPath("base_only.bin"), base_path / "base_only.bin");
}

TEST_F(ZarFsTest, CatalogDiscoversAllInOneArchiveAndSelectsNewestUpdate) {
    const auto container_path = test_dir / "collection.zar";
    const auto base_param = MakeParam("gd", "CUSA00001", "01.00");
    const auto old_patch_param = MakeParam("gp", "CUSA00001", "01.01");
    const auto new_patch_param = MakeParam("gp", "CUSA00001", "01.20");
    const auto dlc_param =
        MakeParam("ac", "CUSA00001", "01.00", "UP0000-CUSA00001_00-DLCONE0000000000");
    ASSERT_TRUE(
        CreateArchiveWithFiles(container_path, {{"base/sce_sys/param.sfo", base_param},
                                                {"base/eboot.bin", "base"},
                                                {"old_update/sce_sys/param.sfo", old_patch_param},
                                                {"old_update/old.bin", "old"},
                                                {"new_update/sce_sys/param.sfo", new_patch_param},
                                                {"new_update/new.bin", "new"},
                                                {"dlc/sce_sys/param.sfo", dlc_param},
                                                {"dlc/content.bin", "dlc"}}));

    const auto catalog = Core::FileSys::GameContentCatalog::Discover(container_path);
    ASSERT_TRUE(catalog.has_value());
    EXPECT_EQ(catalog->GetBaseRoot(), container_path / "base");
    EXPECT_EQ(catalog->GetPatchRoots(), (std::vector<fs::path>{container_path / "new_update"}));
    EXPECT_FALSE(catalog->ResolveAppPath("old.bin").has_value());
    EXPECT_EQ(catalog->ResolveAppPath("new.bin"), container_path / "new_update/new.bin");

    const auto addons = catalog->DiscoverAdditionalContent(test_dir / "missing_addons");
    ASSERT_EQ(addons.size(), 1);
    EXPECT_EQ(addons[0].entitlement, "DLCONE0000000000");
    EXPECT_EQ(addons[0].roots, (std::vector<fs::path>{container_path / "dlc"}));
}

TEST_F(ZarFsTest, CatalogSupportsEveryLooseAndArchiveBaseUpdateCombination) {
    struct Combination {
        std::string_view title_id;
        bool packed_base;
        bool packed_update;
    };
    constexpr std::array combinations{
        Combination{"CUSA00101", false, false}, Combination{"CUSA00102", false, true},
        Combination{"CUSA00103", true, false}, Combination{"CUSA00104", true, true}};

    for (const auto& combination : combinations) {
        const auto base_stem = test_dir / combination.title_id;
        auto base_root = base_stem;
        const auto base_param = MakeParam("gd", std::string{combination.title_id}, "01.00");
        if (combination.packed_base) {
            base_root += ".zar";
            ASSERT_TRUE(CreateArchiveWithFiles(base_root, {{"sce_sys/param.sfo", base_param},
                                                           {"eboot.bin", "base"},
                                                           {"base_only.bin", "base"}}));
        } else {
            WriteFile(base_root / "sce_sys/param.sfo", base_param);
            WriteFile(base_root / "eboot.bin", "base");
            WriteFile(base_root / "base_only.bin", "base");
        }

        auto update_root = base_stem;
        update_root += "-UPDATE";
        const auto update_param = MakeParam("gp", std::string{combination.title_id}, "01.10");
        if (combination.packed_update) {
            update_root += ".zar";
            ASSERT_TRUE(CreateArchiveWithFiles(
                update_root, {{"sce_sys/param.sfo", update_param}, {"update_only.bin", "update"}}));
        } else {
            WriteFile(update_root / "sce_sys/param.sfo", update_param);
            WriteFile(update_root / "update_only.bin", "update");
        }

        const auto catalog = Core::FileSys::GameContentCatalog::Discover(base_root);
        ASSERT_TRUE(catalog.has_value()) << combination.title_id;
        EXPECT_EQ(catalog->GetBaseRoot(), base_root);
        EXPECT_EQ(catalog->ResolveAppPath("update_only.bin"), update_root / "update_only.bin");
        EXPECT_EQ(catalog->ResolveAppPath("base_only.bin"), base_root / "base_only.bin");
    }
}

TEST_F(ZarFsTest, CatalogAppliesModsAndPatchPrecedenceAndCanDisablePatches) {
    const auto base_path = test_dir / "CUSA00200";
    WriteFile(base_path / "sce_sys/param.sfo", MakeParam("gd", "CUSA00200", "01.00"));
    WriteFile(base_path / "eboot.bin", "base");

    auto update_path = base_path;
    update_path += "-UPDATE";
    WriteFile(update_path / "sce_sys/param.sfo", MakeParam("gp", "CUSA00200", "01.10"));
    WriteFile(update_path / "update_only.bin", "update");
    WriteFile(update_path / "patch_conflict.bin", "update");

    auto patch_path = base_path;
    patch_path += "-patch.zar";
    const auto patch_param = MakeParam("gp", "CUSA00200", "01.10");
    ASSERT_TRUE(CreateArchiveWithFiles(patch_path, {{"sce_sys/param.sfo", patch_param},
                                                    {"patch_only.bin", "patch"},
                                                    {"patch_conflict.bin", "patch"}}));

    auto loose_mods = base_path;
    loose_mods += "-mods";
    WriteFile(loose_mods / "shared.bin", "loose mod");

    auto packed_mods = base_path;
    packed_mods += "-mods.zar";
    ASSERT_TRUE(CreateArchiveWithFiles(
        packed_mods, {{"shared.bin", "packed mod"}, {"packed_mod_only.bin", "packed mod"}}));

    const auto catalog = Core::FileSys::GameContentCatalog::Discover(base_path);
    ASSERT_TRUE(catalog.has_value());
    EXPECT_EQ(catalog->GetPatchRoots(), (std::vector<fs::path>{update_path, patch_path}));
    EXPECT_EQ(catalog->GetModRoots(), (std::vector<fs::path>{loose_mods, packed_mods}));
    EXPECT_EQ(catalog->ResolveAppPath("shared.bin"), loose_mods / "shared.bin");
    EXPECT_EQ(catalog->ResolveAppPath("packed_mod_only.bin"), packed_mods / "packed_mod_only.bin");
    EXPECT_EQ(catalog->ResolveAppPath("patch_conflict.bin"), update_path / "patch_conflict.bin");
    EXPECT_FALSE(catalog->ResolveAppPath("update_only.bin", true).has_value());
    EXPECT_EQ(catalog->ResolveAppPath("shared.bin", true), loose_mods / "shared.bin");
}

TEST_F(ZarFsTest, CatalogFindsBaseWhenLaunchedFromUpdate) {
    const auto base_path = test_dir / "CUSA00300";
    WriteFile(base_path / "sce_sys/param.sfo", MakeParam("gd", "CUSA00300", "01.00"));
    WriteFile(base_path / "eboot.bin", "base");

    auto update_path = base_path;
    update_path += "-UPDATE";
    WriteFile(update_path / "sce_sys/param.sfo", MakeParam("gp", "CUSA00300", "01.10"));
    WriteFile(update_path / "eboot.bin", "update");

    const auto catalog = Core::FileSys::GameContentCatalog::Discover(update_path / "eboot.bin");
    ASSERT_TRUE(catalog.has_value());
    EXPECT_EQ(catalog->GetBaseRoot(), base_path);
    EXPECT_EQ(catalog->GetPatchRoots(), (std::vector<fs::path>{update_path}));
    EXPECT_EQ(catalog->ResolveAppPath("eboot.bin"), update_path / "eboot.bin");
}

TEST_F(ZarFsTest, CatalogRejectsMalformedMismatchedAndAmbiguousContent) {
    const auto base_path = test_dir / "CUSA00400";
    WriteFile(base_path / "sce_sys/param.sfo", MakeParam("gd", "CUSA00400", "01.00"));
    WriteFile(base_path / "eboot.bin", "base");

    auto update_path = base_path;
    update_path += "-UPDATE";
    WriteFile(update_path / "sce_sys/param.sfo", MakeParam("gp", "CUSA99999", "09.99"));
    WriteFile(update_path / "wrong_title.bin", "wrong");

    auto packed_update_path = base_path;
    packed_update_path += "-UPDATE.zar";
    const auto invalid_content_id =
        MakeParam("gp", "CUSA00400", "09.99", "UP0000-CUSA00400_00-TOO-SHORT");
    ASSERT_TRUE(
        CreateArchiveWithFiles(packed_update_path, {{"sce_sys/param.sfo", invalid_content_id},
                                                    {"malformed_content_id.bin", "malformed"}}));

    auto patch_path = base_path;
    patch_path += "-patch";
    auto malformed = MakeParam("gp", "CUSA00400", "99.99");
    PSFHeader malformed_header{};
    std::memcpy(&malformed_header, malformed.data(), sizeof(malformed_header));
    malformed_header.key_table_offset = 0xfffffff0;
    std::memcpy(malformed.data(), &malformed_header, sizeof(malformed_header));
    WriteFile(patch_path / "sce_sys/param.sfo", malformed);

    const auto catalog = Core::FileSys::GameContentCatalog::Discover(base_path);
    ASSERT_TRUE(catalog.has_value());
    EXPECT_TRUE(catalog->GetPatchRoots().empty());
    EXPECT_FALSE(catalog->ResolveAppPath("wrong_title.bin").has_value());
    EXPECT_FALSE(catalog->ResolveAppPath("malformed_content_id.bin").has_value());

    const auto ambiguous_path = test_dir / "ambiguous.zar";
    const auto first_param = MakeParam("gd", "CUSA00401", "01.00");
    const auto second_param = MakeParam("gd", "CUSA00402", "01.00");
    ASSERT_TRUE(CreateArchiveWithFiles(ambiguous_path, {{"first/sce_sys/param.sfo", first_param},
                                                        {"first/eboot.bin", "first"},
                                                        {"second/sce_sys/param.sfo", second_param},
                                                        {"second/eboot.bin", "second"}}));
    EXPECT_FALSE(Core::FileSys::GameContentCatalog::Discover(ambiguous_path).has_value());

    const auto invalid_archive = test_dir / "invalid.zar";
    WriteFile(invalid_archive, "not a ZArchive");
    EXPECT_FALSE(Core::FileSys::GameContentCatalog::Discover(invalid_archive).has_value());
}

TEST_F(ZarFsTest, CatalogLayersDuplicateDlcRepresentations) {
    const auto container_path = test_dir / "CUSA00500.zar";
    const auto base_param =
        MakeParam("gd", "CUSA00500", "01.00", "UP0000-CUSA00500_00-BASE000000000000");
    const auto dlc_param =
        MakeParam("ac", "CUSA00500", "01.00", "UP0000-CUSA00500_00-DLCONE0000000000");
    ASSERT_TRUE(CreateArchiveWithFiles(container_path, {{"sce_sys/param.sfo", base_param},
                                                        {"eboot.bin", "base"},
                                                        {"embedded/sce_sys/param.sfo", dlc_param},
                                                        {"embedded/embedded.bin", "embedded"}}));

    const auto addons_path = test_dir / "addons";
    const auto loose_dlc = addons_path / "CUSA00500" / "loose";
    WriteFile(loose_dlc / "sce_sys/param.sfo", dlc_param);
    WriteFile(loose_dlc / "loose.bin", "loose");

    const auto packed_dlc = addons_path / "CUSA00500.zar";
    ASSERT_TRUE(CreateArchiveWithFiles(
        packed_dlc, {{"packed/sce_sys/param.sfo", dlc_param}, {"packed/packed.bin", "packed"}}));

    const auto catalog = Core::FileSys::GameContentCatalog::Discover(container_path);
    ASSERT_TRUE(catalog.has_value());
    const auto addons = catalog->DiscoverAdditionalContent(addons_path);
    ASSERT_EQ(addons.size(), 1);
    EXPECT_EQ(addons[0].entitlement, "DLCONE0000000000");
    EXPECT_EQ(addons[0].roots, (std::vector<fs::path>{loose_dlc, packed_dlc / "packed",
                                                      container_path / "embedded"}));
}

TEST_F(ZarFsTest, KeepsOpenFileValidAfterCacheEviction) {
    auto file = OpenFile(archive_path / "eboot.bin");
    ASSERT_NE(file, nullptr);

    for (int i = 0; i < 9; ++i) {
        const auto other_archive = test_dir / ("other_" + std::to_string(i) + ".zar");
        ASSERT_TRUE(CreateArchive(other_archive));
        EXPECT_TRUE(Exists(other_archive / "eboot.bin"));
    }

    std::string contents(file->GetSize(), '\0');
    ASSERT_EQ(file->Read(contents.data(), contents.size()), contents.size());
    EXPECT_EQ(contents, "executable data");
}

} // Anonymous namespace
} // namespace Common::FS::Zar
