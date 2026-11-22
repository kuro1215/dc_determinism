#include "common/log.hpp"

#include <cstdio>
#include <mutex>

namespace dc_determinism {

void log(const std::string& message) {
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    std::fwrite(message.data(), 1, message.size(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
}

}  // namespace dc_determinism
