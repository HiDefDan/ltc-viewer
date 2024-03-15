const raspi = require('raspi');
const Serial = require('raspi-serial').Serial;


const express = require("express");
const http = require("http");
const socketIO = require("socket.io");
const path = require("path");

const app = express();
const server = http.createServer(app);
const io = socketIO(server);


// const updater = new ValueUpdater();
raspi.init(() => {
  var serial = new Serial('/dev/ttyAMA0', 9600);
  serial.open(() => {
    serial.on('data', (data) => {
      //process.stdout.write("\x1B[?25l"); // hide cursor
      //process.stdout.write("\x1Bc"); // clear console
      // process.stdout.write(data);
      serverData.timecode = data;
    });
    //serial.write('Hello from raspi-serial');
  });
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

// rtAudio.start();

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