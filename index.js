const { RtAudio, RtAudioFormat } = require("audify");

// Init RtAudio instance using ALSA API
const rtAudio = new RtAudio();
const { LTCDecoder } = require("libltc-wrapper");
const decoder = new LTCDecoder(48000, 25, "s16"); // 48khz, 25 fps, unsigned 8 bit

const express = require('express');
const http = require('http');
const socketIO = require('socket.io');
const path = require('path'); 

const app = express();
const server = http.createServer(app);
const io = socketIO(server);


const frameSize = 0;


rtAudio.openStream(
  null,
  { deviceId: 129, nChannels: 1, firstChannel: 0 },
  RtAudioFormat.RTAUDIO_SINT16,
  48000,
  frameSize,
  "MyStream",
  (pcm) => {
    // console.log("pcm: ", pcm);
    // process.stdout.write("pcm: " + pcm);
    decoder.write(pcm);
    let frame = decoder.read();
    // console.log("Frame: ", frame);
    if (frame !== undefined) {
      // console.log("Frame: ", frame);
      let hh = padZero(frame.hours);
      let mm = padZero(frame.minutes);
      let ss = padZero(frame.seconds);
      let ff = padZero(frame.frames);
      let framesDelim = frame.drop_frame_format ? ";" : ":";

      let formattedString = `${hh}:${mm}:${ss}${framesDelim}${ff}`;
      // console.clear();
      // process.stdout.write("\x1B[?25l"); // hide cursor
      // process.stdout.write("\x1Bc"); // clear console
      // process.stdout.write(formattedString);
      serverData = formattedString;

    }
  },
);

// rtAudio.start();

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
app.use(express.static(path.join(__dirname, 'public')));

app.get('/', (req, res) => {
  res.sendFile(__dirname + '/public/index.html');
});

// Define a variable to send to the client
let serverData;

// Update the variable at regular intervals and send it to the client
setInterval(() => {
  // serverData++; // Update the variable (you can replace this with any logic)
  rtAudio.start();

  // Send the updated variable to all connected clients
  io.emit('updateServerData', serverData);
}, 1); // Update every 20ms (adjust as needed)

io.on('connection', (socket) => {
  console.log('Client connected');

  // Send the current value of the variable to the newly connected client
  socket.emit('updateServerData', serverData);

  // Handle client disconnect
  socket.on('disconnect', () => {
    console.log('Client disconnected');
  });
});

const port = 3000;
server.listen(port, () => {
  console.log(`Server is running on http://localhost:${port}`);
});