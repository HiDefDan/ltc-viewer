const ws = new WebSocket("ws://" + window.location.host);
let inactiveSince = null;
const systemTimeWaitPeriod = 3;

ws.onmessage = (event) => {
  const data = JSON.parse(event.data);
  updateData(data);
};

// function to return system time as formatted string

function getSystemTime() {
  const now = new Date();

  const hours = now.getHours().toString().padStart(2, "0");
  const minutes = now.getMinutes().toString().padStart(2, "0");
  const seconds = now.getSeconds().toString().padStart(2, "0");

  const timeString = `${hours}.${minutes}.${seconds}`;
  return timeString;
}
// Function to show the current system time
function showSystemTime() {
  const formattedStringElem = document.getElementById("formattedString");
  formattedStringElem.textContent = getSystemTime();
  formattedStringElem.className = "system-time";

  // Set the color of all elements with class 'info' to transparent
  const infoElements = document.querySelectorAll(".info");
  infoElements.forEach((elem) => {
    elem.style.color = "transparent";
  });
}

function updateDataMembers(data) {
  // Ensure the function only runs on the intended page
  if (window.location.pathname !== "/") {
    return; // Exit if not the target page
  }

  const dataMembersDiv = document.getElementById("dataMembers");
  dataMembersDiv.innerHTML = ""; // Clear previous data

  Object.entries(data).forEach(([key, value]) => {
    // Skip the keys 'timecode', 'running', and 'debug'
    if (key === "running" || key === "debug") {
      return; // Skip this iteration
    }
    const line = document.createElement("div");
    line.textContent = `${key}: ${value}`;
    dataMembersDiv.appendChild(line);
  });
}

function updateData(data) {
  // Update data members
  updateDataMembers(data);

  if (!data.running) {
    if (inactiveSince === null) {
      inactiveSince = Date.now();
      if (performance.navigation.type === performance.navigation.TYPE_RELOAD) {
        const formattedStringElem = document.getElementById("formattedString");
        formattedStringElem.style.transition = "none";
      }
    }
  } else {
    inactiveSince = null;
  }

  const inactiveDuration = getInactiveDuration();

  if (inactiveDuration >= systemTimeWaitPeriod) {
    showSystemTime();
  }

  toggleDebugDiv(data);

  if (data.running) {
    const formattedStringElem = document.getElementById("formattedString");
    formattedStringElem.textContent = `${data.hours.toString().padStart(2, "0")}.${data.minutes.toString().padStart(2, "0")}.${data.seconds.toString().padStart(2, "0")}.${data.frames.toString().padStart(2, "0")}`;
    formattedStringElem.className = "";

    const infoElements = document.querySelectorAll(".info");
    infoElements.forEach((elem) => {
      elem.style.color = "var(--timecode-red-color)";
    });

    document.getElementById("df").textContent = data.drop_frame_format
      ? "df"
      : "";
    document.getElementById("fps").textContent = `${data.fps}`;
  }
}

function getInactiveDuration() {
  if (inactiveSince === null) {
    return 0;
  }
  return (Date.now() - inactiveSince) / 1000;
}

// Function to hide or show the div based on data.debug value
function toggleDebugDiv(data) {
  // Ensure the function only runs on the intended page
  if (window.location.pathname !== "/") {
    return; // Exit if not the target page
  }

  const debugDiv = document.getElementById("dataMembers");

  if (!data.debug) {
    // Hide the div if debug is false
    debugDiv.style.display = "none";
  } else {
    // Show the div if debug is true
    debugDiv.style.display = "block";
  }
}
