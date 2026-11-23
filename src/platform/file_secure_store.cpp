#include "platform/file_secure_store.h"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace pk {
namespace {

std::filesystem::path default_directory() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::filesystem::path(xdg) / "puck-kiosk";
    }
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config" / "puck-kiosk";
    }
    return std::filesystem::path("build") / "puck-kiosk";
}

bool safe_key(const std::string& key) {
    if (key.empty() || key == "." || key == "..") {
        return false;
    }
    for (const char character : key) {
        const auto byte = static_cast<unsigned char>(character);
        if (!std::isalnum(byte) && character != '.' && character != '_' && character != '-') {
            return false;
        }
    }
    return true;
}

void set_owner_only(const std::filesystem::path& path, bool directory) {
    std::error_code error;
    const auto permissions = std::filesystem::perms::owner_read |
                             std::filesystem::perms::owner_write |
                             (directory ? std::filesystem::perms::owner_exec
                                        : std::filesystem::perms::none);
    std::filesystem::permissions(path, permissions, std::filesystem::perm_options::replace, error);
}

}  // namespace

FileSecureStore::FileSecureStore(std::filesystem::path directory)
    : directory_(std::move(directory)) {
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) {
        return;
    }
    set_owner_only(directory_, true);
    ready_ = true;
}

std::filesystem::path FileSecureStore::path_for(const std::string& key) const {
    return directory_ / key;
}

bool FileSecureStore::load(const std::string& key, std::string* value) {
    if (!ready_ || !safe_key(key)) {
        return false;
    }
    std::ifstream input(path_for(key), std::ios::binary);
    if (!input) {
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (value) {
        *value = contents.str();
    }
    return true;
}

bool FileSecureStore::save(const std::string& key, const std::string& value) {
    if (!ready_ || !safe_key(key)) {
        return false;
    }
    const auto target = path_for(key);
    auto temporary = target;
    temporary += ".tmp";

    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            return false;
        }
        output.write(value.data(), static_cast<std::streamsize>(value.size()));
        output.flush();
        if (!output) {
            return false;
        }
    }

    set_owner_only(temporary, false);

    std::error_code error;
    std::filesystem::rename(temporary, target, error);
    if (error) {
        std::filesystem::remove(target, error);
        error.clear();
        std::filesystem::rename(temporary, target, error);
    }
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

bool FileSecureStore::erase(const std::string& key) {
    if (!ready_ || !safe_key(key)) {
        return false;
    }
    std::error_code error;
    std::filesystem::remove(path_for(key), error);
    return !error;
}

std::unique_ptr<FileSecureStore> create_file_secure_store(std::string* error) {
    const auto directory = default_directory();
    auto store = std::make_unique<FileSecureStore>(directory);
    if (!store->ready()) {
        if (error) {
            *error = "cannot create secure store directory " + directory.string();
        }
        return nullptr;
    }
    return store;
}

}  // namespace pk
