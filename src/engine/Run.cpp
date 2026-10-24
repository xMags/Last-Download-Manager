#include "engine/Run.h"

namespace ldm::engine {

void ConnectionOwner::Attach(CURL* easy, ConnectionOwner* owner)
{
    curl_easy_setopt(easy, CURLOPT_PRIVATE, static_cast<void*>(owner));
}

ConnectionOwner* ConnectionOwner::Of(CURL* easy)
{
    void* owner = nullptr;
    if (curl_easy_getinfo(easy, CURLINFO_PRIVATE, &owner) != CURLE_OK) {
        return nullptr;
    }
    return static_cast<ConnectionOwner*>(owner);  // Attach stored exactly this pointer type
}

}  // namespace ldm::engine
