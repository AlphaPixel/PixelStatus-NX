# Host Monitor Engine

The portable monitor foundation provides a complete synchronous reference path:

```text
due interval
    -> MonitorRunner
    -> MonitorResult
    -> Evaluator
    -> StateStore
    -> Renderer
```

The engine itself contains no sockets, operating-system APIs, threads, ESP-IDF
types, or wall-clock dependencies. Tests drive it with explicit steady-clock time
points and scripted runners, so scheduling and status transitions are deterministic
on the desktop. Concrete transports remain separate runner adapters.

## Runner Contract

Each concrete runner implements `MonitorRunner` and returns one normalized
`MonitorResult` containing:

- whether the transport succeeded;
- a scalar `StateValue` observation;
- elapsed latency;
- a categorized `MonitorError`;
- diagnostic detail;
- an optional observation timestamp.

Runner errors include timeout, name resolution, connection, TLS, protocol,
oversized response, invalid response, and internal failure. A runner exception is
caught at the engine boundary and normalized to an internal transport failure, so
one faulty monitor does not terminate the scheduler.

## Evaluation

An `EvaluationPolicy` defines an ordered list of rules and the named statuses used
for transport failure and no match. The first matching rule wins. A final rule with
no condition represents `otherwise`.

Supported comparisons are:

```text
exists              not_exists
equals              not_equals
contains            not_contains
greater_than        greater_or_equal
less_than           less_or_equal
between
```

Integer and floating-point observations compare numerically across their concrete
types. `between` is inclusive. Contains operations require string observations and
string operands. Invalid policies are rejected when a monitor is registered.

Transport failure bypasses value rules and uses `transport_failure_status`, which
defaults to `communication_failure`. A successful observation that matches no rule
uses `no_match_status`, which defaults to `unknown`.

## Interval Scheduling

A monitor registration supplies its ID, interval, optional state TTL, evaluation
policy, runner, and first due time. The engine supports at most 256 registrations.

`run_due(now, maximum_runs)` claims due jobs, executes the claimed work
synchronously in the calling thread, and publishes the resulting states. Due jobs
are ordered by due time and then monitor ID. `maximum_runs` bounds the work
performed by one pump without losing pending jobs. Multiple callers may pump one
engine concurrently; an in-flight claim prevents the same monitor from overlapping
itself.

If a caller pumps late, each due monitor runs once and advances to its first interval
strictly after `now`. It does not launch a burst to replay every missed interval.
This is intentional for a resource-constrained appliance: current health matters
more than recreating obsolete checks.

The synchronous pump remains the deterministic reference interface. The Win32
simulator drives it through a bounded reusable executor with two workers by default
and a configurable range of one through eight. Each worker claims at most one job
per pump. A blocked HTTP request therefore consumes one worker without blocking the
display event loop, status API, browser display, or an available monitor worker.

The host executor uses `std::jthread` stop requests for orderly ownership and joins.
An HTTP request already inside the synchronous runner cannot be interrupted, so
shutdown can wait up to that monitor's configured timeout. The future FreeRTOS
executor can reuse the same claim/evaluate/publish contract while providing its own
task and cancellation mechanisms.

## Desktop Network Adapters

The HTTP runner performs bounded GET requests and can observe the
response status code, complete body, or a scalar selected by RFC 6901 JSON Pointer.
It enforces connection/read/write/overall timeouts, disables redirects, rejects URL
credentials, and streams response data through an explicit byte limit.

The adapter intentionally distinguishes valid HTTP responses from transport errors.
For example, HTTP 503 can be evaluated as an integer observation, while an invalid
JSON document selected by a JSON monitor becomes `invalid_response` and maps through
the transport-failure status.

The TCP-connect runner resolves a configured hostname or IP address and performs a
non-blocking connection attempt against each returned address within one shared
deadline. Success observes connection latency in integer milliseconds. Resolution,
connection, and timeout errors use the same normalized `MonitorError` contract as
HTTP, so neither the evaluator nor scheduler depends on the transport type.
The shared deadline begins before resolution, but the operating system's blocking
resolver call cannot itself be cancelled by the current host executor.

The TCP-exchange runner builds on the same private socket transport. It optionally
sends a bounded text payload, reads through a configured delimiter under a bounded
response size, and exposes either the response text or total exchange latency. One
deadline covers resolution, connection, sending, and reading. Premature peer close,
timeout, and response-size exhaustion remain distinct normalized failures.

The standalone DNS runner uses the same resolver boundary with explicit IPv4,
IPv6, or combined selection. It normalizes results into a sorted unique address
list and can observe that list, its address count, or lookup latency. A result that
returns after its configured deadline is classified as a timeout; the blocking host
resolver call itself remains non-cancellable.

## Deliberately Deferred

HTTPS, custom HTTP request methods/headers/bodies, DNS record-type queries, binary or
multi-step TCP exchanges, jitter, cron scheduling, and in-flight request cancellation
remain deferred. HTTPS URLs are valid portable configuration, but the current
desktop factory rejects them at startup because this build deliberately does not
link a TLS library.
