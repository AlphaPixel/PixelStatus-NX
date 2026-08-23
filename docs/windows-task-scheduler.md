# Windows Published Runtime and Task Scheduler

The development tree can publish a tested, immutable runtime snapshot beneath the
current user's local application-data directory:

```text
%LOCALAPPDATA%\AlphaPixel\PixelStatus-NX\
├── current.json
├── current.previous.json
├── run-published-runtime.ps1
├── logs\
└── releases\
    └── <UTC timestamp>\
```

This deployment path is implemented and was registered, started, and health-checked
against the live browser, UniFi, OpenWrt, and MI outputs on 2026-08-22. Its optional
hardening backlog is recorded in
[Implementation Status and Loose Ends](implementation-status.md).

The scheduled task reads `current.json` through the stable bootstrap and runs only
that release. Building or editing the Git working tree therefore cannot alter or
lock the live executable. Publishing creates a new release directory and replaces
the small pointer only after every required file has been copied. The previous
pointer is retained for diagnosis or manual rollback.

The release contains the simulator executable, private operations configuration,
Python host adapters, generic launcher, and a local runtime profile. The profile
contains appliance URLs and certificate fingerprints but no API key, password,
Wi-Fi key, Bluetooth address, or other credential. UniFi and OpenWrt secrets remain
in Windows Credential Manager.

## Publish

Build and validate the repository first. Then publish the current tested snapshot:

```powershell
.\tools\publish-windows-runtime.ps1 `
    -UnifiBaseUrl 'https://gateway.example/proxy/network/integration' `
    -UnifiCertificateSha256 '<64 hexadecimal digits>' `
    -OpenWrtUrl 'https://bridge.example/ubus' `
    -OpenWrtCertificateSha256 '<64 hexadecimal digits>'
```

If the scheduled task is already running, publishing stops it before promotion and
starts it again afterward. Old release directories are retained intentionally;
publishing never recursively deletes a runtime or release directory.

## Register and Start

Register the task once and start it immediately:

```powershell
.\tools\register-windows-task.ps1 -Start
```

The task is named `AlphaPixel PixelStatus-NX`. It starts 15 seconds after the
current user logs on, runs at normal user privilege with an interactive token,
rejects concurrent instances, has no execution time limit, and retries failures up
to 20 times at one-minute intervals. Interactive execution is required for the
user's Bluetooth session and Credential Manager vault. The task does not run while
the user is logged out; the next matching logon starts it again.

## Inspect or Remove

Show the scheduled-task state, selected release, loopback endpoints, and log path:

```powershell
.\tools\get-windows-runtime-status.ps1
```

The status includes the MI bridge's atomic heartbeat: connection state, last
displayed source-frame number, and update timestamp. It deliberately omits the
Bluetooth hardware address.

Stop and unregister the task without deleting published releases:

```powershell
.\tools\unregister-windows-task.ps1
```

Runtime output is appended to
`%LOCALAPPDATA%\AlphaPixel\PixelStatus-NX\logs\runtime.log`. At startup, a log larger
than 5 MiB is moved to `runtime.previous.log`.

## Release Packaging Boundary

This per-user deployment is the appropriate development arrangement because the
MI Bluetooth path and named secrets belong to the interactive user. A future
versioned installer may place read-only binaries in Program Files, but private
configuration, credentials, mutable release selection, and logs would still belong
outside Program Files.

The present publisher intentionally does not delete old releases, automatically
roll back an unhealthy release, or bundle/install Python and Bleak. It verifies and
records the existing Python executable during publication. `current.previous.json`
makes the former selection inspectable, but rollback is manual until a bounded
rollback command is added. These are operational refinements, not blockers for the
currently running per-user development deployment.

References:

- [Microsoft logon-trigger documentation](https://learn.microsoft.com/en-us/windows/win32/taskschd/logontrigger)
- [Microsoft task logon types](https://learn.microsoft.com/en-us/windows/win32/taskschd/taskschedulerschema-logontype-principaltype-element)
- [Microsoft restart-on-failure settings](https://learn.microsoft.com/en-us/windows/win32/taskschd/taskschedulerschema-restartonfailure-settingstype-element)
