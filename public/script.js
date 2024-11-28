const ws = new WebSocket("ws://" + window.location.host);

// Default `ltc` state with explicit initialization
let ltc = {
  drop_frame_format: "",
  days: "",
  months: "",
  years: "",
  hours: "",
  minutes: "",
  seconds: "",
  frames: "",
  offset_start: "",
  reverse: "",
  volume: "",
  timezone: "",
  frame_rate: null,
  _debug: "",
  _hold: "",
  _info: "",
  _frame_history: [],
  _latency_ms: null,
  _latency_frames: null,
};

let firstRun = true;
let systemTimeInterval = null;
let isSystemTimeVisible = false; // Track if system time is currently visible

// WebSocket message handling
ws.onmessage = (event) => {
  const changes = JSON.parse(event.data);
  // console.log("Received message from WebSocket:", changes); // Debug message showing received data

  // Apply the received changes to the `ltc` object
  Object.assign(ltc, changes);

  // Call initializeSettingsPanel after updating `ltc`
  initializeSettingsPanel(); // Ensure settings panel reflects changes immediately

  // Always update debug info based on the current state of _debug
  updateDebugDiv(ltc);
  toggleDebugDiv(ltc);

  if (ltc._running) {
    updateFrameRate(ltc);
    if (processChanges(changes)) {
      ltc._latency_ms = latency[1];
      ltc._latency_frames = getWsLatencyFrames(ltc._latency_ms, ltc.frame_rate);
      clearSystemTime();
      if (ltc.frame_rate) {
        displayLTC();
      }
    }
  } else if (ltc._hold > 0 || firstRun) {
    const systemTimeDisplayDelay = firstRun ? 0 : ltc._hold * 1000;
    setTimeout(showSystemTime, systemTimeDisplayDelay);
    firstRun = false;
  }

  // Update visibility of info display when _info is updated from the server
  updateInfoDisplay();
};

// Display the LTC time
function displayLTC() {
  const timeStringElem = document.getElementById("timeString");
  if (!timeStringElem) return;

  timeStringElem.textContent = adjustTimeForLatency(ltc);
  timeStringElem.className = "";

  // If _hold is 0, keep the last timecode visible
  if (ltc._hold === 0) {
    updateInfoDisplay(); // Ensure info is still shown if _info is true
  }
}

// Show system time
function showSystemTime() {
  const timeStringElem = document.getElementById("timeString");
  if (!timeStringElem) return;

  isSystemTimeVisible = true; // Set flag to true when system time is shown

  const update = () => {
    const now = new Date();
    timeStringElem.textContent = `${now.getHours().toString().padStart(2, "0")}.${now
      .getMinutes()
      .toString()
      .padStart(2, "0")}.${now.getSeconds().toString().padStart(2, "0")}`;
    timeStringElem.className = "system-time";
    updateInfoDisplay(); // Update info display when showing system time
  };

  if (!systemTimeInterval) {
    update();
    systemTimeInterval = setInterval(update, 1000);
  }
}

// Clear system time interval
function clearSystemTime() {
  if (systemTimeInterval) {
    clearInterval(systemTimeInterval);
    systemTimeInterval = null;
  }

  isSystemTimeVisible = false; // Reset flag when system time is cleared
}

// Toggle debug div visibility
function toggleDebugDiv(data) {
  const debugDiv = document.getElementById("dataMembers");
  if (!debugDiv) return;

  // Set the display based on _debug value, no need to check _running
  debugDiv.style.display = ltc._debug ? "block" : "none";
}

// Update the debug div content
function updateDebugDiv(data) {
  const dataMembersDiv = document.getElementById("dataMembers");
  if (!dataMembersDiv) return;

  dataMembersDiv.innerHTML = ""; // Clear the previous content
  Object.entries(data).forEach(([key, value]) => {
    const line = document.createElement("div");
    line.textContent = `${key}: ${value}`;
    dataMembersDiv.appendChild(line);
  });
}

// Update frame rate based on frame history
function updateFrameRate(ltc) {
  // If drop_frame_format is true, set frame_rate to 30 immediately
  if (ltc.drop_frame_format) {
    ltc.frame_rate = 30;
    return ltc.frame_rate; // Return immediately as no further calculations are needed
  }

  if (!ltc._frame_history) ltc._frame_history = [];

  // Add the current frame to the history
  ltc._frame_history.push(ltc.frames);

  // Limit the history to 30 frames
  if (ltc._frame_history.length > 30) {
    ltc._frame_history.shift();
  }

  // Calculate the frame rate only if history has 30 entries
  if (ltc._frame_history.length === 30) {
    const maxFrame = Math.max(...ltc._frame_history);
    let calculatedFrameRate = maxFrame + 1;

    // Only set the frame rate to valid values (24, 25, or 30)
    if ([24, 25, 30].includes(calculatedFrameRate)) {
      ltc.frame_rate = calculatedFrameRate;
    } else {
      ltc.frame_rate = null;
    }
  }

  return ltc.frame_rate;
}

