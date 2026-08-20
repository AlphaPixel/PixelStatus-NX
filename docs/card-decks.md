# Card Decks

A card deck lets one logical display rotate through several full-screen views while
the monitor engine continues updating every source in the background. The same
rendered frames go to the Win32, browser, and eventual hardware drivers; a backend
does not need to understand cards or transitions.

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
