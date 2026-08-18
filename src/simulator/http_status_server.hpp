#pragma once

#include "pixelstatus/status_api.hpp"

#include <cstdint>
#include <memory>
#include <string>

namespace pixelstatus::simulator {

struct HttpStatusServerImpl;

class HttpStatusServer {
public:
    explicit HttpStatusServer(StatusApi& api);
    ~HttpStatusServer();

    HttpStatusServer(const HttpStatusServer&) = delete;
    HttpStatusServer& operator=(const HttpStatusServer&) = delete;

    [[nodiscard]] bool start(std::string bind_address, std::uint16_t port);
    void stop();

    [[nodiscard]] bool running() const;
    [[nodiscard]] std::string error() const;

private:
    std::unique_ptr<HttpStatusServerImpl> impl_;
};

}  // namespace pixelstatus::simulator
