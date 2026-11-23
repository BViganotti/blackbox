// Blackbox workbench bootstrap. Replaced by the full workbench as the UI
// milestones land.
(() => {
  "use strict";
  const root = document.getElementById("app");
  if (!root) return;
  const status = document.createElement("p");
  status.textContent = "Waiting for the flight recorder service…";
  root.appendChild(status);
  fetch("/api/health")
    .then((response) => response.json())
    .then((data) => {
      status.textContent = `Connected to blackbox ${data.version || ""}`;
    })
    .catch(() => {
      status.textContent = "The local server is not responding.";
    });
})();
