# Architecture & Implementation Guide

This document explains the technical design of the LTC timecode reader and provides implementation roadmap.

## System Architecture

```
GPIO Input (LTC bitstream)
    ↓
[GPIO Edge Capture Thread]  — DMA timestamped edges (pigpio)
    ↓
[LTC Bi-Phase Decoder]      — 80-bit frame extraction, sync word detection
    ↓
[Time of Day / Offset]      — system time + LTC frame offset
    ↓
[Font Rasterizer]           — bitmap glyph rendering to ARGB8888 buffer
    ↓
[DRM/KMS Page Flip @vblank] — drmModeSetCrtc() → display scanout
    ↓
HDMI Output (1920×1080 @ 50/60 Hz)
```

## Component Details

### 1. GPIO Edge Capture (`src/gpio.c`)

**Current Status**: Stub (pigpio integration ready)

**Design**:
- Captures GPIO 17 (BCM) edges with microsecond precision
- Uses pigpio DMA timestamping for jitter-free edge logging
- Runs in background thread; edges queued to LTC decoder

**Implementation Tasks**:
```c
int gpio_init(gpio_context_t *ctx, int bcm_pin) {
    // TODO: lgpio_start() or gpioInitialise()
    // Set pin as input, enable edge detection
    // Register callback for rising/falling edges
}

int gpio_wait_edge(gpio_context_t *ctx, uint64_t *timestamp_us, int *level) {
    // TODO: Poll or sleep-wait for queued edge
    // Return: timestamp (µs), level (0 or 1)
    // Block until next edge available
}
```

**Expected Timing**:
- GPIO edge latency: ~5–10 µs (pigpio DMA)
- Queue depth: ~100 edges (8 bytes each)

### 2. LTC Decoder (`src/ltc.c`)

**Current Status**: Stub (frame processing hooks ready)

**Design**:
- Bi-phase Manchester decoding of 80-bit LTC frames
- Accumulates bits from GPIO edges and checks sync words
- Extracts HH:MM:SS:FF from 32-bit payload

**LTC Frame Format** (80 bits total):
```
Bits 0–63:   User data (currently unused) + timecode
  Bits 17–20:  Frame units (0–9)
  Bits 21–24:  Frame tens (0–3)
  Bits 25–32:  Seconds units/tens
  Bits 33–40:  Minutes units/tens
  Bits 41–48:  Hours units/tens
Bits 64–79:   Sync word (0x3FFC or 0xBFFC for drop-frame)
```

**Implementation Tasks**:
```c
int ltc_feed_edge(ltc_decoder_t *decoder, uint64_t edge_time_us, int level) {
    // Measure time since last edge
    uint64_t bit_period = 1600000 / 25; // µs for 25 fps
    uint64_t delta = edge_time_us - decoder->last_edge_time_us;
    
    // Bi-phase decode:
    // - 1 transition per bit = 0
    // - 2 transitions per bit = 1
    
    // Accumulate into 80-bit shift register
    // When bit_count == 80, validate sync word and extract timecode
}
```

**Expected Latency**: ~2–5 ms (CPU decoding, 80 bits @ ~100 µs per bit)

### 3. Bitmap Font Rendering (`src/font.c`)

**Current Status**: 5×7 monospace stub (pre-rasterization needed)

**Design**:
- Pre-rasterized monospace glyphs, 1-bit per pixel
- Each glyph stored as rows of packed bytes
- Blitted to ARGB8888 framebuffer with alpha blending

**Glyph Storage**:
```c
typedef struct {
    uint32_t glyph_width;      // e.g., 16 pixels (for large display)
    uint32_t glyph_height;     // e.g., 64 pixels (~6% of 1080p)
    uint32_t pitch;            // bytes per row
    const uint8_t *bitmap;     // packed bitmap data
} font_atlas_t;
```

**Implementation Tasks**:

1. **Generate font atlas** using freetype2 or ImageMagick:
```bash
# Example: rasterize Courier New at 64px to bitmap
convert -font "Courier-New" -pointsize 64 \
    -background black -fill white \
    label:"0123456789:" /tmp/font_atlas.png
```

2. **Convert PNG to C array**:
```bash
xxd -i /tmp/font_atlas.png > src/font_atlas.h
```

3. **Embed in `src/font.c`**:
```c
const uint8_t font_bitmap[] = {
    0xFF, 0xFF, ... // glyph bitmap data
};
```

**Expected Rendering Time**: ~1–2 ms (CPU blitting, ~12 glyphs + colons)

### 4. DRM/KMS Page Flipping (`src/drm.c`)

**Current Status**: Complete (libdrm integration)

