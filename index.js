const { RtAudio, RtAudioFormat } = require("audify");
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
const wss = new WebSocket.Server({ server });

const frameSize = 1;

class ValueUpdater {
  constructor() {
    this.frameOffsetArr = new Int32Array(100); // Fixed-size integer array
    this.index = 0; // Tracks the current index in the circular buffer
    this.lastFrameOffsetDiff = null;
    this.previousMedianOffset = null; // Store previous median offset
  }

  update(newValue) {
    if (this.lastFrameOffsetDiff !== null) {
      const difference = Math.floor(newValue - this.lastFrameOffsetDiff); // Ensure the value is an integer
      this.frameOffsetArr[this.index] = difference; // Update current index in circular buffer
      this.index = (this.index + 1) % this.frameOffsetArr.length; // Move index and wrap around
    }
    this.lastFrameOffsetDiff = newValue;
  }

  getMedianOffset() {
    const sorted = Array.from(this.frameOffsetArr).sort((a, b) => a - b); // Convert to regular array for sorting
    const mid = Math.floor(sorted.length / 2);
    return sorted.length % 2 === 0
      ? (sorted[mid - 1] + sorted[mid]) / 2
      : sorted[mid];
  }
}

const updater = new ValueUpdater();

// Threshold for detecting sudden change
const threshold = 2; // Adjust as needed based on your data

rtAudio.openStream(
  null,
  { deviceId: 129, nChannels: 1, firstChannel: 0 },
  RtAudioFormat.RTAUDIO_SINT16,
  48000,
  frameSize,
  "MyStream",
  (pcm) => {
    decoder.write(pcm);
    let frame = decoder.read();
    if (frame !== undefined) {

      serverData.timecode = `${String(frame.hours).padStart(2, "0")}.${String(frame.minutes).padStart(2, "0")}.${String(frame.seconds).padStart(2, "0")}.${String(frame.frames).padStart(2, "0")}`;
      serverData.dropFrame = frame.drop_frame_format ? "df" : "";

      // Update the ValueUpdater with new data
      updater.update(frame.offset_start);

      // Get the current median offset
      const currentMedian = updater.getMedianOffset();

      // Check if there's a significant change in the median offset
      if (
        updater.previousMedianOffset !== null &&
        Math.abs(currentMedian - updater.previousMedianOffset) > threshold
      ) {
        serverData.fps = ""; // Clear fps value if there's a sudden change
      }

      // Update the previous median for the next comparison
      updater.previousMedianOffset = currentMedian;

      // Calculate the new fps based on the current median
      serverData.fps = getValueCategory(Math.round(currentMedian));
    }
  },
);

function getValueCategory(value) {
  if (value >= 2002 && value <= 2003) return "23.976";
  if (value >= 1999 && value <= 2001) return "24";
  if (value >= 1919 && value <= 1921) return "25";
  if (value >= 1601 && value <= 1602) return "29.97";
  if (value >= 1599 && value <= 1600) return "30";
  return "";
}

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
let serverData = {
  timecode: "", // Placeholder for formatted string
  dropFrame: "", // Placeholder for additional parameter
  fps: "",
};

rtAudio.start();

// Broadcast server data to all connected clients
function broadcastData() {
  const data = JSON.stringify(serverData);
  wss.clients.forEach((client) => {
    if (client.readyState === WebSocket.OPEN) {
      client.send(data);
    }
  });
}

// Update and broadcast the data at regular intervals
setInterval(broadcastData, 30);

wss.on("connection", (ws) => {
  console.log("Client connected");

  // Send initial data to the newly connected client
  ws.send(JSON.stringify(serverData));

  ws.on("close", () => {
    // console.log("Client disconnected");
  });
});

const port = 80;
server.listen(port, () => {
  // console.log(`Server is running on http://localhost:${port}`);
});