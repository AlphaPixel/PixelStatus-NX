#include "pixelstatus/host/http_display_driver.hpp"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

namespace pixelstatus::host {
namespace {

using json = nlohmann::json;

constexpr std::chrono::milliseconds minimum_refresh_interval{16};
constexpr std::chrono::milliseconds maximum_refresh_interval{5 * 60 * 1000};

constexpr std::string_view display_page = R"HTML(<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
  <meta name="theme-color" content="#000000">
  <meta name="color-scheme" content="dark">
  <meta name="mobile-web-app-capable" content="yes">
  <meta name="apple-mobile-web-app-capable" content="yes">
  <meta name="apple-mobile-web-app-status-bar-style" content="black-translucent">
  <title>PixelStatus NX Display</title>
  <link rel="manifest" href="/manifest.webmanifest">
  <link rel="icon" href="/favicon.svg" type="image/svg+xml">
  <style>
    :root { color-scheme: dark; background: #000; }
    * { box-sizing: border-box; }
    html, body {
      width: 100%;
      height: 100%;
      margin: 0;
      overflow: hidden;
      background: #000;
    }
    body {
      display: grid;
      place-items: center;
      font: 13px/1.3 system-ui, -apple-system, "Segoe UI", sans-serif;
    }
    body.idle { cursor: none; }
    #pixelstatus-display {
      display: block;
      width: min(100vw, calc(100vh * var(--display-aspect, 1)));
      height: min(100vh, calc(100vw / var(--display-aspect, 1)));
      image-rendering: pixelated;
      image-rendering: crisp-edges;
      background: #000;
    }
    #settings {
      position: fixed;
      z-index: 2;
      top: 0;
      right: 0;
      min-width: 270px;
      padding: 12px;
      display: grid;
      grid-template-columns: auto minmax(105px, auto);
      align-items: center;
      gap: 8px 10px;
      color: #f4f7fa;
      background: rgb(17 20 26 / 92%);
      border: 1px solid rgb(255 255 255 / 18%);
      border-width: 0 0 1px 1px;
      border-radius: 0 0 0 10px;
      box-shadow: 0 8px 30px rgb(0 0 0 / 45%);
      opacity: 0;
      transform: translateY(-8px);
      transition: opacity 160ms ease, transform 160ms ease;
    }
    #settings:hover,
    #settings:focus-within,
    #settings.reveal {
      opacity: 1;
      transform: translateY(0);
    }
    #settings label { color: #c8d0db; }
    #settings select,
    #settings button {
      min-height: 32px;
      border: 1px solid #4b5563;
      border-radius: 6px;
      padding: 5px 8px;
      color: #fff;
      background: #252b35;
      font: inherit;
    }
    #settings button { grid-column: 1 / -1; cursor: pointer; }
    #connection {
      grid-column: 1 / -1;
      min-height: 1.3em;
      color: #9ca8b8;
      text-align: right;
    }
    #connection.error { color: #ff7992; }
    @media (prefers-reduced-motion: reduce) {
      #settings { transition: none; }
    }
  </style>
