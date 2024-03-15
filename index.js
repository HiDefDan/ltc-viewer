const { SerialPort } = require("serialport");

const express = require("express");
const http = require("http");
const socketIO = require("socket.io");
const path = require("path");

const app = express();
const server = http.createServer(app);
const io = socketIO(server);

let incomingData = ""; // Buffer for incoming data

// const updater = new ValueUpdater();
// SerialPort.init(() => {
const sp = new SerialPort({ path: "/dev/ttyAMA0", baudRate: 2000000 });

sp.on("error", function (err) {
  console.log("Error: ", err.message);
});

// Switches the port into "flowing mode"
sp.on("data", function (data) {
  //process.stdout.write("\x1B[?25l"); // hide cursor
  //process.stdout.write("\x1Bc"); // clear console
  incomingData += data.toString(); // Append incoming data to buffer
  while (incomingData.includes("\n")) {
    // Process data until there are no new lines left
    const newlineIndex = incomingData.indexOf("\n"); // Find index of first newline
    const tc = incomingData.slice(0, newlineIndex); // Extract data between newlines
    // console.log("Received data:", tc); // Do something with the data

    serverData.timecode = tc;
    incomingData = incomingData.slice(newlineIndex + 1); // Remove processed data from buffer
  }
});
// });

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