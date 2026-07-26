// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/file_format/pkg.h"
#include "common/alignment.h"
#include "common/io_file.h"
#include "common/logging/formatter.h"
#include "common/logging/log.h"
#include "core/file_format/pkg_type.h"
#include <atomic>
#include <chrono>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <span>
#include <sstream>
#include <thread>
#include <zlib.h>

static void DecompressPFSC(char *compressed_data, size_t compressed_size,
                           char *decompressed_data, size_t decompressed_size) {
  z_stream decompressStream;
  decompressStream.zalloc = Z_NULL;
  decompressStream.zfree = Z_NULL;
  decompressStream.opaque = Z_NULL;

  if (inflateInit(&decompressStream) != Z_OK) {
    // std::cerr << "Error initializing zlib for deflation." << std::endl;
  }

  decompressStream.avail_in = static_cast<uInt>(compressed_size);
  decompressStream.next_in = reinterpret_cast<unsigned char *>(compressed_data);
  decompressStream.avail_out = static_cast<uInt>(decompressed_size);
  decompressStream.next_out =
      reinterpret_cast<unsigned char *>(decompressed_data);

  if (inflate(&decompressStream, Z_FINISH)) {
  }
  if (inflateEnd(&decompressStream) != Z_OK) {
    // std::cerr << "Error ending zlib inflate" << std::endl;
  }
}

u32 GetPFSCOffset(const u8 *pfs_image, size_t size) {
  static constexpr u32 PfscMagic = 0x43534650;
  u32 value;
  for (u32 i = 0x20000; i < size; i += 0x10000) {
    std::memcpy(&value, pfs_image + i, sizeof(u32));
    if (value == PfscMagic)
      return i;
  }
  return -1;
}

PKG::PKG() = default;

PKG::~PKG() = default;

bool PKG::Open(const std::filesystem::path &filepath, std::string &failreason) {
  // LOG_DEBUG(Common, "Inizio PKG::Open su {}", filepath.string());
  
  // Validate file path
  if (filepath.empty()) {
    failreason = "Empty file path";
    return false;
  }
  
  // Convert to string for validation
  std::string filepath_str;
  try {
    filepath_str = filepath.string();
  } catch (const std::exception& e) {
    failreason = std::string("Failed to convert path to string: ") + e.what();
    return false;
  }
  
  if (filepath_str.empty()) {
    failreason = "Path string is empty after conversion";
    return false;
  }
  
  Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Read);
  if (!file.IsOpen()) {
    // LOG_ERROR(Common, "File non aperto: {}", filepath.string());
    failreason = "Failed to open PKG file";
    return false;
  }
  
  pkgSize = file.GetSize();
  if (pkgSize < sizeof(PKGHeader)) {
    failreason = "PKG file is too small to contain header";
    return false;
  }

  try {
    file.Read(pkgheader);
  } catch (const std::exception& e) {
    failreason = std::string("Exception reading PKG header: ") + e.what();
    return false;
  }
  
  if (pkgheader.magic != 0x7F434E54) {
    // LOG_ERROR(Common, "Magic non valido nel PKG header");
    failreason = "Invalid PKG magic number";
    return false;
  }

  for (const auto &flag : flagNames) {
    if (isFlagSet(pkgheader.pkg_content_flags, flag.first)) {
      if (!pkgFlags.empty())
        pkgFlags += (", ");
      pkgFlags += (flag.second);
    }
  }

  // Find title id it is part of pkg_content_id starting at offset 0x40
  file.Seek(0x47); // skip first 7 characters of content_id
  file.Read(pkgTitleID);

  u32 offset = pkgheader.pkg_table_entry_offset;
  u32 n_files = pkgheader.pkg_table_entry_count;

  // LOG_DEBUG(Common, "Table entry offset: {}, count: {}", offset, n_files);
  
  // Validate offset and count
  if (offset >= pkgSize) {
    failreason = "Invalid PKG table entry offset (beyond file size)";
    return false;
  }
  
  if (n_files > 10000) { // Sanity check
    failreason = "Invalid PKG table entry count (too large)";
    return false;
  }

  if (!file.Seek(offset)) {
    failreason = "Failed to seek to PKG table entry offset";
    // LOG_ERROR(Common, "{}", failreason);
    return false;
  }

  pkgEntries.clear();
  for (int i = 0; i < n_files; i++) {
    PKGEntry entry{};
    file.Read(entry.id);
    file.Read(entry.filename_offset);
    file.Read(entry.flags1);
    file.Read(entry.flags2);
    file.Read(entry.offset);
    file.Read(entry.size);
    file.Seek(8, Common::FS::SeekOrigin::CurrentPosition);
    pkgEntries.push_back(entry);
    // Try to figure out the name
    const auto name = GetEntryNameByType(entry.id);

    // Direct debug output to stderr
    // LOG_DEBUG(Common, "Entry {}: id={} (0x{:x}), name={}", i, entry.id,
    //           (u32)entry.id, std::string(name));

    if (name == "param.sfo") {
      sfo.clear();
      if (!file.Seek(entry.offset)) {
        failreason = "Failed to seek to param.sfo offset";
        // LOG_ERROR(Common, "{}", failreason);
        return false;
      }
      
      // Validate param.sfo size
      if (entry.size > 0x100000) { // 1MB sanity check
        failreason = "param.sfo size is too large";
        return false;
      }
      
      if (entry.offset + entry.size > pkgSize) {
        failreason = "param.sfo extends beyond file size";
        return false;
      }
      
      sfo.resize(entry.size);
      file.ReadRaw<u8>(sfo.data(), entry.size);
    }
  }
  file.Close();

  // LOG_DEBUG(Common, "Fine PKG::Open");
  return true;
}

