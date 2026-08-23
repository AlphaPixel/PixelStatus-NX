# Card Decks

A card deck lets one logical display rotate through several full-screen views while
the monitor engine continues updating every source in the background. The same
rendered frames go to the Win32, browser, and eventual hardware drivers; a backend
does not need to understand cards or transitions.

All card, transition, split, stack, and widget forms documented here are implemented
and covered by host tests. The physical MI profile may choose instant transitions
because that panel visibly refreshes at about 4 Hz; animated transitions remain
available to the browser, Win32 window, and faster future outputs. Current project
status is summarized in [Implementation Status and Loose Ends](implementation-status.md).

Use exactly one of the top-level `indicators` or `cards` arrays. A deck contains
1–32 cards. Every card requires a unique `id`, a `type`, and a `hold` duration.
The optional `transition` belongs to the outgoing card and leads to the next card,
wrapping from the last card back to the first.

```json
{
  "cards": [
    {
      "id": "clock",
      "type": "clock",
      "hold": "5s",
      "transition": {"type": "slide_left", "duration": "750ms"},
      "local_color": "#40C4FF",
      "utc_color": "#FFD600"
    }
  ]
}
```

`instant` needs no duration. Animated transitions are `fade`, `slide_left`,
`slide_right`, `slide_up`, and `slide_down`, each with a duration from 1 ms through
10 seconds. Holds range from one second through 24 hours. Deck timing uses the
portable monotonic clock, so system-clock corrections do not restart or skip cards.

## Bitmap Cards

A bitmap card maps printable one-character palette keys to colors. `pixels` must
have exactly the configured display height, and every row must have exactly the
display width. This keeps logos and other fixed artwork deterministic and makes
them easy to review in source control.

```json
{
  "id": "logo",
  "type": "bitmap",
  "hold": "3s",
  "palette": {".": "#02050A", "X": "#40C4FF"},
  "pixels": ["................", "......XX........"]
}
```

The abbreviated example above illustrates the palette only; a real 16×16 bitmap
must contain all 16 rows.

## Clock Cards

The clock uses a compact three-column-by-seven-row digit font. The top line is
local 24-hour time and the bottom line is UTC, distinguished by independently
configurable colors. The colon blinks once per second. A clock requires a display
of at least 15×15 pixels; extra space is left as the configured background color.

## Indicator Cards

An indicator card embeds the same rectangular indicators accepted by the legacy
top-level layout. Each source may appear on more than one card. Indicator IDs need
only be unique within their card, while source IDs continue to refer to monitor or
push-state IDs.

```json
{
  "id": "network",
  "type": "indicators",
  "hold": "5s",
  "indicators": [
    {"id": "lan", "source": "lan-dns", "x": 0, "y": 0, "width": 7, "height": 16},
    {"id": "wan", "source": "wan-web", "x": 9, "y": 0, "width": 7, "height": 16}
  ]
}
```

## Composite Layout Cards

A `layout` card composes different widget types in one frame. Every widget has
resolved `x`, `y`, `width`, and `height` bounds checked against the configured
display while loading the file. A card contains exactly one of an explicit `widgets`
array or a split-tree `root`. Widgets render in resolved order, so later explicit
widgets may deliberately paint over earlier ones.

The explicit form remains useful for direct placement:

```json
{
  "id": "composite-status",
  "type": "layout",
  "hold": "10s",
  "widgets": [
    {
      "id": "router",
      "type": "indicator",
      "source": "router",
      "x": 0,
      "y": 0,
      "width": 3,
      "height": 7
    },
    {
      "id": "utc",
      "type": "clock",
      "timezone": "utc",
      "color": "#FFD600",
      "x": 0,
      "y": 9,
      "width": 16,
      "height": 7
    }
  ]
}
```

Clock widget bounds must be at least 15×7 pixels. The glyph is centered when the
bounds are larger. `timezone` is `local` or `utc`; omitted colors default to blue
for local time and yellow for UTC. Widget IDs are unique within their card.

[`layout-card.example.json`](../examples/layout-card.example.json) is a runnable
16×16 explicit composite example.

### Layout trees

A layout tree uses `row` containers for left-to-right allocation and `column`
containers for top-to-bottom allocation. A container may set a non-negative `gap`.
Each child chooses either a fixed pixel `size` along the parent axis or a positive
`weight`; omitting both means `weight: 1`. Fixed children and gaps are removed first,
then weighted children share the remaining pixels. Cumulative integer division
assigns remainder pixels deterministically. Every child must receive at least one
pixel. If a container has only fixed children, their sizes plus gaps must consume
its complete axis.

