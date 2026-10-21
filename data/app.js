(function () {
  window.onerror = function (message, source, line) {
    if (window.console && console.log) console.log("ESP UI error", message, source, line);
  };
  function log() {
    if (window.console && console.log) console.log.apply(console, arguments);
  }
  function all(selector) {
    return Array.prototype.slice.call(document.querySelectorAll(selector));
  }
  function hasClass(element, name) {
    return element && (" " + element.className + " ").indexOf(" " + name + " ") >= 0;
  }
  function addClass(element, name) {
    if (element && !hasClass(element, name)) element.className += (element.className ? " " : "") + name;
  }
  function removeClass(element, name) {
    if (element) element.className = (" " + element.className + " ").replace(" " + name + " ", " ").replace(/^\s+|\s+$/g, "");
  }
  function toggleClass(element, name, on) {
    if (on) addClass(element, name);
    else removeClass(element, name);
  }
  function closest(element, selector) {
    while (element && element !== document) {
      var matcher = element.matches || element.msMatchesSelector || element.webkitMatchesSelector;
      if (matcher && matcher.call(element, selector)) return element;
      element = element.parentNode;
    }
    return null;
  }
  function readStorage(name, fallback) {
    try {
      return localStorage.getItem(name) || fallback;
    } catch (error) {
      return fallback;
    }
  }
  function writeStorage(name, value) {
    try {
      localStorage.setItem(name, value);
    } catch (error) {
    }
  }

  var root = document.documentElement;
  var media = window.matchMedia ? window.matchMedia("(prefers-color-scheme: dark)") : null;
  var scheme = readStorage("scheme", "system");
  function applyScheme(value) {
    var dark = value === "dark" || (value === "system" && media && media.matches);
    root.setAttribute("data-scheme", dark ? "dark" : "light");
  }
  var schemeSelect = document.querySelector("[data-scheme]");
  if (schemeSelect) {
    schemeSelect.value = scheme;
    schemeSelect.onchange = function () {
      writeStorage("scheme", schemeSelect.value);
      applyScheme(schemeSelect.value);
    };
  }
  applyScheme(scheme);
  if (media) {
    var onSchemeChange = function () {
      applyScheme(readStorage("scheme", "system"));
    };
    if (media.addEventListener) media.addEventListener("change", onSchemeChange);
    else if (media.addListener) media.addListener(onSchemeChange);
  }

  var tabs = all("[data-tab]");
  var panels = all("[data-panel]");
  log("ESP UI boot", "tabs", tabs.length, "panels", panels.length, "hash", location.hash);
  function hasPanel(name) {
    for (var i = 0; i < panels.length; i++) {
      if (panels[i].getAttribute("data-panel") === name) return true;
    }
    return false;
  }
  function activate(name) {
    if (!name || !hasPanel(name)) name = "system";
    log("tab activate", name);
    for (var i = 0; i < tabs.length; i++) {
      toggleClass(tabs[i], "active", tabs[i].getAttribute("data-tab") === name);
    }
    for (var j = 0; j < panels.length; j++) {
      toggleClass(panels[j], "active", panels[j].getAttribute("data-panel") === name);
    }
    if (location.hash.slice(1) !== name && history.replaceState) history.replaceState(null, "", "#" + name);
  }
  for (var t = 0; t < tabs.length; t++) {
    tabs[t].onclick = function () {
      activate(this.getAttribute("data-tab"));
    };
  }
  activate(location.hash.slice(1) || "system");
  window.onhashchange = function () {
    activate(location.hash.slice(1) || "system");
  };

  var toggles = all("[data-toggle-password]");
  for (var p = 0; p < toggles.length; p++) {
    toggles[p].onchange = function () {
      var form = closest(this, "form") || document;
      var input = form.querySelector("input[data-password]");
      if (input) input.type = this.checked ? "text" : "password";
    };
  }

  var logView = document.querySelector("[data-log-view]");
  var logStatus = document.querySelector("[data-log-status]");
  var logButton = document.querySelector("[data-refresh-logs]");
  var clearLogButton = document.querySelector("[data-clear-logs]");
  function refreshLogs() {
    if (!logView || !window.fetch) {
      if (location.hash.slice(1) === "logs") location.href = "/logs";
      return;
    }
    if (logStatus) logStatus.textContent = "\u6b63\u5728\u5237\u65b0...";
    fetch("/logs?ts=" + Date.now(), { cache: "no-store" })
      .then(function (response) { return response.text(); })
      .then(function (text) {
        logView.textContent = text || "\u6682\u65e0\u65e5\u5fd7";
        logView.scrollTop = logView.scrollHeight;
        if (logStatus) {
          var lines = text ? text.replace(/\s+$/g, "").split("\n").filter(function (line) { return line.length > 0; }).length : 0;
          logStatus.textContent = "\u5df2\u5237\u65b0\uff0c\u5171 " + lines + " \u6761";
        }
      })
      .catch(function () {
        if (logStatus) logStatus.textContent = "\u5237\u65b0\u5931\u8d25";
      });
  }
  if (logButton) logButton.onclick = refreshLogs;
  if (clearLogButton) clearLogButton.onclick = function () {
    if (!window.confirm("\u786e\u5b9a\u6e05\u9664\u5168\u90e8\u65e5\u5fd7\uff1f")) return;
    if (logStatus) logStatus.textContent = "\u6b63\u5728\u6e05\u9664...";
    fetch("/logs/clear", { method: "POST", cache: "no-store" })
      .then(function (response) {
        if (!response.ok) throw new Error("clear failed");
        logView.textContent = "\u6682\u65e0\u65e5\u5fd7";
        if (logStatus) logStatus.textContent = "\u5df2\u6e05\u9664\uff0c\u5171 0 \u6761";
      })
      .catch(function () {
        if (logStatus) logStatus.textContent = "\u6e05\u9664\u5931\u8d25";
      });
  };
  if (logView && location.hash.slice(1) === "logs") refreshLogs();

  var toast = document.createElement("div");
  toast.className = "toast";
  document.body.appendChild(toast);
  var toastTimer = 0;
  function showBusy(text) {
    toast.textContent = text;
    addClass(toast, "show");
    clearTimeout(toastTimer);
    toastTimer = setTimeout(function () {
      removeClass(toast, "show");
    }, 4500);
  }
  function markBusy(element, text) {
    if (!element || hasClass(element, "busy")) return;
    element.setAttribute("data-original-text", (element.textContent || "").replace(/^\s+|\s+$/g, ""));
    addClass(element, "busy");
    element.setAttribute("aria-busy", "true");
    element.textContent = text;
  }
  document.onclick = function (event) {
    event = event || window.event;
    var link = closest(event.target || event.srcElement, "a.btn");
    if (!link) return;
    var href = link.getAttribute("href") || "";
    if (!href || href.charAt(0) === "#" || href.indexOf("javascript:") === 0 || link.target) return;
    if ((link.getAttribute("onclick") || "").indexOf("confirm") >= 0) return;
    markBusy(link, "\u5904\u7406\u4e2d...");
    showBusy("\u5df2\u70b9\u51fb\uff0c\u6b63\u5728\u6267\u884c...");
  };
  document.onsubmit = function (event) {
    event = event || window.event;
    var form = event.target || event.srcElement;
    if (!form || hasClass(form, "is-submitting")) return;
    addClass(form, "is-submitting");
    var button = form.querySelector("button[type=submit],input[type=submit],.btn");
    markBusy(button, "\u63d0\u4ea4\u4e2d...");
    var isUpload = ((form.enctype || "").indexOf("multipart/form-data") >= 0);
    showBusy(isUpload ? "\u6b63\u5728\u4e0a\u4f20\uff0c\u8bf7\u4e0d\u8981\u65ad\u7535..." : "\u6b63\u5728\u63d0\u4ea4...");
  };

  function getJson(url, done) {
    if (!window.XMLHttpRequest) return;
    var xhr = new XMLHttpRequest();
    xhr.open("GET", url, true);
    xhr.onreadystatechange = function () {
      if (xhr.readyState !== 4 || xhr.status !== 200) return;
      try {
        done(JSON.parse(xhr.responseText));
      } catch (error) {
      }
    };
    xhr.send(null);
  }
  var learnState = document.querySelector("[data-rf-learn-state]");
  var countdown = document.querySelector("[data-rf-countdown]");
  var pending = document.querySelector("[data-rf-pending]");
  function updateRfStatus() {
    if (!learnState || !countdown || !pending) return;
    getJson("/rf-status?ts=" + Date.now(), function (data) {
      learnState.textContent = data.learning ? "\u7b49\u5f85\u63a5\u6536" : (data.ready ? "\u5df2\u63a5\u6536\uff0c\u5f85\u4fdd\u5b58" : "\u7a7a\u95f2");
      countdown.textContent = data.remaining || 0;
      pending.textContent = data.ready ? (data.value + " / " + data.bits + " bits") : "\u6682\u65e0";
    });
  }
  if (learnState) {
    updateRfStatus();
    setInterval(updateRfStatus, 1000);
  }

  var irLearnState = document.querySelector("[data-ir-learn-state]");
  var irCountdown = document.querySelector("[data-ir-countdown]");
  var irPending = document.querySelector("[data-ir-pending]");
  function updateIrStatus() {
    if (!irLearnState || !irCountdown || !irPending) return;
    getJson("/ir-status?ts=" + Date.now(), function (data) {
      irLearnState.textContent = data.learning ? "\u7b49\u5f85\u63a5\u6536" : (data.ready ? "\u5df2\u63a5\u6536\uff0c\u5f85\u4fdd\u5b58" : "\u7a7a\u95f2");
      irCountdown.textContent = data.remaining || 0;
      irPending.textContent = data.ready ? (data.length + " pulses / " + data.frequency + " kHz") : "\u6682\u65e0";
    });
  }
  if (irLearnState) {
    updateIrStatus();
    setInterval(updateIrStatus, 1000);
  }
})();