// Attach event listeners and initialize UI
document.addEventListener("DOMContentLoaded", () => {
  // Hamburger menu and settings panel
  const hamburger = document.getElementById("hamburger");
  const settingsPanel = document.getElementById("settingsPanel");

  if (hamburger && settingsPanel) {
    hamburger.addEventListener("click", () => {
      const isActive = hamburger.classList.toggle("active");
      settingsPanel.style.display = isActive ? "block" : "none";
    });
  }

  // Settings controls
  const debugToggle = document.getElementById("debugToggle");
  const holdTime = document.getElementById("holdTime");
  const showInfoToggle = document.getElementById("showInfoToggle");

  if (debugToggle) {
    debugToggle.addEventListener("change", () => {
      ltc._debug = debugToggle.checked;
      // console.log("Debug Mode:", ltc._debug);

      // Send the updated _debug value to the WebSocket server
      ws.send(JSON.stringify({ _debug: ltc._debug }));

      // Update the debug div based on new _debug state
      updateDebugDiv(ltc);
      toggleDebugDiv(ltc);
    });
  }

  if (holdTime) {
    holdTime.addEventListener("input", (event) => {
      let value = event.target.value.slice(0, 2);
      holdTime.value = value;
      ltc._hold = parseInt(value, 10) || 0;

      // Send the updated _info value to the WebSocket server
      ws.send(JSON.stringify({ _hold: ltc._hold }));
      // console.log("Hold Time:", ltc._hold);
    });
  }

  if (showInfoToggle) {
    showInfoToggle.addEventListener("change", () => {
      ltc._info = showInfoToggle.checked;
      // Send the updated _info value to the WebSocket server
      ws.send(JSON.stringify({ _info: ltc._info }));
      updateInfoDisplay();
      // console.log("Show Info:", ltc._info);
    });
  }

  // Initialize settings panel values
  initializeSettingsPanel();
});

// Update displayed info
function updateInfoDisplay() {
  const fpsDiv = document.getElementById("fps");
  const dfDiv = document.getElementById("df");

  if (!fpsDiv || !dfDiv) return;

  // If system time is visible, hide the info
  if (isSystemTimeVisible) {
    fpsDiv.textContent = "";
    dfDiv.textContent = "";
  } else if (ltc._info) {
    fpsDiv.textContent = ltc.frame_rate || "";
    dfDiv.textContent = ltc.drop_frame_format ? "df" : "";
  } else {
    fpsDiv.textContent = "";
    dfDiv.textContent = "";
  }
}

// Initialize settings panel with current `ltc` values
function initializeSettingsPanel() {
  const debugToggle = document.getElementById("debugToggle");
  const holdTime = document.getElementById("holdTime");
  const showInfoToggle = document.getElementById("showInfoToggle");

  if (debugToggle) {
    debugToggle.checked = ltc._debug;
    // console.log("Debug:", ltc._debug); // Debugging line
  }
  if (holdTime) {
    holdTime.value = ltc._hold || "";
    // console.log("Hold Time:", ltc._hold); // Debugging line
  }
  if (showInfoToggle) {
    showInfoToggle.checked = ltc._info;
    // console.log("Info Display:", ltc._info); // Debugging line
  }

  updateInfoDisplay();
}

function getWsLatencyFrames(wsLatencyMs, frameRate) {
  if (!wsLatencyMs || !frameRate) {
    // console.log(`Websocket latency or framerate are not available`);
    return;
  }
  return Math.round(wsLatencyMs / (1000 / frameRate));
}
let timestamps = []; // Array to store the last two timestamps
let latency = [];

function processChanges(changes) {
  const currentTimestamp = changes._timestamp;

  // Add the current frame to the history
  timestamps.push(currentTimestamp);

  // Limit the history to 2 timestamps
  if (timestamps.length > 2) {
    timestamps.shift();
  }

  // add the current differnce to the differences array
  latency.push(timestamps[1] - timestamps[0]);

  // Limit the history to 2 timestamps
  if (latency.length > 2) {
    latency.shift();
  }

  const latencyPercentage = (
    (Math.abs(latency[1] - latency[0]) / latency[0]) *
    100
  ).toFixed(3);

  return latencyPercentage < 5;
}

function adjustTimeForLatency(ltc) {
  monitorFrameRate(ltc);
  // Adjust frames by adding latency frames
  ltc.frames += ltc._latency_frames;

  // If frames exceed the frame rate (should be 0 to frame_rate - 1)
  if (ltc.frames >= ltc.frame_rate) {
    ltc.frames -= ltc.frame_rate; // Reset frames to fit within frame rate
    ltc.seconds += 1; // Increment seconds
  }

  // If seconds exceed 59, increment minutes
  if (ltc.seconds >= 60) {
    ltc.seconds -= 60; // Reset seconds to 0
    ltc.minutes += 1; // Increment minutes
  }

  // If minutes exceed 59, increment hours
  if (ltc.minutes >= 60) {
    ltc.minutes -= 60; // Reset minutes to 0
    ltc.hours += 1; // Increment hours
  }

  // If hours exceed 23, reset to 0 (wrap around)
  if (ltc.hours >= 24) {
    ltc.hours = 0; // Reset hours to 0
  }

  // Format the time string
  const timeString = `${ltc.hours?.toString().padStart(2, "0") || "00"}:${
    ltc.minutes?.toString().padStart(2, "0") || "00"
  }:${ltc.seconds?.toString().padStart(2, "0") || "00"}:${
    ltc.frames?.toString().padStart(2, "0") || "00"
  }`;

  return timeString;
}

let previousFrameRate = null;

function monitorFrameRate(ltc) {
  if (ltc.frame_rate !== previousFrameRate) {
    console.warn(
      `Frame rate changed from ${previousFrameRate} to ${ltc.frame_rate}`,
    );
    previousFrameRate = ltc.frame_rate;
  }
}