bool PKG::Extract(const std::filesystem::path &filepath,
                  const std::filesystem::path &extract,
                  std::string &failreason) {
  // LOG_DEBUG(Common, "Inizio PKG::Extract su {} -> {}", filepath.string(),
  //           extract.string());
  
  // Validate input paths
  if (filepath.empty()) {
    failreason = "Empty PKG file path";
    return false;
  }
  
  if (extract.empty()) {
    failreason = "Empty extraction path";
    return false;
  }
  
  extract_path = extract;
  pkgpath = filepath;
  Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Read);
  if (!file.IsOpen()) {
    // LOG_ERROR(Common, "File non aperto in Extract: {}", filepath.string());
    failreason = "Failed to open PKG file for extraction";
    return false;
  }
  pkgSize = file.GetSize();
  
  if (pkgSize < sizeof(PKGHeader)) {
    failreason = "PKG file is too small to contain header";
    return false;
  }
  
  file.ReadRaw<u8>(&pkgheader, sizeof(PKGHeader));

  // LOG_DEBUG(Common, "pkgheader.magic: {}", pkgheader.magic);
  // LOG_DEBUG(Common, "pkgheader.pkg_size: {}", pkgheader.pkg_size);
  // LOG_DEBUG(Common, "pkgheader.pkg_content_size: {}",
  //           pkgheader.pkg_content_size);
  // LOG_DEBUG(Common, "pkgheader.pkg_content_offset: {}",
  //           pkgheader.pkg_content_offset);
  // LOG_DEBUG(Common, "pkgheader.pkg_table_entry_offset: {}",
  //           pkgheader.pkg_table_entry_offset);
  // LOG_DEBUG(Common, "pkgheader.pkg_table_entry_count: {}",
  //           pkgheader.pkg_table_entry_count);
  // LOG_DEBUG(Common, "pkgheader.pfs_image_offset: {}",
  //           pkgheader.pfs_image_offset);
  // LOG_DEBUG(Common, "pkgheader.pfs_cache_size: {}", pkgheader.pfs_cache_size);

  if (pkgheader.magic != 0x7F434E54) {
    // LOG_ERROR(Common, "Magic non valido in Extract");
    failreason = "Invalid PKG magic number in Extract";
    return false;
  }

  if (pkgheader.pkg_size > pkgSize) {
    failreason = "PKG file size is different";
    // LOG_ERROR(Common, "{}", failreason);
    return false;
  }
  if ((pkgheader.pkg_content_size + pkgheader.pkg_content_offset) >
      pkgheader.pkg_size) {
    failreason = "Content size is bigger than pkg size";
    // LOG_ERROR(Common, "{}", failreason);
    return false;
  }

  u32 offset = pkgheader.pkg_table_entry_offset;
  u32 n_files = pkgheader.pkg_table_entry_count;
  // LOG_DEBUG(Common, "Table entry offset: {}, count: {}", offset, n_files);

  std::array<u8, 64> concatenated_ivkey_dk3;
  std::array<u8, 32> seed_digest;
  std::array<std::array<u8, 32>, 7> digest1;
  std::array<std::array<u8, 256>, 7> key1;
  std::array<u8, 256> imgkeydata;

  if (!file.Seek(offset)) {
    failreason = "Failed to seek to PKG table entry offset";
    return false;
  }

  for (int i = 0; i < n_files; i++) {
    PKGEntry entry{};
    file.Read(entry.id);
    file.Read(entry.filename_offset);
    file.Read(entry.flags1);
    file.Read(entry.flags2);
    file.Read(entry.offset);
    file.Read(entry.size);
    file.Seek(8, Common::FS::SeekOrigin::CurrentPosition);

    auto currentPos = file.Tell();

    // Try to figure out the name
    const auto name = GetEntryNameByType(entry.id);
    const auto filepath = extract_path / "sce_sys" / name;
    std::filesystem::create_directories(filepath.parent_path());

    if (name.empty()) {
      // Just print with id
      Common::FS::IOFile out(extract_path / "sce_sys" /
                                 std::to_string(entry.id),
                             Common::FS::FileAccessMode::Write);
      if (!file.Seek(entry.offset)) {
        failreason = "Failed to seek to PKG entry offset";
        return false;
      }

      std::vector<u8> data;
      data.resize(entry.size);
      file.ReadRaw<u8>(data.data(), entry.size);
      out.WriteRaw<u8>(data.data(), data.size());
      out.Close();

      file.Seek(currentPos);
      continue;
    }

    if (entry.id == 0x1) {         // DIGESTS, seek;
                                   // file.Seek(entry.offset, fsSeekSet);
    } else if (entry.id == 0x10) { // ENTRY_KEYS, seek;
      file.Seek(entry.offset);
      file.Read(seed_digest);

      for (int i = 0; i < 7; i++) {
        file.Read(digest1[i]);
      }

      for (int i = 0; i < 7; i++) {
        file.Read(key1[i]);
      }

      PKG::crypto.RSA2048Decrypt(dk3_, key1[3], true); // decrypt DK3
    } else if (entry.id == 0x20) { // IMAGE_KEY, seek; IV_KEY
      file.Seek(entry.offset);
      file.Read(imgkeydata);

      // The Concatenated iv + dk3 imagekey for HASH256
      std::memcpy(concatenated_ivkey_dk3.data(), &entry, sizeof(entry));
      std::memcpy(concatenated_ivkey_dk3.data() + sizeof(entry), dk3_.data(),
                  sizeof(dk3_));

      PKG::crypto.ivKeyHASH256(concatenated_ivkey_dk3, ivKey); // ivkey_
      // imgkey_ to use for last step to get ekpfs
      PKG::crypto.aesCbcCfb128Decrypt(ivKey, imgkeydata, imgKey);
      // ekpfs key to get data and tweak keys.
      PKG::crypto.RSA2048Decrypt(ekpfsKey, imgKey, false);
    } else if (entry.id == 0x80) {
      // GENERAL_DIGESTS, seek;
      // file.Seek(entry.offset, fsSeekSet);
    }

    Common::FS::IOFile out(extract_path / "sce_sys" / name,
                           Common::FS::FileAccessMode::Write);
    if (!file.Seek(entry.offset)) {
      failreason = "Failed to seek to PKG entry offset";
      return false;
    }

    std::vector<u8> data;
    data.resize(Common::AlignUp(static_cast<u32>(entry.size), 16));
    file.ReadRaw<u8>(data.data(), entry.size);
    out.WriteRaw<u8>(data.data(), entry.size);
    out.Close();

    // Decrypt Np stuff and overwrite.
    if (entry.id == 0x400 || entry.id == 0x401 || entry.id == 0x402 ||
        entry.id == 0x403) { // somehow 0x401 is not decrypting
      decNp.resize(Common::AlignUp(static_cast<u32>(entry.size), 16));
      if (!file.Seek(entry.offset)) {
        failreason = "Failed to seek to PKG entry offset";
        return false;
      }

      std::array<u8, 64> concatenated_ivkey_dk3_;
      std::memcpy(concatenated_ivkey_dk3_.data(), &entry, sizeof(entry));
      std::memcpy(concatenated_ivkey_dk3_.data() + sizeof(entry), dk3_.data(),
                  sizeof(dk3_));
      PKG::crypto.ivKeyHASH256(concatenated_ivkey_dk3_, ivKey);
      PKG::crypto.aesCbcCfb128DecryptEntry(
          std::span<const CryptoPP::byte, 32>(
              reinterpret_cast<const CryptoPP::byte *>(ivKey.data()), 32),
          std::span<CryptoPP::byte>(
              reinterpret_cast<CryptoPP::byte *>(data.data()), data.size()),
          std::span<CryptoPP::byte>(
              reinterpret_cast<CryptoPP::byte *>(decNp.data()), decNp.size()));
      Common::FS::IOFile out(extract_path / "sce_sys" / name,
                             Common::FS::FileAccessMode::Write);
      out.WriteRaw<u8>(decNp.data(), entry.size);
      out.Close();
    }

    file.Seek(currentPos);
  }

  // Read the seed
  std::array<u8, 16> seed;
  if (!file.Seek(pkgheader.pfs_image_offset + 0x370)) {
    failreason = "Failed to seek to PFS image offset";
    return false;
  }
  file.Read(seed);

  // Get data and tweak keys.
  PKG::crypto.PfsGenCryptoKey(ekpfsKey, seed, dataKey, tweakKey);
  const u32 length = pkgheader.pfs_cache_size * 0x2; // Seems to be ok.

  int num_blocks = 0;
  std::vector<u8> pfsc(length);
  if (length != 0) {
    // For large files, use streaming to avoid massive memory allocation
    const size_t CHUNK_SIZE = 0x1000000; // 16MB chunks
    std::vector<u8> pfs_encrypted(std::min(static_cast<size_t>(length), CHUNK_SIZE));
    std::vector<u8> pfs_decrypted(std::min(static_cast<size_t>(length), CHUNK_SIZE));
    
    file.Seek(pkgheader.pfs_image_offset);
    
    // Read and decrypt in chunks
    size_t total_read = 0;
    size_t pfsc_write_pos = 0;
    
    while (total_read < length) {
      size_t to_read = std::min(CHUNK_SIZE, static_cast<size_t>(length - total_read));
      pfs_encrypted.resize(to_read);
      pfs_decrypted.resize(to_read);
      
      file.ReadRaw<u8>(pfs_encrypted.data(), to_read);
      
      // Decrypt chunk
      PKG::crypto.decryptPFS(dataKey, tweakKey, pfs_encrypted, pfs_decrypted, total_read / 0x1000);
      
      // Copy to pfsc buffer
      size_t to_copy = std::min(to_read, static_cast<size_t>(length - pfsc_write_pos));
      std::memcpy(pfsc.data() + pfsc_write_pos, pfs_decrypted.data(), to_copy);
      
      total_read += to_read;
      pfsc_write_pos += to_copy;
    }
    
    file.Close();

    // Retrieve PFSC from decrypted pfs_image.
    pfsc_offset = GetPFSCOffset(pfsc.data(), pfsc.size());

    PFSCHdr pfsChdr;
    std::memcpy(&pfsChdr, pfsc.data(), sizeof(pfsChdr));

    num_blocks = (int)(pfsChdr.data_length / pfsChdr.block_sz2);
    sectorMap.resize(num_blocks +
                     1); // 8 bytes, need extra 1 to get the last offset.

    for (int i = 0; i < num_blocks + 1; i++) {
      std::memcpy(&sectorMap[i], pfsc.data() + pfsChdr.block_offsets + i * 8,
                  8);
    }
  }

  u32 ent_size = 0;
  u32 ndinode = 0;
  int ndinode_counter = 0;
  bool dinode_reached = false;
  bool uroot_reached = false;
  std::vector<char> compressedData;
  std::vector<char> decompressedData(0x10000);

  // Get iNdoes and Dirents.
  // LOG_DEBUG(Common, "Inizio parsing blocchi PFS, num_blocks: {}", num_blocks);
  for (int i = 0; i < num_blocks; i++) {
    const u64 sectorOffset = sectorMap[i];
    const u64 sectorSize = sectorMap[i + 1] - sectorOffset;

    compressedData.resize(sectorSize);
    std::memcpy(compressedData.data(), pfsc.data() + sectorOffset, sectorSize);

    if (sectorSize == 0x10000) // Uncompressed data
      std::memcpy(decompressedData.data(), compressedData.data(), 0x10000);
    else if (sectorSize < 0x10000) // Compressed data
      DecompressPFSC(compressedData.data(), compressedData.size(),
                     decompressedData.data(), decompressedData.size());

    if (i == 0) {
      std::memcpy(&ndinode, decompressedData.data() + 0x30,
                  4); // number of folders and files
      // LOG_DEBUG(Common, "ndinode (num folder/file): {}", ndinode);
    }

    int occupied_blocks =
        (ndinode * 0xA8) /
        0x10000; // how many blocks(0x10000) are taken by iNodes.
    if (((ndinode * 0xA8) % 0x10000) != 0)
      occupied_blocks += 1;

    if (i >= 1 && i <= occupied_blocks) { // Get all iNodes, gives type, file
                                          // size and location.
      for (int p = 0; p < 0x10000; p += 0xA8) {
        Inode node;
        std::memcpy(&node, &decompressedData[p], sizeof(node));
        if (node.Mode == 0) {
          break;
        }
        iNodeBuf.push_back(node);
        // LOG_DEBUG(Common, "iNode aggiunto: Mode={}", node.Mode);
      }
    }

    // let's deal with the root/uroot entries here.
    // Sometimes it's more than 2 entries (Tomb Raider Remastered)
    const std::string_view flat_path_table(&decompressedData[0x10], 15);
    if (flat_path_table == "flat_path_table") {
      uroot_reached = true;
      // LOG_DEBUG(Common, "flat_path_table trovato, uroot_reached=true");
    }

    if (uroot_reached) {
      for (int i = 0; i < 0x10000; i += ent_size) {
        Dirent dirent;
        std::memcpy(&dirent, &decompressedData[i], sizeof(dirent));
        ent_size = dirent.entsize;
        // LOG_DEBUG(Common, "Dirent uroot: ino={}, entsize={}", dirent.ino,
        //           dirent.entsize);
        if (dirent.ino != 0) {
          ndinode_counter++;
        } else {
          // Imposta la cartella base per l'estrazione.
          // NON forzare parent_path/TitleID: rispetta la directory di output
          // scelta dall'utente. Tutto verrà estratto sotto extract_path.
          extractPaths[ndinode_counter] = extract_path;
          uroot_reached = false;
          break;
        }
      }
    }

    const char dot = decompressedData[0x10];
    const std::string_view dotdot(&decompressedData[0x28], 2);
    if (dot == '.' && dotdot == "..") {
      dinode_reached = true;
      // LOG_DEBUG(Common, "dinode_reached=true");
    }

    // Get folder and file names.
    bool end_reached = false;
    if (dinode_reached) {
      for (int j = 0; j < 0x10000;
           j += ent_size) { // Skip the first parent and child.
        Dirent dirent;
        std::memcpy(&dirent, &decompressedData[j], sizeof(dirent));

        // Stop here and continue the main loop
        if (dirent.ino == 0) {
          // LOG_DEBUG(Common, "Dirent.ino==0, break ciclo");
          break;
        }

        ent_size = dirent.entsize;
        auto &table = fsTable.emplace_back();
        table.name = std::string(dirent.name, dirent.namelen);
        table.inode = dirent.ino;
        table.type = dirent.type;
        // LOG_DEBUG(Common, "fsTable aggiunta: nome={}, inode={}, type={}",
  //           table.name, table.inode, table.type);

        if (table.type == PFS_CURRENT_DIR) {
          current_dir = extractPaths[table.inode];
        }
        extractPaths[table.inode] =
            current_dir / std::filesystem::path(table.name);
        if (table.type == PFS_FILE || table.type == PFS_DIR) {
          if (table.type == PFS_DIR) {
            // no disk writes in Scan
          }
          ndinode_counter++;
          if ((ndinode_counter + 1) == ndinode)
            end_reached = true;
        }
      }
      if (end_reached) {
        // LOG_DEBUG(Common, "end_reached=true, break ciclo blocchi");
        break;
      }
    }
  }
  // LOG_DEBUG(Common, "Finished parsing PFS blocks");
  return true;
}

