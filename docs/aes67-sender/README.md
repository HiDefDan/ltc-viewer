# AES67 Sender Reference

Working notes describing the **other** box in the two-unit AES67 timecode
setup — a Pi 5 (hostname `aes67`) acting as PTP grandmaster and AES67 LTC
transmitter. Kept here because they are the authoritative record of what the
sender is configured to emit; the decoder side must match.

These are session notes, not polished documentation, and they are written from
the sender's point of view. Read them in order:

1. **`timecode-decoder-prompt.md`** — the original specification: PTP domain,
   grandmaster identity, multicast address, stream format (`L24/48000/1`,
   PT 127, 1 ms packet time), and the SDP. Still the reference for stream
   parameters.
2. **`timecode-decoder-diagnostic-update.md`** — the packet-loss investigation
   that eventually identified the LAN switch as the cause.
3. **`timecode-unicast-switch-update.md`** — the switch to unicast, including
   the exact sender-side config change and how to revert it.

## Current state (2026-08-09)

- Stream: **unicast** to the decoder's address on port 5004, `L24/48000/1`,
  PT 127, 1 ms packet time, 25 fps LTC generated in **UTC** (deliberate — it
  avoids a timecode jump at DST boundaries, so expect the display to differ
  from local wall clock outside winter).
- The sender's multicast config is commented out rather than deleted, so
  reverting is a two-line edit — see the unicast update doc.
- Both boxes hit the same `SIOCSHWTSTAMP` boot race in `ptp4l`; both are now
  fixed, by different means (this repo's decoder uses a `phydev` wait, see
  `PRODUCTION_IMAGE_GUIDE.md` §5f; the sender uses a `network-online.target`
  dependency).

Anything the decoder needs to *do* about this lives in `README.md`
(application) and `PRODUCTION_IMAGE_GUIDE.md` §5f (system config).
