#pragma once

#include <string>

namespace pk {

// Session cookies and actor tokens are secrets. On a device without a secure
// element this can be an encrypted file; on desktop it can be the keyring.
class SecureStore {
public:
    virtual ~SecureStore() = default;
    virtual bool load(const std::string& key, std::string* value) = 0;
    virtual bool save(const std::string& key, const std::string& value) = 0;
    virtual bool erase(const std::string& key) = 0;
};

}  // namespace pk
