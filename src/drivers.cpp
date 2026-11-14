// src/drivers.cpp
#include <cstdlib>
#include <filesystem>
#include <stdlib.h>

void configure_vaapi_driver_path() {
#if !defined(_WIN32)
    if (std::getenv("LIBVA_DRIVERS_PATH")) {
        return;
    }

    constexpr const char *paths[] = {
        "/usr/lib/x86_64-linux-gnu/dri",
        "/usr/lib64/dri",
        "/usr/lib/dri",
        "/usr/local/lib/dri",
    };

    for (const char *path : paths) {
        if (std::filesystem::exists(path)) {
            setenv("LIBVA_DRIVERS_PATH", path, 0);
            return;
        }
    }
#endif
}
