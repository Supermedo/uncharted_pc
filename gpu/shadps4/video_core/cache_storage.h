// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/path_util.h"
#include "common/singleton.h"
#include "common/types.h"

#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace Storage {

enum class BlobType : u32 {
    ShaderMeta,
    ShaderBinary,
    PipelineKey,
    ShaderProfile,
};

class DataBase {
public:
    static DataBase& Instance() {
        return *Common::Singleton<DataBase>::Instance();
    }

    /// bbport: names the GPU (vendor, device) the cache belongs to; call before Open(). Each GPU
    /// gets its own cache, so a PC with two GPUs, or one whose GPU was replaced, does not feed one
    /// card's shaders and driver cache to the other or throw them away on every switch.
    void SetDeviceTag(std::string tag);
    [[nodiscard]] const std::string& DeviceTag() const;
    void Open();
    void Close();
    [[nodiscard]] bool IsOpened() const {
        return opened;
    }
    void FinishPreload();
    /// bbport: removes every cached blob (an incompatible cache is rebuilt, not ignored).
    void Clear();

    bool Save(BlobType type, const std::string& name, std::vector<u8>&& data);
    bool Save(BlobType type, const std::string& name, std::vector<u32>&& data);

    void Load(BlobType type, const std::string& name, std::vector<u8>& data);
    void Load(BlobType type, const std::string& name, std::vector<u32>& data);

    void ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func);

    /// bbport: names (without extension) of the stored blobs of a type, for loading them one by
    /// one through Load() (in parallel for a folder cache; the zip reader is not thread-safe).
    std::vector<std::string> ListBlobNames(BlobType type);
    /// bbport: the cache folder (valid after Open(); empty for an archived cache).
    [[nodiscard]] const std::filesystem::path& Directory() const {
        return directory;
    }

    /// bbport: waits (at most `max_ms`) until the queued writes are on disk, so a game that
    /// ends right after a compile does not lose it.
    void Flush(u32 max_ms);

private:
    std::jthread io_worker{};
    std::filesystem::path cache_path{};
    std::filesystem::path directory{}; ///< cache_path for a folder cache, empty for an archive
    bool opened{};
};

} // namespace Storage
