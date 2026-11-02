/* SPDX-License-Identifier: MIT
 * Xbox presentation wrapper for the separately licensed Boxedwine browser shell.
 * The Wine desktop itself is rendered by the emulator, not by this script.
 */
(function () {
  "use strict";
  if (new URLSearchParams(window.location.search).get("embed") !== "true") return;
  var style = document.createElement("style");
  style.textContent =
    "html,body{margin:0;width:100%;height:100%;overflow:hidden;background:#000;color:#fff;font-family:Segoe UI,Arial,sans-serif}" +
    "#app,#dropzone{position:absolute;inset:0;margin:0;padding:0}" +
    ".emscripten_border{border:0;position:absolute;inset:0;display:flex;align-items:center;justify-content:center}" +
    "#canvas{margin:0;padding:0;border:0;display:block;flex:none}" +
    "#controls,#console,#app>hr{display:none!important}" +
    "body.xbox-diagnostics #controls{display:block!important;position:fixed;top:36px;left:8px;right:8px;z-index:30;background:#222;color:#fff;padding:8px}" +
    "body.xbox-diagnostics #console{display:block!important;position:fixed;left:8px;right:8px;bottom:8px;height:25vh;z-index:30}" +
    "body.xbox-diagnostics #output{display:block!important;box-sizing:border-box;width:100%;height:100%;background:#111;color:#eee;border:1px solid #555}" +
    "#loading{position:fixed;inset:0;z-index:40;background:#182533;display:flex;flex-direction:column;align-items:center;justify-content:center}" +
    "#xbox-wine-starting{position:fixed;left:50%;top:50%;transform:translate(-50%,-50%);padding:16px 24px;border-radius:6px;background:#182533;z-index:20;font-size:15px}" +
    "#xbox-wine-tools{position:fixed;right:5px;top:5px;z-index:50;width:30px;height:26px;border:1px solid #617080;border-radius:4px;background:#182533cc;color:#fff;font-size:20px;line-height:16px;cursor:pointer}" +
    "#xbox-wine-error{display:none;position:fixed;left:12%;right:12%;top:25%;padding:24px;background:#3d1620;border:1px solid #ec7385;border-radius:8px;z-index:60;white-space:pre-wrap;overflow-wrap:anywhere}";
  document.head.appendChild(style);
  var canvas = document.getElementById("canvas");
  function resize() {
    var width = canvas.width || 1280, height = canvas.height || 720;
    var scale = Math.min(window.innerWidth / width, window.innerHeight / height);
    canvas.style.width = Math.floor(width * scale) + "px";
    canvas.style.height = Math.floor(height * scale) + "px";
  }
  window.addEventListener("resize", resize);
  new MutationObserver(resize).observe(canvas, { attributes: true, attributeFilter: ["width", "height"] });
  resize();
  var startup = document.createElement("div");
  startup.id = "xbox-wine-starting";
  startup.textContent = "Starting Wine desktop…";
  startup.style.display = "none";
  document.body.appendChild(startup);
  var error = document.createElement("div");
  error.id = "xbox-wine-error";
  error.setAttribute("role", "alert");
  document.body.appendChild(error);
  function showError(reason) {
    error.textContent = "Wine could not continue.\n\n" + String(reason) + "\n\nOpen ⋯ for diagnostics.";
    error.style.display = "block";
    startup.style.display = "none";
    document.getElementById("loading").style.display = "none";
  }
  var tools = document.createElement("button");
  tools.id = "xbox-wine-tools";
  tools.type = "button";
  tools.textContent = "⋯";
  tools.title = "Wine diagnostics (Ctrl+Shift+D)";
  tools.setAttribute("aria-label", "Wine diagnostics");
  function toggleDiagnostics() {
    document.body.classList.toggle("xbox-diagnostics");
    tools.setAttribute("aria-expanded", document.body.classList.contains("xbox-diagnostics") ? "true" : "false");
  }
  tools.addEventListener("click", toggleDiagnostics);
  document.body.appendChild(tools);
  window.addEventListener("keydown", function (event) {
    if (event.ctrlKey && event.shiftKey && event.key.toLowerCase() === "d") {
      event.preventDefault();
      toggleDiagnostics();
    }
  });
  var oldPrint = Module.print;
  Module.print = function () {
    var line = Array.prototype.join.call(arguments, " ");
    if (oldPrint) oldPrint.apply(Module, arguments);
    if (line.indexOf("Launching ") >= 0) startup.style.display = "block";
    // This acknowledges the actual SDL surface, not Explorer/game compatibility.
    if (line.indexOf("Showing Window") >= 0) startup.style.display = "none";
  };
  var oldAbort = Module.onAbort;
  Module.onAbort = function (reason) {
    showError(reason);
    if (oldAbort) oldAbort(reason);
  };
  window.addEventListener("error", function (event) {
    if (event.message) showError(event.message);
    else if (event.target && event.target.tagName === "SCRIPT") showError("A Wine runtime script failed to load.");
  }, true);
  window.addEventListener("unhandledrejection", function (event) { showError(event.reason); });
})();
