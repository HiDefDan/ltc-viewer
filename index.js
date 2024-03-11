const { RtAudio, RtAudioFormat } = require("audify");

// Init RtAudio instance using ALSA API
const rtAudio = new RtAudio();
const { LTCDecoder } = require("libltc-wrapper");
const decoder = new LTCDecoder(48000, 30, "s16"); // 48khz, 25 fps, signed 16 bit

const express = require("express");
const http = require("http");
const socketIO = require("socket.io");
const path = require("path");

const app = express();
const server = http.createServer(app);
const io = socketIO(server);

const frameSize = 1;

class ValueUpdater {
  constructor() {
    this.frameOffsetArr = [];
    this.lastFrameOffsetDiff = null;
    this.medianOffset = null;
  }

  update(newValue) {
    if (this.lastFrameOffsetDiff !== null) {
      const difference = newValue - this.lastFrameOffsetDiff;
      this.frameOffsetArr.push(difference);
      if (this.frameOffsetArr.length > 90) {
        this.frameOffsetArr.shift(); // Remove oldest value
      }

      const sortedValues = this.frameOffsetArr.slice().sort((a, b) => a - b);
      const mid = Math.floor(sortedValues.length / 2);

      if (sortedValues.length % 2 === 0) {
        this.medianOffset = (sortedValues[mid - 1] + sortedValues[mid]) / 2;
      } else {
        this.medianOffset = sortedValues[mid];
      }
    }
    this.lastFrameOffsetDiff = newValue;
  }

  getMedianOffset() {
    return this.medianOffset;
  }
}

const updater = new ValueUpdater();

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
      // console.log("Frame: ", frame);

      let hh = String(frame.hours).padStart(2, "0");
      let mm = String(frame.minutes).padStart(2, "0");
      let ss = String(frame.seconds).padStart(2, "0");
      let ff = String(frame.frames).padStart(2, "0");

      // process.stdout.write("\x1B[?25l"); // hide cursor
      // process.stdout.write("\x1Bc"); // clear console
      // process.stdout.write(timecode);
      serverData.timecode = `${hh}.${mm}.${ss}.${ff}`;
      serverData.dropFrame = frame.drop_frame_format ? "df" : "";
      updater.update(frame.offset_start);
      serverData.fps = getValueCategory(Math.round(updater.getMedianOffset()));
    }
  },
);

function getValueCategory(value) {
  if (value >= 2001 && value <= 2003) {
    return "23.976";
  } else if (value >= 1999 && value <= 2001) {
    return "24";
  } else if (value >= 1919 && value <= 1921) {
    return "25";
  } else if (value >= 1601 && value <= 1602) {
    return "29.97";
  } else if (value >= 1599 && value <= 1600) {
    return "30";
  } // Add more ranges as needed
  else {
    return "";
  }
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
}, 16); // Update every 16ms ( ~once per frame at 60Hz )

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