</head>
<body>
  <canvas id="pixelstatus-display" width="1" height="1" aria-label="PixelStatus NX pixel display"></canvas>
  <section id="settings" class="reveal" aria-label="Display controls">
    <label for="refresh-rate">Refresh rate</label>
    <select id="refresh-rate">
      <option value="17">60 frames/second</option>
      <option value="33">30 frames/second</option>
      <option value="50">20 frames/second</option>
      <option value="100">10 frames/second</option>
      <option value="250">4 frames/second</option>
      <option value="500">2 frames/second</option>
      <option value="1000">Every second</option>
      <option value="2000">Every 2 seconds</option>
      <option value="5000">Every 5 seconds</option>
      <option value="10000">Every 10 seconds</option>
      <option value="30000">Every 30 seconds</option>
      <option value="60000">Every minute</option>
      <option value="120000">Every 2 minutes</option>
      <option value="300000">Every 5 minutes</option>
    </select>
    <button id="clean-window" type="button">Open minimal display window</button>
    <output id="connection" aria-live="polite">Connecting…</output>
  </section>
  <script>
    (() => {
      "use strict";

      const canvas = document.getElementById("pixelstatus-display");
      const context = canvas.getContext("2d", { alpha: false });
      const settings = document.getElementById("settings");
      const selector = document.getElementById("refresh-rate");
      const connection = document.getElementById("connection");
      let refreshMilliseconds = 100;
      let etag = "";
      let timer = 0;
      let idleTimer = 0;
      let serverDefaultApplied = false;
      let polling = false;

      const nearestRefreshOption = milliseconds => {
        const options = Array.from(selector.options);
        return options.reduce((nearest, option) =>
          Math.abs(Number(option.value) - milliseconds)
            < Math.abs(Number(nearest.value) - milliseconds) ? option : nearest);
      };

      try {
        const saved = Number(localStorage.getItem("pixelstatus-refresh-ms"));
        if (Number.isFinite(saved) && saved >= 16 && saved <= 300000) {
          nearestRefreshOption(saved).selected = true;
          refreshMilliseconds = Number(selector.value);
          serverDefaultApplied = true;
        }
      } catch (_) {
        // Storage can be unavailable in private or locked-down browser contexts.
      }

      const setConnection = (message, failed = false) => {
        connection.value = message;
        connection.classList.toggle("error", failed);
      };

      const render = frame => {
        if (frame.schema_version !== 1 || frame.format !== "rgb888"
            || !Number.isInteger(frame.width) || !Number.isInteger(frame.height)
            || frame.width < 1 || frame.height < 1
            || !Array.isArray(frame.pixels)
            || frame.pixels.length !== frame.width * frame.height) {
          throw new Error("The server returned an invalid frame");
        }

        if (!serverDefaultApplied && Number.isFinite(frame.default_refresh_ms)) {
          nearestRefreshOption(frame.default_refresh_ms).selected = true;
          refreshMilliseconds = Number(selector.value);
          serverDefaultApplied = true;
        }

        if (canvas.width !== frame.width || canvas.height !== frame.height) {
          canvas.width = frame.width;
          canvas.height = frame.height;
          canvas.style.setProperty("--display-aspect", frame.width / frame.height);
        }

        const image = context.createImageData(frame.width, frame.height);
        for (let index = 0; index < frame.pixels.length; ++index) {
          const rgb = Number(frame.pixels[index]) >>> 0;
          const offset = index * 4;
          image.data[offset] = (rgb >>> 16) & 255;
          image.data[offset + 1] = (rgb >>> 8) & 255;
          image.data[offset + 2] = rgb & 255;
          image.data[offset + 3] = 255;
        }
        context.putImageData(image, 0, 0);
        setConnection(`${frame.width}×${frame.height} · frame ${frame.sequence}`);
      };

      const schedule = () => {
        clearTimeout(timer);
        if (!document.hidden) {
          timer = window.setTimeout(poll, refreshMilliseconds);
        }
      };

      const poll = async () => {
        clearTimeout(timer);
        if (document.hidden || polling) {
          return;
        }
        polling = true;
        try {
          const headers = etag ? { "If-None-Match": etag } : {};
          const response = await fetch("/api/v1/display", { headers, cache: "no-cache" });
          if (response.status === 304) {
            setConnection(connection.value || "Connected");
          } else if (response.ok) {
            etag = response.headers.get("ETag") || "";
            render(await response.json());
          } else {
            throw new Error(`HTTP ${response.status}`);
          }
        } catch (error) {
          setConnection(`Disconnected · ${error.message}`, true);
        } finally {
          polling = false;
          schedule();
        }
      };

      selector.addEventListener("change", () => {
        refreshMilliseconds = Number(selector.value);
        serverDefaultApplied = true;
        try {
          localStorage.setItem("pixelstatus-refresh-ms", String(refreshMilliseconds));
        } catch (_) {
          // The selected rate still applies for this page session.
        }
        schedule();
      });

      document.getElementById("clean-window").addEventListener("click", () => {
        window.open(
          location.href,
          "pixelstatus-nx-display",
          "popup=yes,width=960,height=640,resizable=yes");
      });

      const revealPointer = () => {
        document.body.classList.remove("idle");
        clearTimeout(idleTimer);
        idleTimer = window.setTimeout(() => document.body.classList.add("idle"), 2200);
      };
      window.addEventListener("pointermove", revealPointer, { passive: true });
      document.addEventListener("visibilitychange", () => {
        if (!document.hidden) {
          clearTimeout(timer);
          poll();
        }
      });

      window.setTimeout(() => settings.classList.remove("reveal"), 1800);
      revealPointer();
      poll();
    })();
  </script>
</body>
</html>)HTML";

constexpr std::string_view web_manifest = R"JSON({
  "name": "PixelStatus NX Display",
  "short_name": "PixelStatus NX",
  "description": "Live PixelStatus NX browser display",
  "start_url": "/",
  "scope": "/",
  "display": "standalone",
  "display_override": ["window-controls-overlay", "standalone", "minimal-ui"],
  "background_color": "#000000",
  "theme_color": "#000000",
  "icons": [
    {"src": "/favicon.svg", "sizes": "any", "type": "image/svg+xml", "purpose": "any maskable"}
  ]
})JSON";

