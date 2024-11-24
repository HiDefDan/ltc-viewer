const ws = new WebSocket("ws://" + window.location.host);

// Default `ltc` state with explicit initialization
let ltc = {
  _running: false,
  _debug: false,
  _hold: 0,
  _info: false,
  frame_rate: null,
  drop_frame_format: false,
  _frame_history: [],
};

let firstRun = true;
let systemTimeInterval = null;

// WebSocket message handling
ws.onmessage = (event) => {
  const changes = JSON.parse(event.data);
  Object.assign(ltc, changes);

  updateDebugDiv(ltc);
  toggleDebugDiv(ltc);

  if (ltc._running) {
    clearSystemTime();
    displayLTC();
    updateFrameRate(ltc);
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

  timeStringElem.textContent = `${ltc.hours?.toString().padStart(2, "0") || "00"}.${
    ltc.minutes?.toString().padStart(2, "0") || "00"
  }.${ltc.seconds?.toString().padStart(2, "0") || "00"}.${
    ltc.frames?.toString().padStart(2, "0") || "00"
  }`;
  timeStringElem.className = "";
}

// Show system time
function showSystemTime() {
  const timeStringElem = document.getElementById("timeString");
  if (!timeStringElem) return;

  const update = () => {
    const now = new Date();
    timeStringElem.textContent = `${now.getHours().toString().padStart(2, "0")}.${now
      .getMinutes()
      .toString()
      .padStart(2, "0")}.${now.getSeconds().toString().padStart(2, "0")}`;
    timeStringElem.className = "system-time";
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
}

// Toggle debug div visibility
function toggleDebugDiv(data) {
  const debugDiv = document.getElementById("dataMembers");
  if (!debugDiv) return;

  debugDiv.style.display = ltc._debug ? "block" : "none";
}

// Update the debug div content
function updateDebugDiv(data) {
  const dataMembersDiv = document.getElementById("dataMembers");
  if (!dataMembersDiv) return;

  dataMembersDiv.innerHTML = "";
  Object.entries(data).forEach(([key, value]) => {
    if (key.startsWith("_")) {
      return; // Skip keys starting with an underscore
    }

    const line = document.createElement("div");
    line.textContent = `${key}: ${value}`;
    dataMembersDiv.appendChild(line);
  });
}

// Update frame rate based on frame history
function updateFrameRate(ltc) {
  if (!ltc._frame_history) ltc._frame_history = [];

  ltc._frame_history.push(ltc.frames);

  if (ltc._frame_history.length > 30) {
    ltc._frame_history.shift();
  }

  if (ltc._frame_history.length === 30) {
    const maxFrame = Math.max(...ltc._frame_history);
    let calculatedFrameRate = maxFrame + 1;

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
      console.log("Debug Mode:", ltc._debug);
    });
  }

  if (holdTime) {
    holdTime.addEventListener("input", (event) => {
      let value = event.target.value.slice(0, 2);
      holdTime.value = value;
      ltc._hold = parseInt(value, 10) || 0;
      console.log("Hold Time:", ltc._hold);
    });
  }

  if (showInfoToggle) {
    showInfoToggle.addEventListener("change", () => {
      ltc._info = showInfoToggle.checked;
      // Send the updated _info value to the WebSocket server
      ws.send(JSON.stringify({ _info: ltc._info }));
      updateInfoDisplay();
      console.log("Show Info:", ltc._info);
    });
  }

  // Initialize settings panel values
  initializeSettingsPanel();
});

// Update displayed info
function updateInfoDisplay() {
  const fpsDiv = document.getElementById("fps");
  const dfDiv = document.getElementById("df");

  if (fpsDiv && dfDiv) {
    if (ltc._info) {
      fpsDiv.textContent = ltc.frame_rate || "";
      dfDiv.textContent = ltc.drop_frame_format ? "df" : "";
    } else {
      fpsDiv.textContent = "";
      dfDiv.textContent = "";
    }
  }
}

// Initialize settings panel with current `ltc` values
function initializeSettingsPanel() {
  const debugToggle = document.getElementById("debugToggle");
  const holdTime = document.getElementById("holdTime");
  const showInfoToggle = document.getElementById("showInfoToggle");

  if (debugToggle) debugToggle.checked = ltc._debug;
  if (holdTime) holdTime.value = ltc._hold || "";
  if (showInfoToggle) showInfoToggle.checked = ltc._info;

  updateInfoDisplay();
}