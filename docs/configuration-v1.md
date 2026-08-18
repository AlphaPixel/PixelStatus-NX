# Configuration V1

The canonical machine-readable contract is
[`pixelstatus-config-v1.schema.json`](../schemas/pixelstatus-config-v1.schema.json).
The runtime parser additionally checks total display size, indicator bounds,
duplicate indicator IDs, and aggregate appearance durations.

## Top-Level Shape

```json
{
  "schema_version": 1,
  "display": {
    "width": 16,
    "height": 16,
    "background": "#080A10"
  },
  "statuses": {},
  "indicators": []
}
```

`statuses` must define at least `unknown` and `stale`. Status names, indicator IDs,
and source IDs contain 1-64 ASCII letters, digits, dots, underscores, or hyphens.
Each indicator names a state source and a rectangular, in-bounds display region.
Unknown reported status names render with the `unknown` appearance rather than
changing the transport contract.

Durations are integer strings using `ms`, `s`, `m`, or `h`, such as `250ms`, `2s`,
or `1m`. Colors use `#RRGGBB`.

## Appearance Grammar

Every status defines exactly one appearance form. All high-level forms compile to
the same color-keyframe timeline used by the renderer.

Solid:

```json
{"appearance": {"solid": "#00C853"}}
```

Blink, where `on` and `off` are separate intervals:

```json
{"appearance": {"blink": {"color": "#FF1744", "on": "500ms", "off": "500ms"}}}
```

Toggle between two or more colors. `period` is the duration of each color:

```json
{"appearance": {"toggle": {"colors": ["#D500F9", "#6200EA"], "period": "500ms"}}}
```

Fade from one color to the other and back over the complete period:

```json
{"appearance": {"fade": {"from": "#000000", "to": "#00A0FF", "period": "2s"}}}
```

Pulse a color between two brightness percentages. The defaults are 10% and 100%:

```json
{"appearance": {"pulse": {"color": "#FF9100", "period": "2s", "minimum": "10%", "maximum": "100%"}}}
```

A step sequence repeats by default. Set `repeat` to `false` to hold the final color:

```json
{
  "appearance": {
    "sequence": {
      "repeat": true,
      "steps": [
        {"color": "#FFFFFF", "duration": "100ms"},
        {"color": "#000000", "duration": "900ms"}
      ]
    }
  }
}
```

A cycle repeats and transitions between every adjacent pair, including the final
color back to the first. `transition` defaults to `linear` and may be `step`:

```json
{
  "appearance": {
    "cycle": {
      "transition": "linear",
      "steps": [
        {"color": "#0050C8", "duration": "1s"},
        {"color": "#40C4FF", "duration": "1s"}
      ]
    }
  }
}
```

Animation time normally starts when an indicator enters its effective status. TTL
expiration enters `stale` at the expiry instant and starts the stale appearance at
that point.

## Host Status Input

The current host endpoint accepts:

```text
GET  /api/v1/status
GET  /api/v1/status/{id}
POST /api/v1/status
POST /api/v1/status/{id}
```

All requests require `Authorization: Bearer <token>`. A collection POST requires an
`id`; an item POST takes the ID from the path and rejects a conflicting body ID.
The remaining body fields are:

```json
{
  "status": "ok",
  "value": 42,
  "message": "Optional diagnostic text",
  "ttl": 30
}
```

`status` is required. `ttl` is measured in seconds. Each successful push replaces
the complete state for that ID and resets its observation and update timestamps.
The animation epoch changes when the effective status changes, including when a
push refreshes a stale state. A GET returns both `reported_status` and the effective
`status`, which becomes `stale` once its TTL has elapsed.