constexpr std::string_view favicon = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 16 16">
<rect width="16" height="16" rx="2" fill="#080a10"/>
<rect x="2" y="2" width="5" height="5" fill="#00c853"/>
<rect x="9" y="2" width="5" height="5" fill="#ffd600"/>
<rect x="2" y="9" width="5" height="5" fill="#40c4ff"/>
<rect x="9" y="9" width="5" height="5" fill="#ff1744"/>
</svg>)SVG";

std::uint32_t packed_rgb(const Rgb color) {
    return (static_cast<std::uint32_t>(color.r) << 16U)
        | (static_cast<std::uint32_t>(color.g) << 8U)
        | static_cast<std::uint32_t>(color.b);
}

std::string serialize_frame(
    const Frame& frame,
    std::uint64_t sequence,
    std::chrono::milliseconds default_refresh_interval) {
    json pixels = json::array();
    pixels.get_ref<json::array_t&>().reserve(frame.size());
    for (const auto color : frame.pixels()) {
        pixels.push_back(packed_rgb(color));
    }

    return json{
        {"schema_version", 1},
        {"width", frame.width()},
        {"height", frame.height()},
        {"sequence", sequence},
        {"format", "rgb888"},
        {"default_refresh_ms", default_refresh_interval.count()},
        {"pixels", std::move(pixels)},
    }.dump();
}

void apply_common_headers(httplib::Response& response) {
    response.set_header("Referrer-Policy", "no-referrer");
    response.set_header("X-Content-Type-Options", "nosniff");
    response.set_header("X-Frame-Options", "SAMEORIGIN");
}

void apply_page_headers(httplib::Response& response) {
    apply_common_headers(response);
    response.set_header(
        "Content-Security-Policy",
        "default-src 'none'; connect-src 'self'; img-src 'self'; "
        "manifest-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'");
    response.set_header("Cache-Control", "no-cache");
}

}  // namespace

struct HttpDisplayDriverImpl {
    HttpDisplayDriverImpl(
        std::size_t display_width,
        std::size_t display_height,
        HttpDisplayOptions display_options)
        : width(display_width),
          height(display_height),
          options(std::move(display_options)),
          latest_frame(display_width, display_height),
          serialized_frame(serialize_frame(
              latest_frame,
              0,
              options.default_refresh_interval)) {}

    std::size_t width{};
    std::size_t height{};
    HttpDisplayOptions options;
    httplib::Server server;
    std::thread worker;
    mutable std::mutex mutex;
    Frame latest_frame;
    std::string serialized_frame;
    std::uint64_t sequence{};
    std::uint16_t bound_port{};
    bool frame_pending{};
    bool is_running{};
    bool stop_requested{};
    DriverState driver_state;
};

HttpDisplayDriver::HttpDisplayDriver(
    std::size_t width,
    std::size_t height,
    HttpDisplayOptions options)
    : impl_(std::make_unique<HttpDisplayDriverImpl>(
          width,
          height,
          std::move(options))) {}

HttpDisplayDriver::~HttpDisplayDriver() {
    stop();
}

