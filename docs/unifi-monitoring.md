# UniFi Network Monitoring

The Windows host can query the official local UniFi Network integration API through
the small Python collector in `tools/unifi`. The collector combines application,
site, adopted-gateway, latest-statistics, and WAN-interface responses into one
bounded JSON document at `http://127.0.0.1:18950/health`. Existing PixelStatus HTTP
monitors consume that document, so no vendor-specific behavior enters the renderer,
state store, layout engine, or display drivers.

This adapter is implemented, covered by six standard-library unit tests, and
live-validated against the user's gateway. The remaining appliance backlog is
tracked in [Implementation Status and Loose Ends](implementation-status.md).

The collector is deliberately loopback-only. It resolves the API key from the same
named-secret sources as the C++ host, verifies the configured SHA-256 digest of the
gateway's leaf certificate immediately after the TLS handshake, and sends the
`X-API-Key` header only after that check succeeds. Redirects are not followed. A
pin mismatch sends no HTTP request and therefore does not disclose the API key.

## Provisioning

Create a scoped UniFi API key and save it as the current user's generic Windows
Credential Manager target `PixelStatus-NX/unifi-api-key`. The username is ignored;
the API key belongs in the password field. Never put the key in a command line,
configuration file, certificate file, or Git-tracked document.

Obtain the leaf-certificate SHA-256 fingerprint from a trusted local observation
and verify it against the console before accepting it. The fingerprint is not a
secret, but it is appliance-specific and normally belongs in an ignored local note
or launch command. Certificate renewal changes the fingerprint; a mismatch stops
authenticated requests until the new certificate is deliberately accepted.

Run one sanitized discovery request:

```powershell
.\tools\start-unifi-monitor.ps1 `
    -Probe `
    -BaseUrl 'https://gateway.example/proxy/network/integration' `
    -CertificateSha256 '<64 hexadecimal digits>'
```

Run the loopback collector continuously:

```powershell
.\tools\start-unifi-monitor.ps1 `
    -BaseUrl 'https://gateway.example/proxy/network/integration' `
    -CertificateSha256 '<64 hexadecimal digits>'
```

The stable output includes gateway state, model, firmware/update state, uptime,
heartbeat timestamp, CPU and memory percentages, aggregate uplink rates, physical
port counts, application version, and configured WAN names. It intentionally omits
MAC addresses, public addresses, API object IDs, and the API key.

The official local API currently identifies configured WAN interfaces but does not
report independent live health for each WAN in the tested response. PixelStatus can
combine gateway health with direct service probes and the implemented OpenWrt
Starlink-path collector. Official Starlink dish telemetry remains separate future
work.

## Desktop and ESP32 Boundary

The Python process is a host-development adapter, analogous to the Windows BLE
bridge. It uses only the Python standard library. The eventual ESP32 adapter should
make the same official requests with ESP-TLS/mbedTLS, verify an installed private CA
or explicit pin, resolve the API key from encrypted NVS, and publish the same JSON
shape to the portable monitor/evaluation layer.

References:

- [Getting Started with the Official UniFi API](https://help.ui.com/hc/en-us/articles/30076656117655-Getting-Started-with-the-Official-UniFi-API)
- [UniFi Network API](https://developer.ui.com/network/v10.0.162)