void PKG::ExtractAllFilesWithProgress() {
  const size_t num_files = fsTable.size();
  const size_t max_threads =
      std::min<size_t>(8, std::thread::hardware_concurrency());
  std::atomic<size_t> files_done{0};
  std::mutex print_mutex;

  auto print_progress = [&](size_t done) {
    float percent = (float)done / (float)num_files * 100.0f;
    int barWidth = 40;
    int pos = (int)(barWidth * percent / 100.0f);
    std::ostringstream oss;
    oss << "[";
    for (int i = 0; i < barWidth; ++i)
      oss << (i < pos ? "=" : (i == pos ? ">" : " "));
    oss << "] ";
    oss << std::setw(3) << int(percent) << "% ";
    oss << done << "/" << num_files << " extracted";
    std::lock_guard<std::mutex> lock(print_mutex);
    std::cout << "\r" << std::string(80, ' ') << "\r" << oss.str()
              << std::flush;
  };

  auto extract_worker = [&](size_t start, size_t end) {
    for (size_t i = start; i < end; ++i) {
      ExtractFiles(i);
      size_t done = ++files_done;
      if (done % 1 == 0 || done == num_files)
        print_progress(done);
    }
  };

  std::vector<std::thread> threads;
  size_t batch = (num_files + max_threads - 1) / max_threads;
  for (size_t t = 0; t < max_threads; ++t) {
    size_t start = t * batch;
    size_t end = std::min(num_files, start + batch);
    if (start < end)
      threads.emplace_back(extract_worker, start, end);
  }
  for (auto &th : threads)
    th.join();
  print_progress(num_files);
  std::cout << std::endl;
}