Leaves omit coordinates because they inherit their resolved rectangle:

```json
{
  "id": "operations",
  "type": "layout",
  "hold": "1h",
  "root": {
    "type": "column",
    "gap": 1,
    "children": [
      {
        "type": "row",
        "weight": 1,
        "children": [
          {"id": "disk", "type": "bar", "source": "disk", "direction": "right"},
          {"id": "wan", "type": "bar", "source": "wan", "direction": "up"}
        ]
      },
      {"id": "utc", "type": "clock", "timezone": "utc", "size": 7}
    ]
  }
}
```

The parser flattens this tree into the same explicit AABBs before returning the
configuration. The renderer and every output backend remain unaware of split trees.
Nesting is limited to 16 levels, 2,048 total nodes, and 1,024 resolved widgets.

A `stack` container instead gives every child the same bounds. Children flatten in
array order and paint back-to-front: the first child is the bottom layer, and the
last is the top layer. Direct children of a stack cannot specify `size` or `weight`
because a stack performs no spatial allocation. Row and column containers can be
nested within a stack to place foreground widgets over a shared background.

```json
{
  "type": "stack",
  "children": [
    {
      "id": "overall",
      "type": "aggregate_status",
      "sources": ["router", "nas", "wan"],
      "priority": ["fail", "stale", "warn", "ok"],
      "colors": {
        "fail": "#38050F",
        "stale": "#332000",
        "warn": "#332B00",
        "ok": "#000000"
      }
    },
    {
      "id": "nodes",
      "type": "status_grid",
      "sources": ["router", "nas", "wan"],
      "columns": 3,
      "gap": 1
    }
  ]
}
```

### Layout widgets

- `indicator` fills its complete bounds with the source's effective status
  appearance.
- `clock` draws a centered 15×7 local or UTC time and therefore requires bounds of
  at least that size.
- `bar` reads a numeric source value. `minimum` and `maximum` default to 0 and 100;
  `direction` defaults to `right` and also accepts `left`, `up`, or `down`. The fill
  color is the source's status appearance and `track_color` defaults to the display
  background. Values clamp to the configured range and fill length rounds to the
  nearest pixel. Missing or non-numeric values leave only the track visible.
- `status_grid` paints its `sources` row-major across the required `columns`. Its
  optional `gap` defaults to zero. Uneven cell pixels are distributed
  deterministically, and unused cells in the last row retain the background.
- `aggregate_status` selects the first effective source status found in its
  worst-first `priority` list and fills its complete bounds with that entry from
  `colors`. Every priority entry must name a top-level configured status. Statuses
  omitted from `priority` do not affect the result. Missing
  sources act as `unknown`; omitting `unknown` is therefore useful when an
  intentionally unconfigured placeholder should not change overall card health.
  `default_color` defaults to the display background and is used when no source
  status participates. `colors` must contain exactly one entry for every status in
  `priority`; extra color entries are rejected.
- `bitmap` maps palette characters to pixels exactly within its resolved bounds;
  rows and columns must match those bounds. It does not scale artwork.

All six widgets are accepted in explicit `widgets`; layout-tree leaves use the same
fields but replace coordinates with optional `size` or `weight`.

[`split-layout.example.json`](../examples/split-layout.example.json) is the complete
16×16 storage-bar, drive-grid, dual-WAN, VPS-grid, and UTC-clock example.
[`layered-layout.example.json`](../examples/layered-layout.example.json) demonstrates
a dim worst-status background under a bright service grid and UTC clock.

## Real-Monitor Starter Deck

[`card-deck.example.json`](../examples/card-deck.example.json) is a runnable 16×16
profile with a bitmap logo, local/UTC clock, LAN/WAN card, and four-region server
card. Its harmless public example domains prove DNS, TCP, HTTP, and desktop TLS.
Replace those endpoints with your own health URLs and hostnames one at a time.

For an authenticated endpoint, use a named secret in a header, for example
`Authorization: Bearer ${secret:vps-health-token}`. Do not commit the value. Private
or self-signed appliances also need their issuing CA trusted by Windows today; the
planned ESP32 transport will use a provisioned CA bundle or a deliberately
configured certificate pin. See [Appliance Monitoring and TLS](appliance-monitoring.md).
