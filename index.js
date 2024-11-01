const { RtAudio, RtAudioFormat } = require("audify");
const { LTCDecoder } = require("libltc-wrapper");
const express = require("express");
const http = require("http");
const socketIO = require("socket.io");
const path = require("path");

// Initialize RtAudio and LTCDecoder
const rtAudio = new RtAudio();
const decoder = new LTCDecoder(48000, 30, "s16"); // 48khz, 30 fps, signed 16 bit

const app = express();
const server = http.createServer(app);
const io = socketIO(server);

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

// Update the variable at regular intervals and send it to the client
setInterval(() => {
  // Send the updated variable to all connected clients
  io.emit("updateServerData", serverData);
}, 30); // Update every 16ms ( ~once per frame at 60Hz )

io.on("connection", (socket) => {
  console.log("Client connected");

  // Send the current value of the variable to the newly connected client
  socket.emit("updateServerData", { serverData });

  // Handle client disconnect
  socket.on("disconnect", () => {
    // console.log("Client disconnected");
  });
});

const port = 80;
server.listen(port, () => {
  // console.log(`Server is running on http://localhost:${port}`);
});
