const ws = new WebSocket("ws://" + window.location.host);
let ltc = { running: false }; // Default `ltc` state
let firstRun = true;
let systemTimeInterval = null;

// WebSocket message handling
ws.onmessage = (event) => {
  const changes = JSON.parse(event.data);
  Object.assign(ltc, changes);

  updateDebugDiv(ltc);
  toggleDebugDiv(ltc);

  if (ltc.running) {
    displayLTC();

    console.log(updateFrameRate(ltc));
    clearSystemTime(); // Stop showing the system time
  } else if (ltc.hold > 0 || firstRun) {
    // Only show system time if `ltc.hold` is greater than 0 or it’s the first run
    const systemTimeDisplayDelay = firstRun ? 0 : ltc.hold * 1000;
    setTimeout(showSystemTime, systemTimeDisplayDelay);
    firstRun = false; // Set `firstRun` to false after the first run
  }
};

// Display the LTC time
function displayLTC() {
  const timeStringElem = document.getElementById("timeString");
  if (!timeStringElem) return; // Exit if element doesn't exist

  timeStringElem.textContent = `${ltc.hours?.toString().padStart(2, "0") || "00"}.${
    ltc.minutes?.toString().padStart(2, "0") || "00"
  }.${ltc.seconds?.toString().padStart(2, "0") || "00"}.${
    ltc.frames?.toString().padStart(2, "0") || "00"
  }`;
  timeStringElem.className = ""; // Remove any specific styles
}

function showSystemTime() {
  const timeStringElem = document.getElementById("timeString");
  if (!timeStringElem) return; // Exit if the element doesn't exist

  const update = () => {
    const now = new Date();
    timeStringElem.textContent = `${now.getHours().toString().padStart(2, "0")}.${now
      .getMinutes()
      .toString()
      .padStart(2, "0")}.${now.getSeconds().toString().padStart(2, "0")}`;
    timeStringElem.className = "system-time"; // Add a class for styling
  };

  if (!systemTimeInterval) {
    update(); // Immediate update
    systemTimeInterval = setInterval(update, 1000); // Update every second
  }
}

// Clear the system time interval
function clearSystemTime() {
  if (systemTimeInterval) {
    clearInterval(systemTimeInterval); // Stop the interval
    systemTimeInterval = null; // Reset the interval variable
  }
}

// Function to hide or show the debug div based on `ltc.debug` value
function toggleDebugDiv(data) {
  // Ensure the function only runs on the intended page
  if (window.location.pathname !== "/") return;

  const debugDiv = document.getElementById("dataMembers");
  debugDiv.style.display = ltc.debug ? "block" : "none";
}

// Function to update the debug div content
function updateDebugDiv(data) {
  // Ensure the function only runs on the intended page
  if (window.location.pathname !== "/") return;

  const dataMembersDiv = document.getElementById("dataMembers");
  dataMembersDiv.innerHTML = ""; // Clear previous data

  Object.entries(data).forEach(([key, value]) => {
    const line = document.createElement("div");
    line.textContent = `${key}: ${value}`;
    dataMembersDiv.appendChild(line);
  });
}

function updateFrameRate(ltc) {
  // Initialize or reset the rolling array to track up to 60 frames
  if (!ltc._frameHistory) ltc._frameHistory = [];

  // Add the current frame to the history
  ltc._frameHistory.push(ltc.frames);

  // Limit history to 60 frames (so we can check max after 60 frames)
  if (ltc._frameHistory.length > 30) {
    ltc._frameHistory.shift(); // Remove the oldest frame when we have more than 60
  }

  // After 60 frames, calculate the max value and return max + 1 (if valid)
  if (ltc._frameHistory.length === 30) {
    const maxFrame = Math.max(...ltc._frameHistory);
    let calculatedFrameRate = maxFrame + 1;

    // Only return valid frame rates: 24, 25, or 30
    if (calculatedFrameRate === 24 || calculatedFrameRate === 25 || calculatedFrameRate === 30) {
      ltc.frame_rate = calculatedFrameRate;
    } else {
      // Return 30 if the calculated frame rate is not 24, 25, or 30
      ltc.frame_rate = null;
    }
  }

  return ltc.frame_rate;
}