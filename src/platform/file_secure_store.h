#pragma once

#include "platform/secure_store.h"

#include <filesystem>
#include <memory>
#include <string>

namespace pk {

// File-backed SecureStore for desktop and headless runs. Each value lives in
// its own 0600 file under a 0700 directory outside the repository.
class FileSecureStore final : public SecureStore {
public:
    explicit FileSecureStore(std::filesystem::path directory);

    bool load(const std::string& key, std::string* value) override;
    bool save(const std::string& key, const std::string& value) override;
    bool erase(const std::string& key) override;

    bool ready() const { return ready_; }
    const std::filesystem::path& directory() const { return directory_; }

private:
    std::filesystem::path path_for(const std::string& key) const;

    std::filesystem::path directory_;
    bool ready_ = false;
};

std::unique_ptr<FileSecureStore> create_file_secure_store(std::string* error);

}  // namespace pk
