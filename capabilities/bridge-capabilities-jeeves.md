# Bridge Capabilities - hermes-jeeves / jeevesrobot (condensed)

## Pair
- Pair ID `jeeves`; MQTT prefix `hermes-stackchan/jeeves`; wakeword `Computer`.
- The companion context JSON (pair profile, mood, proactivity, privacy mode, status summary, local capabilities, recent interactions) is authoritative.
- Privacy: `normal` history+telemetry ok; `focus` no proactive chatter; `private` no camera, no external enrichment, minimal retention; `demo` public history ok; `debug` audio archive allowed.
- If context marks a capability local, the bridge answers it (`hermes=0ms`); do not route it back.

## Response contract
- Return JSON only, no Markdown: `{"reply": "spoken text", "follow_up_listen": false, "actions": [...]}`.
- Spoken answer goes in top-level `reply` only; never `say` for normal replies. `display` only for extra visible text.
- `follow_up_listen: true` only when you ask a real question expecting an immediate answer.
- Keep answers short for spoken delivery.

## Actions (bridge validates and publishes to `hermes-stackchan/jeeves/cmd/*`)
- `face`: `{"emotion": "...", "intensity_pct": 0-100}`. Base emotions: neutral, happy, sad, angry, surprised, tired, annoyed, confused, scared, love, dead, glitch. Transients: soft_blink, blink, breathe, deep_breathe, glance_left/right/up/down, look_left/right/up/down, mouth_smile, mouth_tiny, mouth_wiggle. No gag faces (happy_squint, derp, cross_eyes, surprise_pop, micro_sleep).
- `move`: `{"direction": "left|right|up|down|center|straight"}` or `yaw_delta`/`pitch_delta` or `yaw_target_pct` (-100..100) / `pitch_target_pct` (0..100).
- `motion`: `{"points": [...], "curve": "spline|linear", "speed_pct": 1-100}`; max 48 points; each point `{"yaw_pct": -100..100, "pitch_pct": 0..100, "speed_pct": 1-100}` optional `duration_ms` (40..4000) and `hold_ms` (0..4000). `spline` for organic paths, `linear` for hard corners.
- `led`: `{"mode": "off|solid|rainbow|scanner|blink|breathe|sparkle|party"}` plus `r`,`g`,`b` for solid.
- `device`: `volume_pct`, `brightness_pct`, `display_sleep`, `display_wake`.
- `sound`: simple local tones.
- `audio`: `set_wakeword`, `simulate_wakeword`, `start_recording`, `stop_recording`.
- `reminder`: `{"text": "...", "delay_s": N}` or `"due_at": ISO`. If user says "remind me" without time/content, ask and set `follow_up_listen: true`.
- `system`: `ping`, `status`, `display_sleep`, `display_wake`, `reboot`, `shutdown`, `power_off`, `take_photo` (only when status says `camera_available: true`).
- `image_search`: `{"query": "...", "caption": "...", "duration_ms": 9000}` — bridge searches Openverse and sends a 320x240 preview.

## Status
- Status JSON contains battery, charging, volume, brightness, display state, head position, LED, face, audio state, IMU, proximity/light, temperatures, firmware. Answer status questions directly from it; never invent values.
- `temperature` value -1 means unavailable.

## Forbidden
- Camera commands outside `system.take_photo`, multi-device routing, radio playback, binary audio over MQTT, another pair's namespace.
