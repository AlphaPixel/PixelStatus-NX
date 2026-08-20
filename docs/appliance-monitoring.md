# Appliance Monitoring and TLS

This document fixes the intended transport boundary for the first production
appliance integrations. Monitor evaluation, scheduling, and state publication
remain portable. Desktop and ESP32 runners may use different native network
libraries as long as they implement the same `MonitorRunner` result contract.

## Current Capability

The desktop HTTP runner currently supports bounded `GET`, `HEAD`, `POST`, `PUT`,
`PATCH`, and `DELETE` requests with explicit headers and request bodies. It can
observe response status or body, or extract a scalar with RFC 6901 JSON Pointer.
Plain HTTP is integration-tested against the loopback fixture. On Windows, HTTPS
uses WinHTTP/Schannel with system certificate and hostname validation.

Header values support named `${secret:name}` substitution. The Windows host
resolves environment variables for development and generic Windows Credential
Manager entries for production. Resolution occurs once during monitor creation;
unresolved references fail startup and resolved values are never placed in
diagnostics.

## TLS Backends

| Target | Preferred backend | Trust sources | Reason |
| --- | --- | --- | --- |
| Windows desktop | WinHTTP with Schannel (implemented) | Windows root stores; optional installed private CA | No separately built TLS library, native proxy and certificate behavior |
| ESP32 | ESP-IDF `esp_http_client` over ESP-TLS/mbedTLS | ESP x509 bundle or per-appliance PEM CA; optional pin | Native supported firmware path with bounded configuration |
| Optional portable desktop | `cpp-httplib` with mbedTLS, OpenSSL, or wolfSSL | Backend CA file/path | Useful if a non-Windows host is added; introduces a native TLS dependency |
| Optional feature-rich desktop | libcurl, preferably its Schannel build on Windows | Windows root stores or curl CA configuration | Consider only if proxy, cookies, or authentication workflows outgrow WinHTTP |

WinHTTP performs TLS and server-certificate validation through Windows. This lets
development machines trust a local appliance CA through the normal Windows
certificate store. A per-appliance pin is useful when changing the machine-wide
trust store is undesirable, but pin verification and rollover remain a later
increment.

ESP-IDF HTTPS uses ESP-TLS and mbedTLS. Public Internet services can use the ESP
certificate bundle; a self-signed or privately issued LAN appliance should provide
its root CA as a bounded PEM asset. Hostname validation must remain enabled, so a
local DNS name matching the certificate is preferable to connecting by raw IP.

The portable configuration should eventually expose these policies:

- `system` or `public_bundle`: use the platform's normal trusted roots;
- `custom_ca`: resolve a named CA asset, without embedding it in every monitor;
- `pin_sha256`: verify a configured certificate or SPKI digest;
- `insecure_diagnostic`: disable verification only in an explicitly enabled,
  visibly degraded diagnostic mode.

Redirects remain disabled by default so credentials cannot silently cross origins.
Client certificates are possible with both Schannel and ESP-TLS but are not needed
by the four initial targets and should not be part of the first TLS increment.

Official references:

- [WinHTTP SSL handling](https://learn.microsoft.com/windows/win32/winhttp/ssl-in-winhttp)
- [`cpp-httplib` TLS options](https://github.com/yhirose/cpp-httplib)
- [ESP-IDF HTTP client](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/esp_http_client.html)
- [ESP x509 certificate bundle](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-reference/protocols/esp_crt_bundle.html)
- [curl certificate verification](https://curl.se/docs/sslcerts.html)

## Credentials

Production configuration refers to credentials by name instead of containing their
values, for example `"X-API-Key": "${secret:unifi-api-key}"` or
`"Authorization": "Bearer ${secret:truenas-api-key}"`. The implemented Windows
resolver first checks `PIXELSTATUS_SECRET_UNIFI_API_KEY`, then the current user's
generic Credential Manager target `PixelStatus-NX/unifi-api-key`. Environment
variables are intended for tests and unattended development; Credential Manager is
the desktop production source.

Resolution occurs immediately before runner construction. Invalid or unresolved
references fail monitor registration, resolved headers are validated again, and
diagnostics never log resolved values. Encrypted NVS remains the corresponding
ESP32 implementation.

## TrueNAS CORE

TrueNAS CORE exposes its Web UI, REST API, and WebSocket API on the appliance's HTTP
and HTTPS service. CORE 13 commonly provides REST v2 endpoints below
`/api/v2.0/`; the exact endpoint and response schema must be checked against the
target appliance's local API documentation before a profile is committed. TrueNAS
CORE is now a maintenance product rather than the actively developed TrueNAS line,
so the CORE adapter must not silently assume the newer SCALE JSON-RPC API.

Initial mechanism:

1. Use HTTPS with a valid local certificate, a trusted private CA, or an explicit
   pin.
2. Send an API key through `Authorization: Bearer <secret>`.
3. Begin with a scalar health or system-information request to exercise the generic
   HTTP/JSON runner.
4. Add bounded JSON array aggregation/filtering, or a small TrueNAS adapter, for
   pool health and active-alert counts. Scalar JSON Pointer alone cannot summarize
   an arbitrary collection.

The appliance-local `/api/docs/` page is authoritative for the installed CORE
release. Useful official background includes the
[TrueNAS 13 security whitepaper](https://www.truenas.com/wp-content/uploads/2023/06/TrueNAS_13.0_Security_White_Paper_June_2023.pdf),
the [TrueNAS API portal](https://api.truenas.com/), and the
[TrueNAS release/lifecycle guidance](https://cdn.truenas.com/docs/softwarestatus/).

## UniFi Network

Recent UniFi Network releases expose a documented local Network API. API keys are
sent in `X-API-Key`, and device responses include a state such as online or offline.
The URL prefix, API version, site ID, and device ID must be discovered from the
installed Network application's **Integrations** page because the local API follows
the installed controller version. Cloud Gateways normally serve local management
over HTTPS port 443; self-hosted Network servers commonly use 8443.

Initial mechanism:

1. Create a least-privilege local API key and store it through the secret provider.
2. Trust the gateway/controller certificate or configure a deliberate pin.
3. Use the generic HTTP runner with `X-API-Key` for a device-detail request and
   select its scalar state through JSON Pointer.
4. Add UniFi-specific discovery only if maintaining site and device identifiers by
   hand proves fragile.

Official references:

- [Getting started with the official UniFi API](https://help.ui.com/hc/en-us/articles/30076656117655-Getting-Started-with-the-Official-UniFi-API)
- [UniFi Network API documentation](https://developer.ui.com/network/)
- [UniFi local management and ports](https://help.ui.com/hc/en-us/articles/28457353760919-UniFi-Local-Management)

## Netgear Cable Modem

Netgear cable-modem management is model- and firmware-specific. For example, the
CM1000v2 manual documents a local page at `http://192.168.100.1`, an administrator
login, and Cable Connection pages containing initialization and channel status, but
it does not document a supported machine API.

Initial mechanism:

1. Use TCP connect or an unauthenticated HTTP request for basic reachability.
2. Record the exact model, firmware, login request, cookies, CSRF behavior, and
   status requests from the browser network inspector before writing a detailed
   adapter.
3. If stable endpoints exist, implement a bounded model-specific session and HTML
   parser. Do not assume HTTP Basic authentication.
4. If the modem exposes credentials only over plaintext HTTP, prefer a small trusted
   LAN proxy that publishes a sanitized PixelStatus state instead of placing the
   login workflow on the ESP32.

Reference: [Netgear CM1000v2 user manual](https://www.downloads.netgear.com/files/GDC/CM1000v2/CM1000v2_UM_EN.pdf).

## Starlink Mini

Starlink is not an HTTPS/REST integration. SpaceX publishes an officially supported
local device API using gRPC and protobuf. The terminal is reachable at
`192.168.100.1:9200`; a Starlink router is reachable at `192.168.1.1:9000` when the
local network routes to those addresses. The official demonstration uses an
insecure gRPC channel, so this local link does not use TLS.

Initial mechanism:

1. Generate code from SpaceX's official `device.proto` and call the unary
   `Device.Handle` method for diagnostics/status.
2. During host-first development, run a desktop sidecar that translates the gRPC
   result into the existing authenticated PixelStatus push API. This avoids adding a
   large gRPC C++ dependency to the simulator or firmware.
3. Capture representative protobuf responses as fixtures and test all parsing and
   evaluation on Win32.
4. Only after behavior is stable, decide between retaining the proxy in production
   and implementing a minimal ESP32 client with HTTP/2, gRPC framing, and bounded
   protobuf generation such as nanopb.

Official sources:

- [SpaceX device API overview](https://github.com/SpaceExplorationTechnologies/enterprise-api/blob/master/device-api/README.md)
- [Official device protobuf](https://github.com/SpaceExplorationTechnologies/enterprise-api/blob/master/device-api/device.proto)
- [Official Python demonstration](https://github.com/SpaceExplorationTechnologies/enterprise-api/blob/master/device-api/demo.py)

## Remaining Implementation Order

Named secret references, environment/Credential Manager resolution, redaction
tests, and the WinHTTP/Schannel system-trust runner are complete. Continue with:

1. Add deterministic certificate fixtures for trusted, unknown-CA, hostname-
   mismatch, and pinned-certificate cases; implement per-monitor pinning or private
   CA selection only after that policy is fixed.
2. Validate one TrueNAS scalar request and one UniFi device-state request against
   user-supplied fixtures or live appliances; add array aggregation only where a
   real response requires it.
3. Build the Starlink desktop sidecar and recorded-response fixtures.
4. Identify the exact Netgear modem model and decide whether reachability is enough
   or a model-specific proxy is justified.
5. Keep all of the above host-tested. Add ESP-IDF `esp_http_client` and encrypted-NVS
   adapters only when firmware deployment becomes necessary.