bool HttpDisplayDriver::begin() {
    if (running()) {
        return false;
    }
    if (impl_->width == 0U || impl_->height == 0U || impl_->options.bind_address.empty()) {
        std::scoped_lock lock(impl_->mutex);
        impl_->driver_state.connection = DriverConnectionState::failed;
        impl_->driver_state.detail = "HTTP display dimensions and bind address must be valid";
        return false;
    }
    if (impl_->width > std::numeric_limits<std::size_t>::max() / impl_->height
        || impl_->width * impl_->height > 256U * 256U) {
        std::scoped_lock lock(impl_->mutex);
        impl_->driver_state.connection = DriverConnectionState::failed;
        impl_->driver_state.detail = "HTTP display contains too many pixels";
        return false;
    }
    if (impl_->options.default_refresh_interval < minimum_refresh_interval
        || impl_->options.default_refresh_interval > maximum_refresh_interval) {
        std::scoped_lock lock(impl_->mutex);
        impl_->driver_state.connection = DriverConnectionState::failed;
        impl_->driver_state.detail = "HTTP display refresh interval must be between 16ms and 5 minutes";
        return false;
    }

    impl_->server.Get("/", [](const httplib::Request&, httplib::Response& response) {
        apply_page_headers(response);
        response.set_content(display_page.data(), display_page.size(), "text/html; charset=utf-8");
    });
    impl_->server.Get("/index.html", [](const httplib::Request&, httplib::Response& response) {
        apply_page_headers(response);
        response.set_content(display_page.data(), display_page.size(), "text/html; charset=utf-8");
    });
    impl_->server.Get("/manifest.webmanifest", [](const httplib::Request&, httplib::Response& response) {
        apply_common_headers(response);
        response.set_header("Cache-Control", "public, max-age=3600");
        response.set_content(
            web_manifest.data(), web_manifest.size(), "application/manifest+json; charset=utf-8");
    });
    impl_->server.Get("/favicon.svg", [](const httplib::Request&, httplib::Response& response) {
        apply_common_headers(response);
        response.set_header("Cache-Control", "public, max-age=86400");
        response.set_content(favicon.data(), favicon.size(), "image/svg+xml; charset=utf-8");
    });
    impl_->server.Get("/api/v1/display", [this](
        const httplib::Request& request,
        httplib::Response& response) {
        std::string body;
        std::string etag;
        {
            std::scoped_lock lock(impl_->mutex);
            body = impl_->serialized_frame;
            etag = "\"frame-" + std::to_string(impl_->sequence) + '\"';
            impl_->frame_pending = false;
        }
        apply_common_headers(response);
        response.set_header("Cache-Control", "private, no-cache");
        response.set_header("ETag", etag);
        if (request.get_header_value("If-None-Match") == etag) {
            response.status = 304;
            return;
        }
        response.set_content(std::move(body), "application/json; charset=utf-8");
    });

    int port = 0;
    if (impl_->options.port == 0U) {
        port = impl_->server.bind_to_any_port(impl_->options.bind_address);
    } else if (impl_->server.bind_to_port(
                   impl_->options.bind_address,
                   static_cast<int>(impl_->options.port))) {
        port = static_cast<int>(impl_->options.port);
    }
    if (port <= 0 || port > std::numeric_limits<std::uint16_t>::max()) {
        std::scoped_lock lock(impl_->mutex);
        impl_->driver_state.connection = DriverConnectionState::failed;
        impl_->driver_state.detail = "Unable to bind HTTP display server to "
            + impl_->options.bind_address + ':' + std::to_string(impl_->options.port);
        return false;
    }

    {
        std::scoped_lock lock(impl_->mutex);
        impl_->bound_port = static_cast<std::uint16_t>(port);
        impl_->is_running = true;
        impl_->stop_requested = false;
        impl_->driver_state.connection = DriverConnectionState::ready;
        const auto advertised_host = impl_->options.bind_address == "0.0.0.0"
            ? std::string("127.0.0.1")
            : impl_->options.bind_address;
        impl_->driver_state.detail = "HTTP display ready at http://" + advertised_host
            + ':' + std::to_string(impl_->bound_port) + '/';
    }
    impl_->worker = std::thread([this] {
        const auto completed_normally = impl_->server.listen_after_bind();
        std::scoped_lock lock(impl_->mutex);
        impl_->is_running = false;
        if (!completed_normally && !impl_->stop_requested) {
            impl_->driver_state.connection = DriverConnectionState::failed;
            impl_->driver_state.detail = "HTTP display server stopped unexpectedly";
        }
    });
    return true;
}

FrameSubmitResult HttpDisplayDriver::submit_frame(const Frame& frame) {
    std::scoped_lock lock(impl_->mutex);
    if (!impl_->is_running || frame.width() != impl_->width || frame.height() != impl_->height) {
        return FrameSubmitResult::unavailable;
    }

    auto result = FrameSubmitResult::accepted;
    if (impl_->frame_pending) {
        result = FrameSubmitResult::coalesced;
        ++impl_->driver_state.coalesced_frames;
    }
    impl_->latest_frame = frame;
    ++impl_->sequence;
    impl_->serialized_frame = serialize_frame(
        impl_->latest_frame,
        impl_->sequence,
        impl_->options.default_refresh_interval);
    impl_->frame_pending = true;
    ++impl_->driver_state.submitted_frames;
    return result;
}

DriverState HttpDisplayDriver::state() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->driver_state;
}

void HttpDisplayDriver::stop() {
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
    if (impl_->driver_state.connection != DriverConnectionState::failed) {
        impl_->driver_state.connection = DriverConnectionState::stopped;
        impl_->driver_state.detail = "HTTP display stopped";
    }
}

bool HttpDisplayDriver::running() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->is_running;
}

std::uint16_t HttpDisplayDriver::port() const {
    std::scoped_lock lock(impl_->mutex);
    return impl_->bound_port;
}

std::string HttpDisplayDriver::url() const {
    std::scoped_lock lock(impl_->mutex);
    const auto host = impl_->options.bind_address == "0.0.0.0"
        ? std::string("127.0.0.1")
        : impl_->options.bind_address;
    return "http://" + host + ':' + std::to_string(impl_->bound_port) + '/';
}

}  // namespace pixelstatus::host
