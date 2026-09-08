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

This deployment path is implemented and was registered, restarted, and health-checked
against the live browser, UniFi, OpenWrt, and MI outputs on 2026-08-23. Its optional
hardening backlog is recorded in
[Implementation Status and Loose Ends](implementation-status.md).
Linux uses source-tree-based systemd user units instead of this publication model;
see [Linux Host](linux-host.md).

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

For normal updates after the first publication, reuse the selected release's local
URLs, certificate pins, and MI mode without placing them on another command line:

```powershell
.\tools\update-windows-runtime.ps1
```

The one-time `-CleanLegacyProcesses` switch is for recovery from releases predating
the process manifest. It stops only a published simulator below this runtime and the
three specifically named PixelStatus Python adapters before publishing. It is not
needed for routine updates.

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
Bluetooth hardware address. A heartbeat older than 60 seconds is reported as
`stale`, even if its last reported state was `displaying`; use
`-MiHeartbeatMaximumAgeSeconds` to change that diagnostic threshold.

Stop and unregister the task without deleting published releases:

```powershell
.\tools\unregister-windows-task.ps1
```

Runtime output is appended to
`%LOCALAPPDATA%\AlphaPixel\PixelStatus-NX\logs\runtime.log`. At startup, a log larger
than 5 MiB is moved to `runtime.previous.log`.

The runtime also atomically records its simulator, UniFi, and OpenWrt child PIDs,
executable paths, and start times in `logs\runtime-processes.json`. A new task
instance validates all three fields before stopping any recorded predecessor. This
prevents both PID-reuse mistakes and orphaned servers retaining the fixed loopback
ports after a task stop, failure, publication, or restart. The BLE adapter remains
the foreground child and has its own five-second process restart loop in addition to
its internal scan/connect backoff.

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

## 2026-08-23 Recovery Acceptance

The initial long-running bridge stopped after a successful GATT write when a
status/cleanup path escaped the reconnect loop. Its stale heartbeat still said
`displaying`, and earlier task replacements had left multiple simulator/collector
children alive. The hardened release now treats heartbeat replacement and BLE close
errors as nonfatal, supervises a failed BLE process, reports stale heartbeats, and
uses the validated child-process manifest described above.

Release `20260823-164248-027` was published and the task was deliberately stopped and
started. After that independent restart, Task Scheduler reported `Running`, both
collectors were healthy, the browser sequence advanced, the MI state returned to
`displaying`, and its source-frame heartbeat was current. Exactly one simulator and
three Python adapters remained.

References:

- [Microsoft logon-trigger documentation](https://learn.microsoft.com/en-us/windows/win32/taskschd/logontrigger)
- [Microsoft task logon types](https://learn.microsoft.com/en-us/windows/win32/taskschd/taskschedulerschema-logontype-principaltype-element)
- [Microsoft restart-on-failure settings](https://learn.microsoft.com/en-us/windows/win32/taskschd/taskschedulerschema-restartonfailure-settingstype-element)
