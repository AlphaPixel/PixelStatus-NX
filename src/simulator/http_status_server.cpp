#include "http_status_server.hpp"

#include <httplib.h>

#include <chrono>
#include <mutex>
#include <thread>
#include <utility>

namespace pixelstatus::simulator {

struct HttpStatusServerImpl {
    explicit HttpStatusServerImpl(StatusApi& status_api) : api(status_api) {}

    StatusApi& api;
    httplib::Server server;
    std::thread worker;
    mutable std::mutex mutex;
    bool is_running{};
    bool stop_requested{};
    std::string last_error;
};

namespace {

void apply_response(const ApiResponse& source, httplib::Response& destination) {
    destination.status = source.status;
    for (const auto& [name, value] : source.headers) {
        destination.set_header(name, value);
    }
    destination.set_content(source.body, source.content_type);
}

ApiRequest convert_request(const httplib::Request& request) {
    ApiRequest converted;
    converted.method = request.method;
    converted.target = request.path;
    converted.body = request.body;
    for (const auto& [name, value] : request.headers) {
        converted.headers.insert_or_assign(name, value);
    }
    return converted;
}

}  // namespace

HttpStatusServer::HttpStatusServer(StatusApi& api)
    : impl_(std::make_unique<HttpStatusServerImpl>(api)) {}

HttpStatusServer::~HttpStatusServer() {
    stop();
}

bool HttpStatusServer::start(std::string bind_address, std::uint16_t port) {
    if (running()) {
        return false;
    }

    impl_->server.set_payload_max_length(4U * 1024U);
    const auto dispatch = [this](const httplib::Request& request, httplib::Response& response) {
        apply_response(
            impl_->api.handle(convert_request(request), std::chrono::steady_clock::now()),
            response);
    };
    const std::string route_pattern = R"(/api/v1/status(?:/.*)?)";
    impl_->server.Get(route_pattern, dispatch);
    impl_->server.Post(route_pattern, dispatch);
    impl_->server.Put(route_pattern, dispatch);
    impl_->server.Delete(route_pattern, dispatch);
    impl_->server.Patch(route_pattern, dispatch);

    if (!impl_->server.bind_to_port(bind_address, static_cast<int>(port))) {
        std::scoped_lock lock(impl_->mutex);
        impl_->last_error = "Unable to bind HTTP status server to " + bind_address + ':'
            + std::to_string(port);
        return false;
    }

    {
        std::scoped_lock lock(impl_->mutex);
        impl_->is_running = true;
        impl_->stop_requested = false;
        impl_->last_error.clear();
    }
    impl_->worker = std::thread([this] {
        const auto completed_normally = impl_->server.listen_after_bind();
        std::scoped_lock lock(impl_->mutex);
        impl_->is_running = false;
        if (!completed_normally && !impl_->stop_requested && impl_->last_error.empty()) {
            impl_->last_error = "HTTP status server stopped unexpectedly";
        }
    });
    return true;
}

void HttpStatusServer::stop() {
    {
        std::scoped_lock lock(impl_->mutex);
        impl_->stop_requested = true;
    }
    impl_->server.stop();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    std::scoped_lock lock(impl_->mutex);
    impl_->is_running = false;
}

bool HttpStatusServer::running() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->is_running;
}

std::string HttpStatusServer::error() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->last_error;
}

}  // namespace pixelstatus::simulator
