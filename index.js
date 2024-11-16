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
const wseconds = new WebSocket.Server({ server });

const frameSize = 1;

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

rtAudio.openStream(
  null,
  { deviceId: 129, nChannels: 1, firstChannel: 0 },
  RtAudioFormat.RTAUDIO_SINT16,
  48000,
  frameSize,
  "LTC",
  (pcm) => {
    decoder.write(pcm);
    let frame = decoder.read();
    if (frame !== undefined) {
      // console.log(frame);

      framerateCalculator.update(frame.offset_start);
      const currentFramerate = Math.round(framerateCalculator.getFramerate());

      // Update server data
      ltc.days = frame.days;
      ltc.months = frame.months;
      ltc.years = frame.years;
      ltc.drop_frame_format = frame.drop_frame_format;
      ltc.hours = `${String(frame.hours).padStart(2, "0")}`;
      ltc.minutes = `${String(frame.minutes).padStart(2, "0")}`;
      ltc.seconds = `${String(frame.seconds).padStart(2, "0")}`;
      ltc.frames = `${String(frame.frames).padStart(2, "0")}`;
      ltc.timecode = `${String(frame.hours).padStart(2, "0")}.${String(frame.minutes).padStart(2, "0")}.${String(frame.seconds).padStart(2, "0")}.${String(frame.frames).padStart(2, "0")}`;
      ltc.offset_start = frame.offset_start;
      ltc.reverse = frame.reverse;
      ltc.volume = frame.volume;
      ltc.timezone = frame.timezone;
      ltc.fps = currentFramerate ? currentFramerate : "";
    }
  },
  { flags: RtAudioStreamFlags.RTAUDIO_MINIMIZE_LATENCY },
);

// function getValueCategory(value) {
//   // console.log(value);
//   if (value >= 1999 && value <= 2001) return "23.976";
//   if (value >= 1997 && value <= 1998) return "24";
//   if (value >= 1917 && value <= 1918) return "25";
//   if (value >= 1599 && value <= 1600) return "29.97";
//   if (value >= 1598 && value <= 1599) return "30";
//   return "";
// }

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
};

rtAudio.start();

// Broadcast server data to all connected clients
function broadcastData() {
  const data = JSON.stringify(ltc);
  wseconds.clients.forEach((client) => {
    if (client.readyState === WebSocket.OPEN) {
      client.send(data);
    }
  });
}

// Update and broadcast the data at regular intervals
setInterval(broadcastData, 30);

wseconds.on("connection", (ws) => {
  // console.log("Client connected");

  // Send initial data to the newly connected client
  ws.send(JSON.stringify(ltc));

  ws.on("close", () => {
    // console.log("Client disconnected");
  });
});

const port = 80;
server.listen(port, () => {
  // console.log(`Server is running on http://localhost:${port}`);
});
