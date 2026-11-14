// src/cuda_loader.cpp
#include "cuda_loader.hpp"
#include "_base.hpp"
#include "post_processors/processor.hpp"
#include "post_processors/transfer.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <filesystem>
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace {

struct CudaBackend {
    using ProcessorFactory = IFrameProcessor *(*)(int, int, OutputFormat);

    using TransferFactory = IOutputTransfer *(*)();

    void *handle = nullptr;
    ProcessorFactory create_processor = nullptr;
    TransferFactory create_transfer = nullptr;
};

CudaBackend &backend() {
    static CudaBackend api;
    static std::once_flag once;

    std::call_once(once, [&] {
#if defined(_WIN32)
        HMODULE self = nullptr;
        const auto address = reinterpret_cast<LPCWSTR>(&backend);
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                address, &self)) {
            throw std::runtime_error("Cannot locate framepipe extension (GetModuleHandleExW)");
        }

        std::wstring filename(MAX_PATH, L'\0');
        DWORD length = 0;
        for (;;) {
            length = GetModuleFileNameW(self, filename.data(), static_cast<DWORD>(filename.size()));
            if (length == 0) {
                throw std::runtime_error("Cannot get framepipe extension path");
            }
            if (length < filename.size()) {
                filename.resize(length);
                break;
            }
            if (filename.size() >= 32768) {
                throw std::runtime_error("Framesync extension path is too long");
            }
            filename.resize(filename.size() * 2);
        }

        const auto path = std::filesystem::path(filename).parent_path() / L"_framepipe_cuda.pyd";
        HMODULE handle = LoadLibraryW(path.c_str());
        if (!handle) {
            throw std::runtime_error("CUDA backend unavailable: LoadLibraryW failed (Win32 error " +
                                     std::to_string(GetLastError()) + ") for " + path.string());
        }

        auto processor =
            reinterpret_cast<CudaBackend::ProcessorFactory>(GetProcAddress(handle, "framepipe_create_cuda_processor"));
        auto transfer =
            reinterpret_cast<CudaBackend::TransferFactory>(GetProcAddress(handle, "framepipe_create_cuda_transfer"));
        if (!processor || !transfer) {
            FreeLibrary(handle);
            throw std::runtime_error("CUDA backend exports are missing");
        }
#else
        Dl_info info{};

        if (!dladdr(reinterpret_cast<void*>(&backend), &info) ||
            !info.dli_fname) {
            throw std::runtime_error(
                "Cannot locate framepipe extension"
            );
        }

        std::string path(info.dli_fname);
        const auto pos = path.find_last_of('/');

        if (pos == std::string::npos) {
            throw std::runtime_error(
                "Cannot determine framepipe extension directory"
            );
        }

        path.resize(pos + 1);
        path += "_framepipe_cuda.so";

        void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);

        if (!handle) {
            const char* error = dlerror();
            throw std::runtime_error(
                std::string("CUDA backend unavailable: ") +
                (error ? error : "dlopen failed")
            );
        }

        auto processor = reinterpret_cast<CudaBackend::ProcessorFactory>(
            dlsym(handle, "framepipe_create_cuda_processor")
        );

        auto transfer = reinterpret_cast<CudaBackend::TransferFactory>(
            dlsym(handle, "framepipe_create_cuda_transfer")
        );

        if (!processor || !transfer) {
            dlclose(handle);
            throw std::runtime_error(
                "CUDA backend exports are missing"
            );
        }

#endif
        api.handle = reinterpret_cast<void *>(handle);
        api.create_processor = processor;
        api.create_transfer = transfer;
    });

    return api;
}

} // namespace

std::unique_ptr<IFrameProcessor> create_cuda_processor(int width, int height, OutputFormat format) {
    return std::unique_ptr<IFrameProcessor>(backend().create_processor(width, height, format));
}

std::unique_ptr<IOutputTransfer> create_cuda_output_transfer() {
    return std::unique_ptr<IOutputTransfer>(backend().create_transfer());
}