**Design**:
- Opens DRM device (`/dev/dri/card0`)
- Allocates 2 dumb buffers (CPU-rendered ARGB8888)
- Sets CRTC mode (1920×1080 @ 50/60 Hz)
- Synchronous page flip on vblank

**Key Functions**:
```c
int drm_init(drm_context_t *ctx, uint32_t target_vrefresh);
    // Opens device, finds HDMI, allocates buffers, sets mode

uint8_t *drm_get_back_buffer(drm_context_t *ctx);
    // Returns pointer to back buffer for rendering

int drm_page_flip_sync(drm_context_t *ctx);
    // Updates CRTC to display back buffer
    // Blocks until vblank (deterministic timing)
    // Swaps front/back pointers
```

**Expected Latency**:
- Render to back buffer: ~1–2 ms (CPU bitmap blit)
- drmModeSetCrtc() to scanout: ~1–3 ms (vblank wait)
- **Total**: ~3–5 ms from render start to pixels on screen

### 5. Main Render Loop (`src/main.c`)

**Current Status**: Complete (functional with stubs)

**Design**:
- 50/60 Hz loop (tied to display refresh rate)
- Poll GPIO for LTC edges (triggers LTC decoder)
- Render timecode to back buffer
- Vblank-synced page flip

**Pseudocode**:
```c
while (!should_exit) {
    // 1. Poll GPIO for edge
    if (gpio_wait_edge(...)) {
        ltc_feed_edge(...);  // Accumulate bits
    }

    // 2. Get back buffer
    uint8_t *fb = drm_get_back_buffer(&drm);
    memset(fb, 0, drm.back.buffer_size);  // Clear to black

    // 3. Get current timecode
    ltc_frame_t frame;
    if (ltc_get_frame(&ltc, &frame) == 0) {
        ltc_frame_to_string(&frame, buf);
    } else {
        // Fallback to system time
        ltc_frame_to_string_with_tz(...);
    }

    // 4. Render timecode
    font_blit_string(fb, ..., buf, COLOR_WHITE, COLOR_BLACK);

    // 5. Page flip (blocks until vblank)
    drm_page_flip_sync(&drm);

    // 6. Yield to GPIO capture
    nanosleep(..., NULL);
}
```

**Frame Budget** (at 50 Hz = 20 ms per frame):
- GPIO polling: <1 ms
- LTC decoding: ~2–5 ms (when frame complete)
- Font rendering: ~1–2 ms
- DRM page flip: ~1–3 ms (vblank wait)
- Idle time: ~10–15 ms (safe)

## Integration Checklist

### Phase 1: GPIO + LTC Decoding (This Session)
- [x] Project structure scaffolded
- [x] DRM/KMS framebuffer system (complete)
- [ ] GPIO edge capture (pigpio stub → real implementation)
- [ ] LTC bi-phase decoder (stub → full state machine)
- [ ] Font atlas generation and embedding
- [ ] Field test on Pi5 with real LTC signal

### Phase 2: Real-Time Optimization (Optional)
- [ ] CPU isolation (isolcpus kernel param)
- [ ] SCHED_FIFO scheduling
- [ ] Latency profiling with perf/trace-cmd
- [ ] Verify <5 ms end-to-end latency

### Phase 3: Advanced Features (Future)
- [ ] Drop-frame timecode detection
- [ ] User bits display (8-bit field)
- [ ] Color-code based on sync lock
- [ ] SMPTE-2022-6 timecode over IP
- [ ] Timestamp offset calibration UI

## Testing Strategy

### Unit Tests
```bash
# Test LTC frame decoding (mock GPIO edges)
cd tests/
gcc -std=c99 -I../include test_ltc_decoder.c ../src/ltc.c -o test_ltc
./test_ltc
```

### Integration Tests
```bash
# On Pi5 with display:
# 1. Boot into graphical mode (disable LTC service)
# 2. Generate test LTC signal (software GPIO clock + signal generator)
# 3. Run ltc-timecode, verify display output
# 4. Measure latency with high-speed camera or oscilloscope probe
```

### Performance Profiling
```bash
# Measure CPU time and latency
sudo perf record -e cycles,instructions,cache-misses ltc-timecode
sudo perf report

# Trace vblank synchronization
sudo trace-cmd record -e drm ltc-timecode
trace-cmd report
```

## References

- IEEE 1119-1988: Linear Timecode (LTC) standard
- [libdrm Documentation](https://dri.freedesktop.org/docs/drm/)
- [pigpio Library](http://abyz.me.uk/rpi/pigpio/) (for GPIO edge timestamps)
- [Bi-phase/Manchester Decoding](https://en.wikipedia.org/wiki/Manchester_code)
