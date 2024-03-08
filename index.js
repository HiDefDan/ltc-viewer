const { RtAudio, RtAudioFormat } = require("audify");

// Init RtAudio instance using ALSA API
const rtAudio = new RtAudio();
const { LTCDecoder } = require("libltc-wrapper");
const decoder = new LTCDecoder(48000, 25, "s16"); // 48khz, 25 fps, unsigned 8 bit

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
    this.values = [];
    this.lastValue = null;
    this.medianValue = null;
  }

  update(newValue) {
    if (this.lastValue !== null) {
      const difference = newValue - this.lastValue;
      this.values.push(difference);
      if (this.values.length > 90) {
        this.values.shift(); // Remove oldest value
      }

      const sortedValues = this.values.slice().sort((a, b) => a - b);
      const mid = Math.floor(sortedValues.length / 2);

      if (sortedValues.length % 2 === 0) {
        this.medianValue = (sortedValues[mid - 1] + sortedValues[mid]) / 2;
      } else {
        this.medianValue = sortedValues[mid];
      }
    }
    this.lastValue = newValue;
  }

  getMedianValue() {
    return this.medianValue;
  }
}

// Example usage
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
      //console.log("Frame: ", frame);
      let hh = padZero(frame.hours);
      let mm = padZero(frame.minutes);
      let ss = padZero(frame.seconds);
      let ff = padZero(frame.frames);

      let timecode = `${hh}.${mm}.${ss}.${ff}`;
      // process.stdout.write("\x1B[?25l"); // hide cursor
      // process.stdout.write("\x1Bc"); // clear console
      // process.stdout.write(timecode);
      serverData.timecode = timecode;
      serverData.dropFrame = frame.drop_frame_format ? "df" : "";

      updater.update(frame.offset_start);
      serverData.fps = getValueCategory(Math.round(updater.getMedianValue()));
    }
    // else {
    //   // const currentTime = new Date();
    //   // // // console.log('frame is undefined')
    //   // let hh = padZero(currentTime.getHours());
    //   // let mm = padZero(currentTime.getMinutes());
    //   // let ss = padZero(currentTime.getSeconds());
    //   // let ff =padZero(Math.floor(currentTime.getMilliseconds() / 40));

    //   // let timeOfDay = `${hh}.${mm}.${ss}.${ff}`;
    //   // // process.stdout.write("\x1B[?25l"); // hide cursor
    //   // // process.stdout.write("\x1Bc"); // clear console
    //   // // process.stdout.write(timecode);
    //   // serverData.timecode = timeOfDay;
    //   // serverData.dropFrame = "ToD";

    //   // // updater.update(frame.offset_start);
    //   // serverData.fps = "25";
    // }
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

// Helper function to pad zeros for single-digit values
function padZero(value) {
  return value < 10 ? `0${value}` : `${value}`;
}

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
    console.log("Client disconnected");
  });
});

const port = 80;
server.listen(port, () => {
  console.log(`Server is running on http://localhost:${port}`);
});
