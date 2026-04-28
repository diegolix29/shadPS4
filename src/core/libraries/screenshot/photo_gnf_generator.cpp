// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// v155: Generate photo GNFs matching film_dummy.gnf format EXACTLY
//   - 512x512, BC1/DXT1 (data_format=41), Thin1DThin (tiling_index=13)
//   - File size = 262400 bytes (256 header + 262144 pixel data)
//   - NumTextures=2, second T# all zeros (matching original)

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>

#include "common/logging/log.h"
#include "core/libraries/screenshot/photo_gnf_generator.h"
#include "common/stb.h"

namespace Libraries::ScreenShot {

namespace fs = std::filesystem;

PhotoGnfManager& PhotoGnfManager::Instance() {
    static PhotoGnfManager instance;
    return instance;
}

fs::path PhotoGnfManager::GetGnfDir() const {
    return gnf_dir_;
}

// ---- BC1 (DXT1) Encoder ----

static u16 PackRGB565(u8 r, u8 g, u8 b) {
    return static_cast<u16>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void UnpackRGB565(u16 c, u8& r, u8& g, u8& b) {
    r = ((c >> 11) & 0x1F) << 3;
    g = ((c >> 5) & 0x3F) << 2;
    b = (c & 0x1F) << 3;
}

static void EncodeBC1Block(const u8* pixels, u8* out) {
    u8 minR = 255, minG = 255, minB = 255;
    u8 maxR = 0, maxG = 0, maxB = 0;
    for (int i = 0; i < 16; i++) {
        u8 r = pixels[i*4], g = pixels[i*4+1], b = pixels[i*4+2];
        if (r < minR) minR = r; if (g < minG) minG = g; if (b < minB) minB = b;
        if (r > maxR) maxR = r; if (g > maxG) maxG = g; if (b > maxB) maxB = b;
    }
    u16 c0 = PackRGB565(maxR, maxG, maxB);
    u16 c1 = PackRGB565(minR, minG, minB);
    if (c0 < c1) {
        std::swap(c0, c1);
        std::swap(minR, maxR); std::swap(minG, maxG); std::swap(minB, maxB);
    }
    if (c0 == c1 && c0 < 0xFFFF) c0++;

    u8 pal[4][3];
    UnpackRGB565(c0, pal[0][0], pal[0][1], pal[0][2]);
    UnpackRGB565(c1, pal[1][0], pal[1][1], pal[1][2]);
    pal[2][0] = (2*pal[0][0] + pal[1][0] + 1) / 3;
    pal[2][1] = (2*pal[0][1] + pal[1][1] + 1) / 3;
    pal[2][2] = (2*pal[0][2] + pal[1][2] + 1) / 3;
    pal[3][0] = (pal[0][0] + 2*pal[1][0] + 1) / 3;
    pal[3][1] = (pal[0][1] + 2*pal[1][1] + 1) / 3;
    pal[3][2] = (pal[0][2] + 2*pal[1][2] + 1) / 3;

    u32 indices = 0;
    for (int i = 0; i < 16; i++) {
        u8 r = pixels[i*4], g = pixels[i*4+1], b = pixels[i*4+2];
        int best = 0, bestd = INT32_MAX;
        for (int j = 0; j < 4; j++) {
            int dr = r-pal[j][0], dg = g-pal[j][1], db = b-pal[j][2];
            int d = dr*dr + dg*dg + db*db;
            if (d < bestd) { bestd = d; best = j; }
        }
        indices |= (u32(best) << (i * 2));
    }
    out[0] = c0 & 0xFF; out[1] = (c0 >> 8);
    out[2] = c1 & 0xFF; out[3] = (c1 >> 8);
    out[4] = indices & 0xFF; out[5] = (indices >> 8);
    out[6] = (indices >> 16); out[7] = (indices >> 24);
}

static std::vector<u8> EncodeBC1Linear(const u8* rgba, u32 w, u32 h) {
    u32 bw = w / 4, bh = h / 4;
    std::vector<u8> bc1(bw * bh * 8);
    for (u32 by = 0; by < bh; by++) {
        for (u32 bx = 0; bx < bw; bx++) {
            u8 block[64];
            for (int py = 0; py < 4; py++)
                for (int px = 0; px < 4; px++) {
                    u32 sx = bx*4+px, sy = by*4+py;
                    std::memcpy(block + (py*4+px)*4, rgba + (sy*w+sx)*4, 4);
                }
            EncodeBC1Block(block, bc1.data() + (by*bw+bx)*8);
        }
    }
    return bc1;
}

// ---- Micro-Tiler: Thin1DThin for BC1 ----
// Micro-tile: 8 elements wide x 8 elements tall
// Element = one BC1 block = 8 bytes
// Micro-tile = 512 bytes

static std::vector<u8> MicroTileThin1D_BC1(const u8* linear, u32 bw, u32 bh) {
    constexpr u32 MT = 8, ES = 8;
    u32 pw = (bw + MT-1) & ~(MT-1);
    u32 ph = (bh + MT-1) & ~(MT-1);
    u32 mc = pw/MT, mr = ph/MT;
    std::vector<u8> tiled(pw * ph * ES, 0);
    for (u32 my = 0; my < mr; my++)
        for (u32 mx = 0; mx < mc; mx++) {
            u32 mi = my*mc + mx;
            u32 mo = mi * MT * MT * ES;
            for (u32 ey = 0; ey < MT; ey++)
                for (u32 ex = 0; ex < MT; ex++) {
                    u32 sx = mx*MT+ex, sy = my*MT+ey;
                    if (sx < bw && sy < bh)
                        std::memcpy(tiled.data() + mo + (ey*MT+ex)*ES,
                                    linear + (sy*bw+sx)*ES, ES);
                }
        }
    return tiled;
}

// ---- GNF header matching film_dummy.gnf ----

static void BuildMatchingGnfHeader(u8* header) {
    std::memset(header, 0, 256);
    // Magic "GNF "
    header[0]=0x47; header[1]=0x4E; header[2]=0x46; header[3]=0x20;
    // Content size 0xF8
    header[4]=0xF8; header[5]=0; header[6]=0; header[7]=0;
    // Version=2, NumTex-1=1
    header[8]=0x02; header[9]=0x01;
    // Alignment=8
    header[10]=0x08; header[11]=0;
    // Stream size (same as original)
    header[12]=0x00; header[13]=0x01; header[14]=0x04; header[15]=0x00;
    // T#0 — exact bytes from film_dummy.gnf
    const u64 qw0 = 0x0290000800000000ULL;
    const u64 qw1 = 0x94D00FAC707FC1FFULL;
    const u64 qw2 = 0x00000000003FE000ULL;
    const u64 qw3 = 0x0004000000000000ULL;
    std::memcpy(header+0x10, &qw0, 8);
    std::memcpy(header+0x18, &qw1, 8);
    std::memcpy(header+0x20, &qw2, 8);
    std::memcpy(header+0x28, &qw3, 8);
    // T#1 all zeros (already zeroed)
}

std::vector<u8> PhotoGnfManager::BuildGnf(const u8* rgba_pixels) {
    constexpr u32 HDR = 256, PIX = 262144, TOTAL = HDR + PIX;
    std::vector<u8> gnf(TOTAL, 0);
    BuildMatchingGnfHeader(gnf.data());

    auto bc1 = EncodeBC1Linear(rgba_pixels, PHOTO_WIDTH, PHOTO_HEIGHT);
    auto tiled = MicroTileThin1D_BC1(bc1.data(), PHOTO_WIDTH/4, PHOTO_HEIGHT/4);
    u32 sz = std::min(u32(tiled.size()), PIX);
    std::memcpy(gnf.data()+HDR, tiled.data(), sz);
    // Fill remaining space (mipmap proxy)
    if (sz < PIX) {
        u32 rem = PIX - sz;
        std::memcpy(gnf.data()+HDR+sz, tiled.data(), std::min(sz, rem));
    }
    return gnf;
}

std::vector<u8> PhotoGnfManager::BuildSolidGnf(u8 r, u8 g, u8 b, u8 a) {
    std::vector<u8> rgba(PHOTO_WIDTH * PHOTO_HEIGHT * 4);
    for (u32 i = 0; i < PHOTO_WIDTH * PHOTO_HEIGHT; i++) {
        rgba[i*4]=r; rgba[i*4+1]=g; rgba[i*4+2]=b; rgba[i*4+3]=a;
    }
    return BuildGnf(rgba.data());
}

// ---- Resize ----

static void ResizeRGBA(const u8* src, u32 sw, u32 sh,
                       u8* dst, u32 dw, u32 dh) {
    for (u32 dy = 0; dy < dh; dy++) {
        float sy = (dy+0.5f)*sh/dh - 0.5f;
        sy = std::clamp(sy, 0.f, float(sh-1));
        u32 iy0 = u32(sy), iy1 = std::min(iy0+1, sh-1);
        float fy = sy - iy0;
        for (u32 dx = 0; dx < dw; dx++) {
            float sx = (dx+0.5f)*sw/dw - 0.5f;
            sx = std::clamp(sx, 0.f, float(sw-1));
            u32 ix0 = u32(sx), ix1 = std::min(ix0+1, sw-1);
            float fx = sx - ix0;
            for (int c = 0; c < 4; c++) {
                float v = src[(iy0*sw+ix0)*4+c]*(1-fx)*(1-fy)
                        + src[(iy0*sw+ix1)*4+c]*fx*(1-fy)
                        + src[(iy1*sw+ix0)*4+c]*(1-fx)*fy
                        + src[(iy1*sw+ix1)*4+c]*fx*fy;
                dst[(dy*dw+dx)*4+c] = u8(std::clamp(v+.5f, 0.f, 255.f));
            }
        }
    }
}

bool PhotoGnfManager::DecodeAndWriteSlot(int slot_index,
                                          const fs::path& jpeg_path) {
    std::ifstream f(jpeg_path, std::ios::binary|std::ios::ate);
    if (!f.is_open()) return false;
    size_t fsz = f.tellg(); f.seekg(0);
    std::vector<u8> jd(fsz);
    f.read(reinterpret_cast<char*>(jd.data()), fsz);
    f.close();

    int w=0, h=0, ch=0;
    u8* raw = stbi_load_from_memory(jd.data(), int(jd.size()), &w, &h, &ch, 4);
    if (!raw) return false;

    std::vector<u8> resized(PHOTO_WIDTH * PHOTO_HEIGHT * 4);
    if (u32(w)==PHOTO_WIDTH && u32(h)==PHOTO_HEIGHT)
        std::memcpy(resized.data(), raw, resized.size());
    else
        ResizeRGBA(raw, w, h, resized.data(), PHOTO_WIDTH, PHOTO_HEIGHT);
    stbi_image_free(raw);

    auto gnf = BuildGnf(resized.data());
    auto out_path = gnf_dir_ / fmt::format("photo_slot_{}.gnf", slot_index);
    std::ofstream out(out_path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(gnf.data()), gnf.size());
    LOG_INFO(Core, "[PhotoGnf] Slot {}: {}x{} -> 512x512 BC1 GNF ({} bytes)",
             slot_index, w, h, gnf.size());
    return true;
}

void PhotoGnfManager::PrepareSlots(const fs::path& screenshot_dir,
                                    const std::vector<std::string>& content_ids,
                                    u32 page_offset) {
    std::lock_guard lock(mutex_);
    ready_.store(false);
    prepared_count_ = 0;
    std::memset(slot_valid_, 0, sizeof(slot_valid_));
    gnf_dir_ = screenshot_dir / "gnf_cache";
    std::error_code ec;
    fs::create_directories(gnf_dir_, ec);
    int count = std::min(int(content_ids.size())-int(page_offset), MAX_SLOTS);
    if (count <= 0) { ready_.store(true); return; }
    for (int i = 0; i < count; i++) {
        u32 gi = page_offset + i;
        if (gi >= content_ids.size()) break;
        auto jp = screenshot_dir / (content_ids[gi] + ".jpg");
        if (!fs::exists(jp)) continue;
        if (DecodeAndWriteSlot(i, jp)) { slot_valid_[i] = true; prepared_count_++; }
    }
    LOG_INFO(Core, "[PhotoGnf] Prepared {}/{} BC1 photo GNFs", prepared_count_, count);
    ready_.store(true);
}

void PhotoGnfManager::ResetSlotCounter() {
    slot_counter_.store(0);
}

fs::path PhotoGnfManager::ConsumeNextSlotPath() {
    if (!ready_.load()) return {};
    int s = slot_counter_.fetch_add(1);
    if (s < 0 || s >= MAX_SLOTS) return {};
    std::lock_guard lock(mutex_);
    if (!slot_valid_[s]) return {};
    return gnf_dir_ / fmt::format("photo_slot_{}.gnf", s);
}

} // namespace Libraries::ScreenShot