void PKG::ExtractFiles(const int index) {
  int inode_number = fsTable[index].inode;
  int inode_type = fsTable[index].type;
  std::string inode_name = fsTable[index].name;

  // Removed verbose logging for cleaner progress bar
  // LOG_DEBUG(Common, "ExtractFiles: index={}, inode={}, type={}, name={}",
  // index, inode_number, inode_type, inode_name);

  if (inode_type == PFS_FILE) {
    // Create destination directory only for the file about to be written
    try {
      std::filesystem::create_directories(
          extractPaths[inode_number].parent_path());
    } catch (const std::exception &e) {
      // LOG_ERROR(Common, "Directory creation failed: {}", e.what());
    }
    int sector_loc = iNodeBuf[inode_number].loc;
    int nblocks = iNodeBuf[inode_number].Blocks;
    int bsize = iNodeBuf[inode_number].Size;

    Common::FS::IOFile inflated(extractPaths[inode_number], Common::FS::FileAccessMode::Write);
    if (!inflated.IsOpen()) {
      // LOG_ERROR(Common, "Failed to open output file");
      return;
    }

    Common::FS::IOFile pkgFile(pkgpath, Common::FS::FileAccessMode::Read);
    if (!pkgFile.IsOpen()) {
      // LOG_ERROR(Common, "Failed to open PKG file");
      return;
    }

    int size_decompressed = 0;
    std::vector<char> compressedData;
    std::vector<char> decompressedData(0x10000);

    u64 pfsc_buf_size = 0x11000; // extra 0x1000
    std::vector<u8> pfsc(pfsc_buf_size);
    std::vector<u8> pfs_decrypted(pfsc_buf_size);

    for (int j = 0; j < nblocks; j++) {
      u64 sectorOffset =
          sectorMap[sector_loc +
                    j]; // offset into PFSC_image and not pfs_image.
      u64 sectorSize = sectorMap[sector_loc + j + 1] -
                       sectorOffset; // indicates if data is compressed or not.
      u64 fileOffset =
          (pkgheader.pfs_image_offset + pfsc_offset + sectorOffset);
      u64 currentSector1 = (pfsc_offset + sectorOffset) /
                           0x1000; // block size is 0x1000 for xts decryption.

      int sectorOffsetMask = (sectorOffset + pfsc_offset) & 0xFFFFF000;
      int previousData = (sectorOffset + pfsc_offset) - sectorOffsetMask;

      pkgFile.Seek(fileOffset - previousData);
      pkgFile.Read(pfsc);

      // Thread-safe crypto access
      {
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        crypto.decryptPFS(dataKey, tweakKey, pfsc, pfs_decrypted,
                          currentSector1);
      }

      compressedData.resize(sectorSize);
      std::memcpy(compressedData.data(), pfs_decrypted.data() + previousData,
                  sectorSize);

      if (sectorSize == 0x10000) // Uncompressed data
        std::memcpy(decompressedData.data(), compressedData.data(), 0x10000);
      else if (sectorSize < 0x10000) // Compressed data
        DecompressPFSC(compressedData.data(), compressedData.size(),
                       decompressedData.data(), decompressedData.size());

      size_decompressed += 0x10000;

      if (j < nblocks - 1) {
        inflated.WriteRaw<u8>(
            reinterpret_cast<const u8 *>(decompressedData.data()),
            decompressedData.size());
      } else {
        // This is to remove the zeros at the end of the file.
        const u32 write_size =
            decompressedData.size() - (size_decompressed - bsize);
        inflated.WriteRaw<u8>(
            reinterpret_cast<const u8 *>(decompressedData.data()), write_size);
      }
    }
    pkgFile.Close();
    inflated.Close();
  } else if (inode_name.empty()) {
    // Extract also nameless entries (unknown)
    std::ostringstream oss;
    oss << "entry_0x" << std::hex << inode_number << ".bin";
    std::filesystem::path outpath = extract_path / oss.str();
    // Create destination directory only for the file about to be written
    try {
      std::filesystem::create_directories(outpath.parent_path());
    } catch (const std::exception &e) {
      // LOG_ERROR(Common, "Directory creation failed: {}", e.what());
    }
    // Search for the corresponding PKGEntry
    for (const auto &entry : pkgEntries) {
      if (entry.id == static_cast<u32>(inode_number)) {
        Common::FS::IOFile pkgFile(pkgpath, Common::FS::FileAccessMode::Read);
        if (!pkgFile.IsOpen()) {
          // LOG_ERROR(Common, "Failed to open PKG file");
          break;
        }
        pkgFile.Seek(entry.offset);
        std::vector<u8> data(entry.size);
        pkgFile.ReadRaw<u8>(data.data(), entry.size);
        Common::FS::IOFile out(outpath, Common::FS::FileAccessMode::Write);
        if (!out.IsOpen()) {
          // LOG_ERROR(Common, "Failed to open output file");
          break;
        }
        out.WriteRaw<u8>(data.data(), data.size());
        out.Close();
        pkgFile.Close();
        break;
      }
    }
  }
}

