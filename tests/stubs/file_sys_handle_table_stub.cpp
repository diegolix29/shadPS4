// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// Minimal HandleTable for the kernel handle-table tests: equeue.cpp reserves a guest fd per
// queue (CreateHandle/GetFile) and releases it on delete (DeleteHandle). Mirrors fs.cpp's slot
// reuse without pulling in the filesystem backends the real implementation now constructs.
// Kept apart from file_sys_stub.cpp so the other test targets, which need only the game-file
// helpers from that stub, do not pull the handle table in as well.

#include "core/file_sys/fs.h"

namespace Core::FileSys {

int HandleTable::CreateHandle() {
    std::scoped_lock lock{m_mutex};

    auto* file = new File{};
    file->is_opened = false;

    int existing_files_num = m_files.size();

    for (int index = 0; index < existing_files_num; index++) {
        if (m_files.at(index) == nullptr) {
            m_files[index] = file;
            return index;
        }
    }

    m_files.push_back(file);
    return m_files.size() - 1;
}

void HandleTable::DeleteHandle(int d) {
    std::scoped_lock lock{m_mutex};
    delete m_files.at(d);
    m_files[d] = nullptr;
}

File* HandleTable::GetFile(int d) {
    std::scoped_lock lock{m_mutex};
    if (d < 0 || d >= m_files.size()) {
        return nullptr;
    }
    return m_files.at(d);
}

} // namespace Core::FileSys
