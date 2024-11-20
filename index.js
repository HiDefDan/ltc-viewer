const { RtAudio, RtAudioFormat, RtAudioStreamFlags } = require("audify");
const { LTCDecoder } = require("libltc-wrapper");
const express = require("express");
const http = require("http");
const WebSocket = require("ws"); // Import WebSocket library
const path = require("path");

// Initialize RtAudio and LTCDecoder
const rtAudio = new RtAudio();
const decoder = new LTCDecoder(48000, 30, "s16"); // 48khz, 30 fps, signed 16 bit

const app = express();
const server = http.createServer(app);
const ws = new WebSocket.Server({ server });

const frameSize = 1;

// Variable to track when the offset changes
let lastOffsetChangeTime = Date.now(); // Store the last time offset_start changed
let lastOffsetStart = null; // Store the last value of offset_start

class FramerateCalculator {
  constructor(sampleRate) {
    this.sampleRate = sampleRate; // e.g., 48000 Hz
    this.previousOffset = null;
    this.offsetDifferences = [];
    this.maxFrames = 100; // Track the last 100 offset differences
  }

  update(offset) {
    if (this.previousOffset !== null) {
      const difference = offset - this.previousOffset;
      this.offsetDifferences.push(difference);

      if (this.offsetDifferences.length > this.maxFrames) {
        this.offsetDifferences.shift();
      }
    }
    this.previousOffset = offset;
  }

  getMostFrequentOffset() {
    if (this.offsetDifferences.length === 0) return null;

    // Count occurrences of each offset difference
    const counts = {};
    this.offsetDifferences.forEach((diff) => {
      counts[diff] = (counts[diff] || 0) + 1;
    });

    // Find the most frequent offset difference
    let mostFrequentOffset = null;
    let maxCount = 0;
    for (const [diff, count] of Object.entries(counts)) {
      if (count > maxCount) {
        maxCount = count;
        mostFrequentOffset = parseInt(diff, 10);
      }
    }
    return mostFrequentOffset;
  }

  getFramerate() {
    const mostFrequentOffset = this.getMostFrequentOffset();
    return mostFrequentOffset
      ? (this.sampleRate / mostFrequentOffset).toFixed(3)
      : null;
  }
}

const framerateCalculator = new FramerateCalculator(48000);
let lastFrameTime = Date.now(); // Track the last time we received a valid frame
let lastLoggedTime = Date.now();
rtAudio.openStream(
  null,
  { deviceId: 129, nChannels: 1, firstChannel: 0 },
  RtAudioFormat.RTAUDIO_SINT16,
  48000,
  frameSize,
  "LTC",
  (pcm) => {
    try {
      decoder.write(pcm);
      let frame = decoder.read();

      if (frame !== undefined) {
        // console.log(frame);
        // Reset the lastFrameTime when a valid frame is received
        lastFrameTime = Date.now();

        framerateCalculator.update(frame.offset_start);
        const currentFramerate = Math.round(framerateCalculator.getFramerate());

        // Update server data
        ltc.days = frame.days;
        ltc.months = frame.months;
        ltc.years = frame.years;
        ltc.drop_frame_format = frame.drop_frame_format;
        ltc.hours = frame.hours;
        ltc.minutes = frame.minutes;
        ltc.seconds = frame.seconds;
        ltc.frames = frame.frames;
        ltc.offset_start = frame.offset_start;
        ltc.reverse = frame.reverse;
        ltc.volume = frame.volume;
        ltc.timezone = frame.timezone;
        ltc.fps = currentFramerate ? currentFramerate : "";
        ltc.running = true;
      } else {
        // If frame is undefined, check how long it's been
        const currentTime = Date.now();
        if (currentTime - lastFrameTime > 100) {
          // If more than 100ms have passed since the last frame
          ltc.running = false;
        }
      }
    } catch (error) {
      console.error("Error processing frame:", error);
    }
  },
  {
    flags:
      RtAudioStreamFlags.RTAUDIO_SCHEDULE_REALTIME &
      RtAudioStreamFlags.RTAUDIO_MINIMIZE_LATENCY,
  },
);
// Set a timeout to continue working
setTimeout(() => {
  try {
    rtAudio.write(null);
  } catch {
    // console.log("RTAudio fixed, enjoy your stream.");
  }
});

// Serve static files from the 'public' directory
app.use(express.static(path.join(__dirname, "public")));

app.get("/", (req, res) => {
  res.sendFile(__dirname + "/public/index.html");
});

app.get("/displays", (req, res) => {
  res.sendFile(__dirname + "/public/displays.html");
});
// Define a variable to send to the client
let ltc = {
  drop_frame_format: "",
  days: "",
  months: "",
  years: "",
  timecode: "",
  hours: "",
  minutes: "",
  seconds: "",
  frames: "",
  offset_start: "",
  reverse: "",
  volume: "",
  timezone: "",
  fps: "",
  running: "",
  debug: false,
};

rtAudio.start();

// Broadcast server data to all connected clients
function broadcastData() {
  const data = JSON.stringify(ltc);
  ws.clients.forEach((client) => {
    if (client.readyState === WebSocket.OPEN) {
      client.send(data);
    }
  });
}

// Update and broadcast the data at regular intervals
setInterval(broadcastData, 30);

ws.on("connection", (ws) => {
  // console.log("Client connected");

  // Send initial data to the newly connected client
  ws.send(JSON.stringify(ltc));

  ws.on("message", (message) => {
    try {
      // Try to parse the message as JSON
      const parsedMessage = JSON.parse(message.toString());

      // If JSON is valid, log the parsed object
      console.log("Received valid JSON:", parsedMessage);

      // Optionally, send a response back
      // ws.send("Server received valid JSON.");
      if (parsedMessage.hasOwnProperty("debug")) {
        // console.log(parsedMessage.debug);
        ltc.debug = parsedMessage.debug;
      }
    } catch (error) {
      // If JSON is invalid, log the raw message as a string
      console.log("Received non-JSON message:", message.toString());

      // Optionally, send a response back for non-JSON
      // ws.send("Server received non-JSON message.");
    }
  });

  ws.on("close", () => {
    // console.log("Client disconnected");
  });
});

const port = 80;
server.listen(port, () => {
  // console.log(`Server is running on http://localhost:${port}`);
});