std::vector<u8> PKG::GetFileBuffer(const std::string &filename) {
  int index = -1;
  for(size_t i=0; i<fsTable.size(); ++i) {
      if(fsTable[i].name == filename) {
          index = i;
          break;
      }
  }
  if (index == -1) return {};

  int inode_number = fsTable[index].inode;
  int inode_type = fsTable[index].type;
  
  if (inode_type != PFS_FILE) return {}; 

  int sector_loc = iNodeBuf[inode_number].loc;
  int nblocks = iNodeBuf[inode_number].Blocks;
  int bsize = iNodeBuf[inode_number].Size;

  std::vector<u8> resultBuffer;
  resultBuffer.reserve(bsize);

  Common::FS::IOFile pkgFile(pkgpath, Common::FS::FileAccessMode::Read);
  if (!pkgFile.IsOpen()) {
    // LOG_ERROR(Common, "Failed to open PKG file");
    return {};
  }
  
  int size_decompressed = 0;
  std::vector<char> compressedData;
  std::vector<char> decompressedData(0x10000);

  u64 pfsc_buf_size = 0x11000;
  std::vector<u8> pfsc(pfsc_buf_size);
  std::vector<u8> pfs_decrypted(pfsc_buf_size);

  for (int j = 0; j < nblocks; j++) {
      u64 sectorOffset = sectorMap[sector_loc + j];
      u64 sectorSize = sectorMap[sector_loc + j + 1] - sectorOffset;
      u64 fileOffset = (pkgheader.pfs_image_offset + pfsc_offset + sectorOffset);
      u64 currentSector1 = (pfsc_offset + sectorOffset) / 0x1000;

      int sectorOffsetMask = (sectorOffset + pfsc_offset) & 0xFFFFF000;
      int previousData = (sectorOffset + pfsc_offset) - sectorOffsetMask;

      pkgFile.Seek(fileOffset - previousData);
      pkgFile.Read(pfsc);

      {
        std::lock_guard<std::mutex> lock(crypto_mutex_);
        crypto.decryptPFS(dataKey, tweakKey, pfsc, pfs_decrypted, currentSector1);
      }
      
      compressedData.resize(sectorSize);
      std::memcpy(compressedData.data(), pfs_decrypted.data() + previousData, sectorSize);

      if (sectorSize == 0x10000)
         std::memcpy(decompressedData.data(), compressedData.data(), 0x10000);
      else if (sectorSize < 0x10000)
         DecompressPFSC(compressedData.data(), compressedData.size(), decompressedData.data(), decompressedData.size());
      
      size_decompressed += 0x10000;

      u32 write_size = 0x10000;
      if (j == nblocks - 1) {
         write_size = decompressedData.size() - (size_decompressed - bsize);
      }
      
      resultBuffer.insert(resultBuffer.end(), 
                          reinterpret_cast<u8*>(decompressedData.data()), 
                          reinterpret_cast<u8*>(decompressedData.data()) + write_size);
  }
  
  return resultBuffer;
}

