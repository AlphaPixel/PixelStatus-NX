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

It contains no sockets, operating-system APIs, threads, ESP-IDF types, or wall-clock
dependencies. Tests drive it with explicit steady-clock time points and scripted
runners, so scheduling and status transitions are deterministic on the desktop.

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

`run_due(now, maximum_runs)` executes due jobs synchronously and publishes their
states. Due jobs are ordered by due time and then monitor ID. `maximum_runs` bounds
the work performed by one pump without losing pending jobs.

If a caller pumps late, each due monitor runs once and advances to its first interval
strictly after `now`. It does not launch a burst to replay every missed interval.
This is intentional for a resource-constrained appliance: current health matters
more than recreating obsolete checks.

The current synchronous executor is the deterministic reference implementation.
A bounded host or FreeRTOS worker queue can consume the same runner jobs later;
completion still passes through the evaluator and state store contracts.

## Deliberately Deferred

This increment does not add monitor JSON syntax, concrete DNS/TCP/HTTP runners,
jitter, cron scheduling, cancellation, or a worker pool. Keeping those out until
the normalized result and evaluation behavior are stable prevents transport details
from leaking into scheduling or rendering.
