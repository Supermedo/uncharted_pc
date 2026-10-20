// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "bbport_nan_guard.h"

#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <string>
#include <thread>

namespace {

std::mutex submit_mutex{};
std::atomic<u32> num_requests{};
std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};
std::mutex m_request{};

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};
std::string device_tag{};

std::string DeviceSuffix() {
    return device_tag.empty() ? std::string{} : "-" + device_tag;
}

/// A cache from before the per-GPU names cannot be told from a mixed one (it belongs to whichever
/// GPU wrote last, and the loader only compares the shader profile), and a build that reads it
/// would rebuild it anyway: it is removed, on a thread of its own (it can hold tens of thousands
/// of files).
void RemoveUntaggedCache(const std::filesystem::path& untagged, const std::filesystem::path& tagged) {
    if (device_tag.empty() || untagged == tagged) {
        return;
    }
    std::error_code ec;
    if (!std::filesystem::exists(untagged, ec)) {
        return;
    }
    LOG_INFO(Render, "Removing the shader cache {} from before per-GPU caches", untagged.string());
    std::thread{[untagged] {
        std::error_code error;
        std::filesystem::remove_all(untagged, error);
    }}.detach();
}

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    while (!stoken.stop_requested()) {
        {
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(request_cv, lk, stoken, [&] { return num_requests.load() != 0; });
        }

        if (stoken.stop_requested()) {
            break;
        }

        while (num_requests.load() != 0) {
            std::packaged_task<void()> request{};
            {
                std::scoped_lock lock{m_request};
                if (req_queue.empty()) {
                    continue;
                }
                request = std::move(req_queue.front());
                req_queue.pop();
            }

            if (request.valid()) {
                request();
                request.get_future().wait();
            }

            --num_requests;
        }
    }
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

void DataBase::SetDeviceTag(std::string tag) {
    device_tag = std::move(tag);
}

const std::string& DataBase::DeviceTag() const {
    return device_tag;
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_zero_struct(&zip_ar);

        const auto base_name = std::string{game_info.GameSerial()} + BbNanGuard::ShaderCacheSuffix();
        cache_path = GetUserPath(PathType::CacheDir) /
                     std::filesystem::path{base_name + DeviceSuffix()}.replace_extension(".zip");
        RemoveUntaggedCache(GetUserPath(PathType::CacheDir) /
                               std::filesystem::path{base_name}.replace_extension(".zip"),
                           cache_path);

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
    } else {
        const auto base_name = std::string{game_info.GameSerial()} + BbNanGuard::ShaderCacheSuffix();
        cache_path = GetUserPath(PathType::CacheDir) / (base_name + DeviceSuffix());
        RemoveUntaggedCache(GetUserPath(PathType::CacheDir) / base_name, cache_path);
        if (!std::filesystem::exists(cache_path)) {
            std::filesystem::create_directories(cache_path);
        }
        directory = cache_path;
    }

    io_worker = std::jthread{ProcessIO};
    opened = true;
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    io_worker.request_stop();
    io_worker.join();

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
    }

    opened = false;
    LOG_INFO(Render, "Cache dumped");
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    {
        auto request = std::packaged_task<void()>{[=]() {
            auto path{path_};
            path.replace_extension(GetBlobFileExtension(type));
            if (EmulatorSettings.IsPipelineCacheArchived()) {
                ASSERT_MSG(!ar_is_read_only,
                           "The archive is read-only. Did you forget to call `FinishPreload`?");
                if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), v.data(),
                                           v.size() * sizeof(T), MZ_BEST_COMPRESSION)) {
                    LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
                }
            } else {
                // bbport: write beside and rename into place, so a game that ends during the
                // write (the queue is not drained at exit) never leaves a truncated blob that
                // the next start would load.
                using namespace Common::FS;
                auto temp{path};
                temp += ".tmp";
                {
                    const auto file = IOFile{temp, FileAccessMode::Create};
                    if (!file.IsOpen() || file.Write(v) != v.size()) {
                        return;
                    }
                }
                std::error_code error;
                std::filesystem::rename(temp, path, error);
            }
        }};
        std::scoped_lock lock{m_request};
        req_queue.emplace(std::move(request));
    }

    std::scoped_lock lk{submit_mutex};
    ++num_requests;
    request_cv.notify_one();
    return true;
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        int index{-1};
        index = mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            LOG_WARNING(Render, "File {} is not found in the archive", path.string().c_str());
            return;
        }
        mz_zip_archive_file_stat stat{};
        mz_zip_reader_file_stat(&zip_ar, index, &stat);
        v.resize(stat.m_uncomp_size / sizeof(T));
        mz_zip_reader_extract_to_mem(&zip_ar, index, v.data(), stat.m_uncomp_size, 0);
    } else {
        const auto file = IOFile{path, FileAccessMode::Read};
        v.resize(file.GetSize() / sizeof(T));
        file.Read(v);
    }
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    const auto& ext = GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            file_name.fill(0);
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            if (std::string{file_name.data()}.ends_with(ext)) {
                mz_zip_archive_file_stat stat{};
                mz_zip_reader_file_stat(&zip_ar, index, &stat);
                std::vector<u8> data(stat.m_uncomp_size);
                mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0);
                func(std::move(data));
            }
        }
    } else {
        for (const auto& file_name : std::filesystem::directory_iterator{cache_path}) {
            if (file_name.path().extension().string().ends_with(ext)) {
                using namespace Common::FS;
                const auto& file = IOFile{file_name, FileAccessMode::Read};
                if (file.IsOpen()) {
                    std::vector<u8> data(file.GetSize());
                    file.Read(data);
                    func(std::move(data));
                }
            }
        }
    }
}

std::vector<std::string> DataBase::ListBlobNames(BlobType type) {
    std::vector<std::string> names;
    if (!opened) {
        return names;
    }
    const auto ext = "." + GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            const std::string entry{file_name.data()};
            if (entry.ends_with(ext)) {
                names.push_back(entry.substr(0, entry.size() - ext.size()));
            }
        }
        return names;
    }
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path, ec)) {
        if (entry.path().extension().string() == ext) {
            names.push_back(entry.path().stem().string());
        }
    }
    return names;
}

void DataBase::Flush(u32 max_ms) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);
    while (opened && num_requests.load() != 0 && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void DataBase::Clear() {
    if (!opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return;
    }
    std::error_code ec;
    u64 removed = 0;
    for (const auto& entry : std::filesystem::directory_iterator(cache_path, ec)) {
        if (entry.is_regular_file(ec) && std::filesystem::remove(entry.path(), ec)) {
            ++removed;
        }
    }
    LOG_WARNING(Render, "Pipeline cache cleared ({} files): it is rebuilt for this build", removed);
}

void DataBase::FinishPreload() {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
}

} // namespace Storage
