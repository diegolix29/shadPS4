// PKG stream extract for Android (adapted from shadPS4 PKG::Extract / ExtractFiles).
#include "pkg_extractor.h"
#include "pkg_crypto.h"
#include "pkg_type.h"
#include "pfs.h"
#include "types.h"

#include <android/log.h>
#include <unistd.h>
#include <zlib.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "BachataPkg", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "BachataPkg", __VA_ARGS__)

namespace fs = std::filesystem;
namespace bachata_pkg {
namespace {

std::atomic<bool> g_cancel{false};

struct PKGHeader {
    u32_be magic;
    u32_be pkg_type;
    u32_be pkg_0x8;
    u32_be pkg_file_count;
    u32_be pkg_table_entry_count;
    u16_be pkg_sc_entry_count;
    u16_be pkg_table_entry_count_2;
    u32_be pkg_table_entry_offset;
    u32_be pkg_sc_entry_data_size;
    u64_be pkg_body_offset;
    u64_be pkg_body_size;
    u64_be pkg_content_offset;
    u64_be pkg_content_size;
    u8 pkg_content_id[0x24];
    u8 pkg_padding[0xC];
    u32_be pkg_drm_type;
    u32_be pkg_content_type;
    u32_be pkg_content_flags;
    u32_be pkg_promote_size;
    u32_be pkg_version_date;
    u32_be pkg_version_hash;
    u32_be pkg_0x088;
    u32_be pkg_0x08C;
    u32_be pkg_0x090;
    u32_be pkg_0x094;
    u32_be pkg_iro_tag;
    u32_be pkg_drm_type_version;
    u8 pkg_zeroes_1[0x60];
    u8 digest_entries1[0x20];
    u8 digest_entries2[0x20];
    u8 digest_table_digest[0x20];
    u8 digest_body_digest[0x20];
    u8 pkg_zeroes_2[0x280];
    u32_be pkg_0x400;
    u32_be pfs_image_count;
    u64_be pfs_image_flags;
    u64_be pfs_image_offset;
    u64_be pfs_image_size;
    u64_be mount_image_offset;
    u64_be mount_image_size;
    u64_be pkg_size;
    u32_be pfs_signed_size;
    u32_be pfs_cache_size;
    u8 pfs_image_digest[0x20];
    u8 pfs_signed_digest[0x20];
    u64_be pfs_split_size_nth_0;
    u64_be pfs_split_size_nth_1;
};

struct PKGEntry {
    u32_be id;
    u32_be filename_offset;
    u32_be flags1;
    u32_be flags2;
    u32_be offset;
    u32_be size;
    u64_be padding;
};
static_assert(sizeof(PKGEntry) == 32);

bool pread_all(int fd, void* buf, size_t n, off_t off) {
    auto* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    while (got < n) {
        const ssize_t r = pread(fd, p + got, n - got, off + static_cast<off_t>(got));
        if (r <= 0) return false;
        got += static_cast<size_t>(r);
    }
    return true;
}

void DecompressPFSC(std::span<char> compressed, std::span<char> decompressed) {
    z_stream stream{};
    stream.zalloc = Z_NULL;
    stream.zfree = Z_NULL;
    stream.opaque = Z_NULL;
    if (inflateInit(&stream) != Z_OK) return;
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_in = reinterpret_cast<Bytef*>(compressed.data());
    stream.avail_out = static_cast<uInt>(decompressed.size());
    stream.next_out = reinterpret_cast<Bytef*>(decompressed.data());
    inflate(&stream, Z_FINISH);
    inflateEnd(&stream);
}

u32 GetPFSCOffset(std::span<const u8> pfs_image) {
    // Little-endian "PFSC" on disk.
    static constexpr u32 PfscMagic = 0x43534650;
    u32 value = 0;
    // shadPS4 starts at 0x20000; also scan earlier in case layout differs.
    const u32 start = pfs_image.size() > 0x20000 ? 0x10000u : 0u;
    for (u32 i = start; i + 4 <= pfs_image.size(); i += 0x10000) {
        std::memcpy(&value, &pfs_image[i], sizeof(u32));
        if (value == PfscMagic) return i;
    }
    // Byte-scan fallback every 0x1000 for atypical images.
    for (u32 i = 0; i + 4 <= pfs_image.size(); i += 0x1000) {
        std::memcpy(&value, &pfs_image[i], sizeof(u32));
        if (value == PfscMagic) return i;
    }
    return static_cast<u32>(-1);
}

bool safe_under(const fs::path& root, const fs::path& child) {
    const auto r = fs::weakly_canonical(root);
    std::error_code ec;
    const auto c = fs::weakly_canonical(child, ec);
    if (ec) {
        // weakly_canonical may fail for non-existing; check lexically
        auto rel = child.lexically_relative(root);
        return !rel.empty() && rel.native().find("..") == std::string::npos;
    }
    auto rel = c.lexically_relative(r);
    return !rel.empty() && rel.native().find("..") == std::string::npos;
}

struct ExtractState {
    PKGHeader hdr{};
    Crypto crypto;
    std::array<u8, 32> ekpfsKey{};
    std::array<u8, 16> dataKey{};
    std::array<u8, 16> tweakKey{};
    std::vector<pfs_fs_table> fsTable;
    std::vector<Inode> iNodeBuf;
    std::vector<u64> sectorMap;
    u64 pfsc_offset = 0;
    std::unordered_map<int, fs::path> extractPaths;
    fs::path extract_root;
    fs::path current_dir;
    std::string content_id;
    std::string title_id;
};

bool read_header(int fd, PKGHeader& hdr) {
    return pread_all(fd, &hdr, sizeof(hdr), 0);
}

bool digests_equal(const std::array<u8, 32>& a, const std::array<u8, 32>& b) {
    return std::memcmp(a.data(), b.data(), 32) == 0;
}

bool derive_keys_from_entries(int fd, ExtractState& st, const char* passcode, std::string& err) {
    const u32 offset = st.hdr.pkg_table_entry_offset;
    const u32 n_files = st.hdr.pkg_table_entry_count;
    std::array<u8, 32> seed_digest{};
    std::array<std::array<u8, 32>, 7> digest1{};
    std::array<std::array<u8, 256>, 7> key1{};
    std::array<u8, 256> imgkeydata{};
    std::array<u8, 32> dk3{};
    std::array<u8, 32> ivKey{};
    std::array<u8, 256> imgKey{};
    bool have_entry_keys = false;
    bool have_image_key = false;
    bool dk3_ok = false;
    PKGEntry image_entry{};

    for (u32 i = 0; i < n_files; ++i) {
        PKGEntry entry{};
        if (!pread_all(fd, &entry, sizeof(entry), static_cast<off_t>(offset) + i * 32)) {
            err = "Failed reading entry table";
            return false;
        }
        const u32 id = u32(entry.id);
        if (id == 0x10) { // ENTRY_KEYS
            off_t pos = static_cast<off_t>(u32(entry.offset));
            if (!pread_all(fd, seed_digest.data(), 32, pos)) return false;
            pos += 32;
            for (int k = 0; k < 7; ++k) {
                if (!pread_all(fd, digest1[k].data(), 32, pos)) return false;
                pos += 32;
            }
            for (int k = 0; k < 7; ++k) {
                if (!pread_all(fd, key1[k].data(), 256, pos)) return false;
                pos += 256;
            }
            try {
                st.crypto.RSA2048Decrypt(dk3, key1[3], true);
                dk3_ok = true;
            } catch (const std::exception& ex) {
                LOGI("DK3 RSA decrypt failed (may be ok for passcode pkgs): %s", ex.what());
                dk3_ok = false;
            }
            have_entry_keys = true;
        } else if (id == 0x20) { // IMAGE_KEY
            if (!pread_all(fd, imgkeydata.data(), 256, static_cast<off_t>(u32(entry.offset)))) {
                return false;
            }
            image_entry = entry;
            have_image_key = true;
        }
    }

    if (!have_entry_keys) {
        err = "PKG missing ENTRY_KEYS";
        return false;
    }

    // Passcode path: MUST verify digest0 before accepting (LibOrbisPkg CheckPasscode).
    // Unverified ComputeKeys always "works" and previously poisoned EKPFS → PFSC not found.
    if (passcode && std::strlen(passcode) == 32 && st.content_id.size() == 36) {
        std::array<u8, 32> dk0{};
        std::array<u8, 32> digest0{};
        if (Crypto::ComputeKeys(st.content_id, passcode, 0, dk0)) {
            Crypto::XorSha256Digest(dk0, digest0);
            if (digests_equal(digest0, digest1[0])) {
                std::array<u8, 32> ek{};
                if (Crypto::ComputeKeys(st.content_id, passcode, 1, ek)) {
                    st.ekpfsKey = ek;
                    LOGI("Using verified passcode-derived EKPFS");
                    return true;
                }
            } else {
                LOGI("passcode digest mismatch (not accepting ComputeKeys)");
            }
        }
    }

    // Fake/homebrew PKG: RSA-decrypt IMAGE_KEY with FakeKeyset after DK3 unwrap.
    if (have_entry_keys && have_image_key && dk3_ok) {
        std::array<u8, 64> concat{};
        std::memcpy(concat.data(), &image_entry, sizeof(image_entry));
        std::memcpy(concat.data() + sizeof(image_entry), dk3.data(), 32);
        st.crypto.ivKeyHASH256(concat, ivKey);
        st.crypto.aesCbcCfb128Decrypt(ivKey, imgkeydata, imgKey);
        try {
            st.crypto.RSA2048Decrypt(st.ekpfsKey, imgKey, false);
            // Optional: verify derived key digest index 1 when present.
            std::array<u8, 32> ek_digest{};
            Crypto::XorSha256Digest(st.ekpfsKey, ek_digest);
            if (!digests_equal(ek_digest, digest1[1])) {
                LOGI("RSA EKPFS digest1 mismatch — still trying (some fakes differ)");
            }
            LOGI("Using fake-pkg EKPFS via RSA");
            return true;
        } catch (const std::exception& ex) {
            err = std::string("EKPFS RSA failed: ") + ex.what();
            LOGI("%s", err.c_str());
            // fall through to NEED_PASSCODE
        }
    }

    err = "NEED_PASSCODE";
    return false;
}

bool build_fs_table(int fd, ExtractState& st, std::string& err) {
    std::array<u8, 16> seed{};
    if (!pread_all(fd, seed.data(), 16, static_cast<off_t>(u64(st.hdr.pfs_image_offset) + 0x370))) {
        err = "Failed to read PFS seed";
        return false;
    }
    const u64 pfs_flags = u64(st.hdr.pfs_image_flags);
    const bool new_crypt = (pfs_flags & 0x2000000000000000ULL) != 0;
    st.crypto.PfsGenCryptoKey(st.ekpfsKey, seed, st.dataKey, st.tweakKey, new_crypt);
    LOGI("PfsGenCryptoKey new_crypt=%d pfs_flags=0x%llx",
         new_crypt ? 1 : 0,
         static_cast<unsigned long long>(pfs_flags));
    // Match shadPS4: decrypt first pfs_cache_size*2 bytes of outer PFS image.
    u32 length = u32(st.hdr.pfs_cache_size) * 2;
    if (length == 0) {
        length = 0x100000;
        LOGI("pfs_cache_size=0, fallback length=0x%x", length);
    }
    if (length < 0x40000) length = 0x40000;
    const u64 pfs_off = u64(st.hdr.pfs_image_offset);
    const u64 pfs_sz = u64(st.hdr.pfs_image_size);
    if (pfs_sz > 0 && length > pfs_sz) length = static_cast<u32>(pfs_sz);
    // LibOrbisPkg: crypt starts at BlockSize/0x1000 (usually 16).
    constexpr uint64_t kCryptStartSector = 16;
    LOGI("build_fs_table pfs_off=%llu pfs_sz=%llu cache=%u length=%u cryptStart=%llu",
         static_cast<unsigned long long>(pfs_off),
         static_cast<unsigned long long>(pfs_sz),
         u32(st.hdr.pfs_cache_size),
         length,
         static_cast<unsigned long long>(kCryptStartSector));
    std::vector<u8> pfs_encrypted(length);
    std::vector<u8> pfs_decrypted(length);
    if (!pread_all(fd, pfs_encrypted.data(), length, static_cast<off_t>(pfs_off))) {
        err = "Failed reading PFS header region";
        return false;
    }
    auto try_find_pfsc = [&](bool use_new_crypt) -> bool {
        st.crypto.PfsGenCryptoKey(st.ekpfsKey, seed, st.dataKey, st.tweakKey, use_new_crypt);
        st.crypto.decryptPFS(st.dataKey, st.tweakKey, pfs_encrypted, pfs_decrypted, 0, kCryptStartSector);
        st.pfsc_offset = GetPFSCOffset(pfs_decrypted);
        return st.pfsc_offset != static_cast<u32>(-1) && st.pfsc_offset < length;
    };
    bool found = try_find_pfsc(new_crypt);
    if (!found && !new_crypt) {
        LOGI("PFSC miss with new_crypt=0, retry new_crypt=1");
        found = try_find_pfsc(true);
    }
    if (!found && new_crypt) {
        LOGI("PFSC miss with new_crypt=1, retry new_crypt=0");
        found = try_find_pfsc(false);
    }
    // Also try crypt_start_sector=0 (shadPS4 style) if still missing.
    if (!found) {
        LOGI("PFSC miss with cryptStart=16, retry cryptStart=0");
        st.crypto.PfsGenCryptoKey(st.ekpfsKey, seed, st.dataKey, st.tweakKey, new_crypt);
        st.crypto.decryptPFS(st.dataKey, st.tweakKey, pfs_encrypted, pfs_decrypted, 0, 0);
        st.pfsc_offset = GetPFSCOffset(pfs_decrypted);
        found = st.pfsc_offset != static_cast<u32>(-1) && st.pfsc_offset < length;
    }
    if (!found) {
        LOGI("PFSC not found; plain[0..15]=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
             pfs_encrypted[0], pfs_encrypted[1], pfs_encrypted[2], pfs_encrypted[3],
             pfs_encrypted[4], pfs_encrypted[5], pfs_encrypted[6], pfs_encrypted[7],
             pfs_encrypted[8], pfs_encrypted[9], pfs_encrypted[10], pfs_encrypted[11],
             pfs_encrypted[12], pfs_encrypted[13], pfs_encrypted[14], pfs_encrypted[15]);
        LOGI("PFSC not found; decrypted[0..15]=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x",
             pfs_decrypted[0], pfs_decrypted[1], pfs_decrypted[2], pfs_decrypted[3],
             pfs_decrypted[4], pfs_decrypted[5], pfs_decrypted[6], pfs_decrypted[7],
             pfs_decrypted[8], pfs_decrypted[9], pfs_decrypted[10], pfs_decrypted[11],
             pfs_decrypted[12], pfs_decrypted[13], pfs_decrypted[14], pfs_decrypted[15]);
        if (length > 0x20000) {
            LOGI("decrypted@0x20000=%02x%02x%02x%02x",
                 pfs_decrypted[0x20000], pfs_decrypted[0x20001],
                 pfs_decrypted[0x20002], pfs_decrypted[0x20003]);
        }
        err = "PFSC magic not found (wrong keys or unsupported PFS layout)";
        return false;
    }
    LOGI("PFSC offset=0x%llx", static_cast<unsigned long long>(st.pfsc_offset));
    std::vector<u8> pfsc(length - st.pfsc_offset);
    std::memcpy(pfsc.data(), pfs_decrypted.data() + st.pfsc_offset, pfsc.size());
    PFSCHdr pfsChdr{};
    std::memcpy(&pfsChdr, pfsc.data(), sizeof(pfsChdr));
    const int num_blocks = static_cast<int>(pfsChdr.data_length / pfsChdr.block_sz2);
    st.sectorMap.resize(num_blocks + 1);
    for (int i = 0; i < num_blocks + 1; ++i) {
        std::memcpy(&st.sectorMap[i], pfsc.data() + pfsChdr.block_offsets + i * 8, 8);
    }

    u32 ent_size = 0;
    u32 ndinode = 0;
    int ndinode_counter = 0;
    bool dinode_reached = false;
    bool uroot_reached = false;
    std::vector<char> compressedData;
    std::vector<char> decompressedData(0x10000);

    st.extractPaths[0] = st.extract_root;
    st.current_dir = st.extract_root;

    for (int i = 0; i < num_blocks; ++i) {
        if (g_cancel.load()) {
            err = "CANCELLED";
            return false;
        }
        const u64 sectorOffset = st.sectorMap[i];
        const u64 sectorSize = st.sectorMap[i + 1] - sectorOffset;
        compressedData.resize(static_cast<size_t>(sectorSize));
        if (sectorOffset + sectorSize > pfsc.size()) break;
        std::memcpy(compressedData.data(), pfsc.data() + sectorOffset, static_cast<size_t>(sectorSize));
        if (sectorSize == 0x10000) {
            std::memcpy(decompressedData.data(), compressedData.data(), 0x10000);
        } else if (sectorSize < 0x10000) {
            DecompressPFSC(compressedData, decompressedData);
        }

        if (i == 0) {
            std::memcpy(&ndinode, decompressedData.data() + 0x30, 4);
        }
        int occupied_blocks = static_cast<int>((ndinode * 0xA8) / 0x10000);
        if (((ndinode * 0xA8) % 0x10000) != 0) occupied_blocks += 1;

        if (i >= 1 && i <= occupied_blocks) {
            for (int p = 0; p < 0x10000; p += 0xA8) {
                Inode node{};
                std::memcpy(&node, &decompressedData[p], sizeof(node));
                if (node.Mode == 0) break;
                st.iNodeBuf.push_back(node);
            }
        }

        const std::string_view flat_path_table(&decompressedData[0x10], 15);
        if (flat_path_table == "flat_path_table") uroot_reached = true;

        if (uroot_reached) {
            for (int j = 0; j < 0x10000; j += static_cast<int>(ent_size ? ent_size : 1)) {
                Dirent dirent{};
                std::memcpy(&dirent, &decompressedData[j], sizeof(dirent));
                ent_size = dirent.entsize;
                if (ent_size == 0) break;
                if (dirent.ino != 0) {
                    ndinode_counter++;
                } else {
                    st.extractPaths[ndinode_counter] = st.extract_root;
                    uroot_reached = false;
                    break;
                }
            }
        }

        const char dot = decompressedData[0x10];
        const std::string_view dotdot(&decompressedData[0x28], 2);
        if (dot == '.' && dotdot == "..") dinode_reached = true;

        bool end_reached = false;
        if (dinode_reached) {
            for (int j = 0; j < 0x10000; j += static_cast<int>(ent_size ? ent_size : 1)) {
                Dirent dirent{};
                std::memcpy(&dirent, &decompressedData[j], sizeof(dirent));
                if (dirent.ino == 0) break;
                ent_size = dirent.entsize;
                if (ent_size == 0) break;
                pfs_fs_table table{};
                table.name = std::string(dirent.name, dirent.namelen);
                table.inode = dirent.ino;
                table.type = dirent.type;
                if (table.type == PFS_CURRENT_DIR) {
                    auto it = st.extractPaths.find(table.inode);
                    if (it != st.extractPaths.end()) st.current_dir = it->second;
                }
                st.extractPaths[table.inode] = st.current_dir / fs::path(table.name);
                if (table.type == PFS_FILE || table.type == PFS_DIR) {
                    if (table.type == PFS_DIR) {
                        const auto& p = st.extractPaths[table.inode];
                        if (safe_under(st.extract_root, p)) fs::create_directories(p);
                    }
                    st.fsTable.push_back(table);
                    ndinode_counter++;
                    if ((ndinode_counter + 1) == static_cast<int>(ndinode)) end_reached = true;
                }
            }
            if (end_reached) break;
        }
    }
    return true;
}

bool extract_file(int fd, ExtractState& st, const pfs_fs_table& table, std::string& err) {
    if (table.type != PFS_FILE) return true;
    if (table.inode < 0 || static_cast<size_t>(table.inode) >= st.iNodeBuf.size()) {
        err = "Bad inode";
        return false;
    }
    const Inode& node = st.iNodeBuf[table.inode];
    const int sector_loc = node.loc;
    const int nblocks = node.Blocks;
    const int bsize = node.Size;
    auto path_it = st.extractPaths.find(table.inode);
    if (path_it == st.extractPaths.end()) return true;
    const fs::path out_path = path_it->second;
    if (!safe_under(st.extract_root, out_path)) {
        err = "Path escapes extract root";
        return false;
    }
    fs::create_directories(out_path.parent_path());
    std::ofstream out(out_path, std::ios::binary);
    if (!out) {
        err = "Cannot open output file";
        return false;
    }

    int size_decompressed = 0;
    std::vector<char> compressedData;
    std::vector<char> decompressedData(0x10000);
    constexpr u64 pfsc_buf_size = 0x11000;
    std::vector<u8> pfsc(pfsc_buf_size);
    std::vector<u8> pfs_decrypted(pfsc_buf_size);

    for (int j = 0; j < nblocks; ++j) {
        if (g_cancel.load()) {
            err = "CANCELLED";
            return false;
        }
        if (sector_loc + j + 1 >= static_cast<int>(st.sectorMap.size())) {
            err = "sector map OOB";
            return false;
        }
        const u64 sectorOffset = st.sectorMap[sector_loc + j];
        const u64 sectorSize = st.sectorMap[sector_loc + j + 1] - sectorOffset;
        const u64 fileOffset = u64(st.hdr.pfs_image_offset) + st.pfsc_offset + sectorOffset;
        const u64 currentSector1 = (st.pfsc_offset + sectorOffset) / 0x1000;
        const int sectorOffsetMask = static_cast<int>((sectorOffset + st.pfsc_offset) & 0xFFFFF000);
        const int previousData = static_cast<int>((sectorOffset + st.pfsc_offset) - sectorOffsetMask);

        if (!pread_all(fd, pfsc.data(), pfsc_buf_size, static_cast<off_t>(fileOffset - previousData))) {
            err = "PFS read failed";
            return false;
        }
        st.crypto.decryptPFS(st.dataKey, st.tweakKey, pfsc, pfs_decrypted, currentSector1);
        compressedData.resize(static_cast<size_t>(sectorSize));
        std::memcpy(compressedData.data(), pfs_decrypted.data() + previousData, static_cast<size_t>(sectorSize));
        if (sectorSize == 0x10000) {
            std::memcpy(decompressedData.data(), compressedData.data(), 0x10000);
        } else if (sectorSize < 0x10000) {
            DecompressPFSC(compressedData, decompressedData);
        }
        size_decompressed += 0x10000;
        if (j < nblocks - 1) {
            out.write(decompressedData.data(), static_cast<std::streamsize>(decompressedData.size()));
        } else {
            const u32 write_size = static_cast<u32>(decompressedData.size() - (size_decompressed - bsize));
            out.write(decompressedData.data(), static_cast<std::streamsize>(write_size));
        }
    }
    return true;
}

// Write raw sce_sys entries from package table (param.sfo etc).
bool extract_sce_sys_entries(int fd, ExtractState& st, std::string& err) {
    const u32 offset = st.hdr.pkg_table_entry_offset;
    const u32 n_files = st.hdr.pkg_table_entry_count;
    fs::create_directories(st.extract_root / "sce_sys");
    for (u32 i = 0; i < n_files; ++i) {
        PKGEntry entry{};
        if (!pread_all(fd, &entry, sizeof(entry), static_cast<off_t>(offset) + i * 32)) {
            err = "entry read failed";
            return false;
        }
        const auto name = GetEntryNameByType(entry.id);
        if (name.empty()) continue;
        std::vector<u8> data(entry.size);
        if (entry.size > 0) {
            if (!pread_all(fd, data.data(), entry.size, entry.offset)) {
                err = "entry data read failed";
                return false;
            }
        }
        const fs::path out = st.extract_root / "sce_sys" / std::string(name);
        if (!safe_under(st.extract_root, out)) continue;
        std::ofstream f(out, std::ios::binary);
        f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }
    return true;
}

} // namespace

void bachata_pkg_cancel(void) { g_cancel.store(true); }

int bachata_pkg_probe(int fd, BachataPkgProbe* out) {
    LOGI("probe start fd=%d", fd);
    if (!out) return 3;
    std::memset(out, 0, sizeof(*out));
    g_cancel.store(false);
    PKGHeader hdr{};
    if (!read_header(fd, hdr) || u32(hdr.magic) != 0x7F434E54) {
        out->status = 3;
        std::snprintf(out->message, sizeof(out->message), "Invalid PKG header");
        LOGE("probe bad header magic");
        return 3;
    }
    char cid[0x30]{};
    std::memcpy(cid, hdr.pkg_content_id, 0x24);
    std::snprintf(out->content_id, sizeof(out->content_id), "%s", cid);
    out->package_size = u64(hdr.pkg_size);
    out->pfs_image_size = u64(hdr.pfs_image_size);
    // Title hint: last 9 of content after EP...
    if (std::strlen(cid) >= 16) {
        std::snprintf(out->title_hint, sizeof(out->title_hint), "%.9s", cid + 7);
    }
    LOGI("probe header ok contentId=%s pkgSize=%llu pfsSize=%llu",
         cid,
         static_cast<unsigned long long>(out->package_size),
         static_cast<unsigned long long>(out->pfs_image_size));
    // Auth probe with zero passcode + fake path
    ExtractState st;
    st.hdr = hdr;
    st.content_id = cid;
    std::string err;
    LOGI("probe try zero-passcode keys");
    if (derive_keys_from_entries(fd, st, "00000000000000000000000000000000", err)) {
        out->status = 0;
        LOGI("probe ok via zero-passcode");
        return 0;
    }
    if (err == "NEED_PASSCODE") {
        out->status = 1;
        std::snprintf(out->message, sizeof(out->message), "Passcode required");
        LOGI("probe need passcode (after zero)");
        return 1;
    }
    // try without passcode (RSA only)
    err.clear();
    LOGI("probe try RSA/EKPFS keys");
    if (derive_keys_from_entries(fd, st, nullptr, err)) {
        out->status = 0;
        LOGI("probe ok via RSA/EKPFS");
        return 0;
    }
    if (err == "NEED_PASSCODE") {
        out->status = 1;
        std::snprintf(out->message, sizeof(out->message), "Passcode required");
        LOGI("probe need passcode (after RSA)");
        return 1;
    }
    out->status = 3;
    std::snprintf(out->message, sizeof(out->message), "%s", err.c_str());
    LOGE("probe error: %s", err.c_str());
    return 3;
}

int bachata_pkg_extract(int fd, const char* out_path, const char* passcode_or_null,
                        void (*progress)(void* ctx, uint64_t done, uint64_t total, const char* file),
                        void* progress_ctx) {
    LOGI("extract start fd=%d out=%s hasPasscode=%d",
         fd,
         out_path ? out_path : "(null)",
         passcode_or_null && passcode_or_null[0] ? 1 : 0);
    g_cancel.store(false);
    if (!out_path) return 3;
    ExtractState st;
    if (!read_header(fd, st.hdr) || u32(st.hdr.magic) != 0x7F434E54) {
        LOGE("extract bad header");
        return 3;
    }
    char cid[0x30]{};
    std::memcpy(cid, st.hdr.pkg_content_id, 0x24);
    st.content_id = cid;
    if (std::strlen(cid) >= 16) st.title_id.assign(cid + 7, 9);
    st.extract_root = fs::path(out_path);
    fs::create_directories(st.extract_root);

    std::string err;
    const char* pass = passcode_or_null;
    // Try provided passcode, then zero, then RSA-only.
    bool keyed = false;
    if (pass && std::strlen(pass) == 32) {
        LOGI("extract derive keys with provided passcode");
        keyed = derive_keys_from_entries(fd, st, pass, err);
    }
    if (!keyed) {
        err.clear();
        LOGI("extract derive keys with zero passcode");
        keyed = derive_keys_from_entries(fd, st, "00000000000000000000000000000000", err);
    }
    if (!keyed) {
        err.clear();
        LOGI("extract derive keys via RSA/EKPFS");
        keyed = derive_keys_from_entries(fd, st, nullptr, err);
    }
    if (!keyed) {
        if (err == "NEED_PASSCODE") {
            LOGI("extract need passcode");
            return 1;
        }
        LOGE("key derive: %s", err.c_str());
        return 3;
    }
    LOGI("extract keys ok contentId=%s", cid);

    LOGI("extract sce_sys entries");
    if (!extract_sce_sys_entries(fd, st, err)) {
        LOGE("sce_sys: %s", err.c_str());
        return 3;
    }
    LOGI("extract build fs table");
    if (!build_fs_table(fd, st, err)) {
        if (err == "CANCELLED") return 2;
        LOGE("fs table: %s", err.c_str());
        return 3;
    }
    LOGI("extract fs table files=%zu inodes=%zu", st.fsTable.size(), st.iNodeBuf.size());

    uint64_t total = 0;
    for (const auto& t : st.fsTable) {
        if (t.type == PFS_FILE && t.inode >= 0 && static_cast<size_t>(t.inode) < st.iNodeBuf.size()) {
            total += static_cast<uint64_t>(st.iNodeBuf[t.inode].Size);
        }
    }
    LOGI("extract total_bytes=%llu", static_cast<unsigned long long>(total));
    uint64_t done = 0;
    size_t file_index = 0;
    for (const auto& t : st.fsTable) {
        if (g_cancel.load()) {
            LOGI("extract cancelled");
            return 2;
        }
        if (t.type != PFS_FILE) continue;
        ++file_index;
        if (file_index == 1 || (file_index % 25) == 0) {
            LOGI("extract file #%zu name=%s", file_index, t.name.c_str());
        }
        if (!extract_file(fd, st, t, err)) {
            if (err == "CANCELLED") return 2;
            LOGE("extract %s: %s", t.name.c_str(), err.c_str());
            return 3;
        }
        if (t.inode >= 0 && static_cast<size_t>(t.inode) < st.iNodeBuf.size()) {
            done += static_cast<uint64_t>(st.iNodeBuf[t.inode].Size);
        }
        if (progress) progress(progress_ctx, done, total, t.name.c_str());
    }
    LOGI("extract done files=%zu bytes=%llu", file_index, static_cast<unsigned long long>(done));
    return 0;
}

} // namespace bachata_pkg

// C ABI
extern "C" {

void bachata_pkg_cancel(void) { bachata_pkg::bachata_pkg_cancel(); }

int bachata_pkg_probe(int fd, BachataPkgProbe* out) { return bachata_pkg::bachata_pkg_probe(fd, out); }

int bachata_pkg_extract(int fd, const char* out_path, const char* passcode_or_null,
                        void (*progress)(void* ctx, uint64_t done, uint64_t total, const char* file),
                        void* progress_ctx) {
    return bachata_pkg::bachata_pkg_extract(fd, out_path, passcode_or_null, progress, progress_ctx);
}

}