std::vector<std::string> PKG::GetFileList() const {
  std::vector<std::string> files;
  for (const auto &entry : fsTable) {
    if (entry.type == PFS_FILE) {
      files.push_back(entry.name);
    }
  }
  return files;
}

std::vector<std::tuple<std::string, u32, u32>> PKG::GetAllEntries() const {
  // LOG_DEBUG(Common, "Calling GetAllEntries, fsTable size: {}", fsTable.size());
  std::vector<std::tuple<std::string, u32, u32>> entries;
  for (const auto &entry : fsTable) {
    // LOG_DEBUG(Common, "fsTable entry: name={}, inode={}, type={}", entry.name,
  //           entry.inode, entry.type);
    entries.emplace_back(entry.name, entry.inode, entry.type);
  }
  return entries;
}

bool PKG::Scan(const std::filesystem::path &filepath, std::string &failreason,
               std::filesystem::path extract_root) {
  // LOG_DEBUG(Common, "Inizio PKG::Scan su {}", filepath.string());
  extract_path = extract_root; // base relativa per ricostruire i path
  pkgpath = filepath;

  Common::FS::IOFile file(filepath, Common::FS::FileAccessMode::Read);
  if (!file.IsOpen()) {
    failreason = "Unable to open file";
    // LOG_ERROR(Common, "{}: {}", failreason, filepath.string());
    return false;
  }
  pkgSize = file.GetSize();
  file.ReadRaw<u8>(&pkgheader, sizeof(PKGHeader));

  if (pkgheader.magic != 0x7F434E54) {
    failreason = "Invalid PKG Magic";
    // LOG_ERROR(Common, "{}", failreason);
    return false;
  }

  // Readable flags
  pkgFlags.clear();
  for (const auto &flag : flagNames) {
    if (isFlagSet(pkgheader.pkg_content_flags, flag.first)) {
      if (!pkgFlags.empty())
        pkgFlags += ", ";
      pkgFlags += flag.second;
    }
  }

  // TitleID
  file.Seek(0x47);
  file.Read(pkgTitleID);

  // Read table entries to find param.sfo etc. (no disk writes)
  u32 offset = pkgheader.pkg_table_entry_offset;
  u32 n_files = pkgheader.pkg_table_entry_count;
  if (!file.Seek(offset)) {
    failreason = "Seek failed to table entry";
    return false;
  }
  pkgEntries.clear();
  for (u32 i = 0; i < n_files; i++) {
    PKGEntry entry{};
    file.Read(entry.id);
    file.Read(entry.filename_offset);
    file.Read(entry.flags1);
    file.Read(entry.flags2);
    file.Read(entry.offset);
    file.Read(entry.size);
    file.Seek(8, Common::FS::SeekOrigin::CurrentPosition);
    pkgEntries.push_back(entry);
  }

  // Seed e decrypt PFS come in Extract(), ma senza scritture file.
  if (pkgheader.pkg_size > pkgSize) {
    failreason = "Inconsistent PKG size";
    return false;
  }
  if ((pkgheader.pkg_content_size + pkgheader.pkg_content_offset) >
      pkgheader.pkg_size) {
    failreason = "Content size exceeds pkg size";
    return false;
  }

  // Avanza fino a popolare sectorMap come in Extract()
  // Leggi strutture preliminari necessarie a derivare le chiavi
  // e popolare ivKey, imgKey, ekpfsKey.
  // Riusa lo stesso codice di Extract per le voci necessarie.

  // Riposizionarsi sulla tabella entries
  file.Seek(pkgheader.pkg_table_entry_offset);
  std::array<u8, 64> concatenated_ivkey_dk3;
  std::array<u8, 32> seed_digest;
  std::array<std::array<u8, 32>, 7> digest1;
  std::array<std::array<u8, 256>, 7> key1;
  std::array<u8, 256> imgkeydata;

  for (u32 i = 0; i < n_files; i++) {
    PKGEntry entry{};
    file.Read(entry.id);
    file.Read(entry.filename_offset);
    file.Read(entry.flags1);
    file.Read(entry.flags2);
    file.Read(entry.offset);
    file.Read(entry.size);
    file.Seek(8, Common::FS::SeekOrigin::CurrentPosition);

    auto currentPos = file.Tell();
    if (entry.id == 0x10) { // ENTRY_KEYS
      file.Seek(entry.offset);
      file.Read(seed_digest);
      for (int j = 0; j < 7; j++)
        file.Read(digest1[j]);
      for (int j = 0; j < 7; j++)
        file.Read(key1[j]);
      PKG::crypto.RSA2048Decrypt(dk3_, key1[3], true); // decrypt DK3
    } else if (entry.id == 0x20) {                     // IMAGE_KEY (IV_KEY)
      file.Seek(entry.offset);
      file.Read(imgkeydata);
      std::memcpy(concatenated_ivkey_dk3.data(), &entry, sizeof(entry));
      std::memcpy(concatenated_ivkey_dk3.data() + sizeof(entry), dk3_.data(),
                  sizeof(dk3_));
      PKG::crypto.ivKeyHASH256(concatenated_ivkey_dk3, ivKey);
      PKG::crypto.aesCbcCfb128Decrypt(ivKey, imgkeydata, imgKey);
      PKG::crypto.RSA2048Decrypt(ekpfsKey, imgKey, false);
    }
    file.Seek(currentPos);
  }

  // Seed
  std::array<u8, 16> seed;
  if (!file.Seek(pkgheader.pfs_image_offset + 0x370)) {
    failreason = "Seek failed to PFS seed";
    return false;
  }
  file.Read(seed);
  PKG::crypto.PfsGenCryptoKey(ekpfsKey, seed, dataKey, tweakKey);

  const u32 length = pkgheader.pfs_cache_size * 0x2;
  int num_blocks = 0;
  std::vector<u8> pfsc(length);
  if (length != 0) {
    std::vector<u8> pfs_encrypted(length);
    file.Seek(pkgheader.pfs_image_offset);
    file.Read(pfs_encrypted);
    file.Close();

    std::vector<u8> pfs_decrypted(length);
    PKG::crypto.decryptPFS(dataKey, tweakKey, pfs_encrypted, pfs_decrypted, 0);

    pfsc_offset = GetPFSCOffset(pfs_decrypted.data(), pfs_decrypted.size());
    std::memcpy(pfsc.data(), pfs_decrypted.data() + pfsc_offset,
                length - pfsc_offset);

    PFSCHdr pfsChdr;
    std::memcpy(&pfsChdr, pfsc.data(), sizeof(pfsChdr));
    num_blocks = (int)(pfsChdr.data_length / pfsChdr.block_sz2);
    sectorMap.resize(num_blocks + 1);
    for (int i = 0; i < num_blocks + 1; i++) {
      std::memcpy(&sectorMap[i], pfsc.data() + pfsChdr.block_offsets + i * 8,
                  8);
    }
  }

  // Parse iNodes e Dirents per popolare fsTable/extractPaths
  iNodeBuf.clear();
  fsTable.clear();
  extractPaths.clear();
  u32 ent_size = 0;
  u32 ndinode = 0;
  int ndinode_counter = 0;
  bool dinode_reached = false;
  bool uroot_reached = false;
  std::vector<char> compressedData;
  std::vector<char> decompressedData(0x10000);

  for (int i = 0; i < (int)sectorMap.size() - 1; i++) {
    const u64 sectorOffset = sectorMap[i];
    const u64 sectorSize = sectorMap[i + 1] - sectorOffset;
    compressedData.resize(sectorSize);
    std::memcpy(compressedData.data(), pfsc.data() + sectorOffset, sectorSize);
    if (sectorSize == 0x10000)
      std::memcpy(decompressedData.data(), compressedData.data(), 0x10000);
    else if (sectorSize < 0x10000)
      DecompressPFSC(compressedData.data(), compressedData.size(),
                     decompressedData.data(), decompressedData.size());

    if (i == 0) {
      std::memcpy(&ndinode, decompressedData.data() + 0x30, 4);
    }

    int occupied_blocks = (ndinode * 0xA8) / 0x10000;
    if (((ndinode * 0xA8) % 0x10000) != 0)
      occupied_blocks += 1;

    if (i >= 1 && i <= occupied_blocks) {
      for (int p = 0; p < 0x10000; p += 0xA8) {
        Inode node;
        std::memcpy(&node, &decompressedData[p], sizeof(node));
        if (node.Mode == 0)
          break;
        iNodeBuf.push_back(node);
      }
    }

    const std::string_view flat_path_table(&decompressedData[0x10], 15);
    if (flat_path_table == "flat_path_table") {
      uroot_reached = true;
    }

    if (uroot_reached) {
      for (int k = 0; k < 0x10000; k += ent_size) {
        Dirent dirent;
        std::memcpy(&dirent, &decompressedData[k], sizeof(dirent));
        ent_size = dirent.entsize;
        if (dirent.ino != 0) {
          ndinode_counter++;
        } else {
          extractPaths[ndinode_counter] = extract_path;
          uroot_reached = false;
          break;
        }
      }
    }

    const char dot = decompressedData[0x10];
    const std::string_view dotdot(&decompressedData[0x28], 2);
    if (dot == '.' && dotdot == "..") {
      dinode_reached = true;
    }

    bool end_reached = false;
    if (dinode_reached) {
      for (int j = 0; j < 0x10000; j += ent_size) {
        Dirent dirent;
        std::memcpy(&dirent, &decompressedData[j], sizeof(dirent));
        if (dirent.ino == 0)
          break;
        ent_size = dirent.entsize;
        auto &table = fsTable.emplace_back();
        table.name = std::string(dirent.name, dirent.namelen);
        table.inode = dirent.ino;
        table.type = dirent.type;
        if (table.type == PFS_CURRENT_DIR) {
          current_dir = extractPaths[table.inode];
        }
        extractPaths[table.inode] =
            current_dir / std::filesystem::path(table.name);
        if (table.type == PFS_FILE || table.type == PFS_DIR) {
          if (table.type == PFS_DIR) {
            // niente scritture su disco in Scan
          }
          ndinode_counter++;
          if ((ndinode_counter + 1) == ndinode)
            end_reached = true;
        }
      }
      if (end_reached) {
        // LOG_DEBUG(Common, "end_reached=true, break ciclo blocchi");
        break;
      }
    }
  }
  // LOG_DEBUG(Common, "Fine PKG::Scan, entries: {}", fsTable.size());
  return true;
}

std::vector<PKG::EntryInfo> PKG::GetEntriesInfo() const {
  std::vector<EntryInfo> out;
  out.reserve(fsTable.size());
  for (const auto &e : fsTable) {
    EntryInfo info{};
    info.name = e.name;
    info.inode = e.inode;
    info.type = e.type;
    auto it = extractPaths.find(e.inode);
    info.path =
        (it != extractPaths.end()) ? it->second.string() : std::string();
    if (e.type == PFS_FILE && e.inode < iNodeBuf.size()) {
      info.size = static_cast<u64>(iNodeBuf[e.inode].Size);
      info.blocks = static_cast<u32>(iNodeBuf[e.inode].Blocks);
      info.loc = static_cast<u32>(iNodeBuf[e.inode].loc);
    } else {
      info.size = 0;
      info.blocks = 0;
      info.loc = 0;
    }
    out.push_back(std::move(info));
  }
  return out;
}
