#include "web_server.h"
#include "config.h"
#include "ntp_time.h"
#include "mqtt_client.h"
#include "stm32_uart.h"
#include <WebServer.h>
#include <WiFi.h>
#include <ArduinoJson.h>

static WebServer s_server(80);
static SystemConfig *s_pcfg = nullptr;
static WebRelayToggleCallback s_relay_cb = nullptr;
static WebGoodWeToggleCallback s_goodwe_cb = nullptr;

static const char CSS_COMMON[] PROGMEM =
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;margin:0;padding:20px;background:#f0f2f5;color:#2d3748;}"
    ".container{max-width:840px;margin:0 auto;background:#fff;padding:26px;border-radius:8px;box-shadow:0 4px 6px -1px rgba(0,0,0,0.1),0 2px 4px -1px rgba(0,0,0,0.06);}"
    "h1,h2,h3{color:#1a202c;margin-top:0;}"
    ".header-bar{display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid #e2e8f0;padding-bottom:12px;margin-bottom:20px;}"
    "nav a{margin-right:18px;text-decoration:none;color:#3182ce;font-weight:600;font-size:15px;}"
    "nav a:hover{color:#2b6cb0;text-decoration:underline;}"
    ".live-tag{display:inline-flex;align-items:center;font-size:12px;font-weight:600;color:#2f855a;background:#f0fff4;border:1px solid #c6f6d5;padding:3px 9px;border-radius:12px;}"
    ".live-dot{width:8px;height:8px;background-color:#38a169;border-radius:50%;margin-right:6px;transition:opacity .3s;box-shadow:0 0 0 2px rgba(56,161,105,0.3);}"
    "table{width:100%;border-collapse:collapse;margin:14px 0 24px;}"
    "th,td{padding:10px 12px;border:1px solid #e2e8f0;text-align:left;font-size:14px;}"
    "th{background:#edf2f7;color:#4a5568;font-weight:600;}"
    ".badge-on{color:#fff;background:#38a169;padding:3px 8px;border-radius:4px;font-size:12px;font-weight:600;display:inline-block;}"
    ".badge-off{color:#fff;background:#e53e3e;padding:3px 8px;border-radius:4px;font-size:12px;font-weight:600;display:inline-block;}"
    ".btn{display:inline-block;padding:8px 16px;border:none;border-radius:4px;cursor:pointer;font-weight:600;font-size:14px;text-decoration:none;}"
    ".btn-primary{background:#3182ce;color:#fff;}"
    ".btn-primary:hover{background:#2b6cb0;}"
    ".btn-danger{background:#e53e3e;color:#fff;}"
    ".btn-danger:hover{background:#c53030;}"
    ".btn-sm{padding:4px 10px;font-size:12px;}"
    ".form-group{margin-bottom:16px;}"
    "label{display:block;margin-bottom:6px;font-weight:600;font-size:13px;color:#4a5568;}"
    "input[type=text],input[type=password],input[type=number]{width:100%;box-sizing:border-box;padding:8px 10px;border:1px solid #cbd5e0;border-radius:4px;font-size:14px;}"
    "input[type=text]:focus,input[type=password]:focus,input[type=number]:focus{border-color:#3182ce;outline:none;}"
    ".pwd-wrap{position:relative;display:flex;align-items:center;}"
    ".pwd-wrap input{padding-right:72px;}"
    ".pwd-btn{position:absolute;right:6px;padding:4px 9px;font-size:12px;font-weight:600;background:#edf2f7;border:1px solid #cbd5e0;border-radius:4px;cursor:pointer;color:#4a5568;user-select:none;}"
    ".pwd-btn:hover{background:#e2e8f0;}"
    ".switch{position:relative;display:inline-block;width:50px;height:28px;vertical-align:middle;}"
    ".switch input{opacity:0;width:0;height:0;}"
    ".slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background-color:#cbd5e0;transition:.3s;border-radius:28px;}"
    ".slider:before{position:absolute;content:'';height:22px;width:22px;left:3px;bottom:3px;background-color:#fff;transition:.3s;border-radius:50%;box-shadow:0 2px 4px rgba(0,0,0,0.2);}"
    "input:checked + .slider{background-color:#38a169;}"
    "input:checked + .slider:before{transform:translateX(22px);}"
    "input:disabled + .slider{opacity:0.5;cursor:not-allowed;}"
    ".info-card{background:#f7fafc;border:1px solid #e2e8f0;border-radius:6px;padding:12px 16px;margin-bottom:18px;font-size:13px;display:flex;justify-content:space-between;flex-wrap:wrap;gap:8px;}"
    ".footer{margin-top:24px;font-size:12px;color:#a0aec0;text-align:center;}";

static const char PAGE_OVERVIEW[] PROGMEM = R"html(<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>
<title>RESS Logger Gateway</title>
<style>
body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;margin:0;padding:20px;background:#f0f2f5;color:#2d3748;}
.container{max-width:840px;margin:0 auto;background:#fff;padding:26px;border-radius:8px;box-shadow:0 4px 6px -1px rgba(0,0,0,0.1),0 2px 4px -1px rgba(0,0,0,0.06);}
h1,h2,h3{color:#1a202c;margin-top:0;}
.header-bar{display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid #e2e8f0;padding-bottom:12px;margin-bottom:20px;}
nav a{margin-right:18px;text-decoration:none;color:#3182ce;font-weight:600;font-size:15px;}
nav a:hover{color:#2b6cb0;text-decoration:underline;}
.live-tag{display:inline-flex;align-items:center;font-size:12px;font-weight:600;color:#2f855a;background:#f0fff4;border:1px solid #c6f6d5;padding:3px 9px;border-radius:12px;}
.live-dot{width:8px;height:8px;background-color:#38a169;border-radius:50%;margin-right:6px;transition:opacity .3s;box-shadow:0 0 0 2px rgba(56,161,105,0.3);}
table{width:100%;border-collapse:collapse;margin:14px 0 24px;}
th,td{padding:10px 12px;border:1px solid #e2e8f0;text-align:left;font-size:14px;}
th{background:#edf2f7;color:#4a5568;font-weight:600;}
.badge-on{color:#fff;background:#38a169;padding:3px 8px;border-radius:4px;font-size:12px;font-weight:600;display:inline-block;}
.badge-off{color:#fff;background:#e53e3e;padding:3px 8px;border-radius:4px;font-size:12px;font-weight:600;display:inline-block;}
.switch{position:relative;display:inline-block;width:50px;height:28px;vertical-align:middle;}
.switch input{opacity:0;width:0;height:0;}
.slider{position:absolute;cursor:pointer;top:0;left:0;right:0;bottom:0;background-color:#cbd5e0;transition:.3s;border-radius:28px;}
.slider:before{position:absolute;content:'';height:22px;width:22px;left:3px;bottom:3px;background-color:#fff;transition:.3s;border-radius:50%;box-shadow:0 2px 4px rgba(0,0,0,0.2);}
input:checked + .slider{background-color:#38a169;}
input:checked + .slider:before{transform:translateX(22px);}
input:disabled + .slider{opacity:0.5;cursor:not-allowed;}
.footer{margin-top:24px;font-size:12px;color:#a0aec0;text-align:center;}
</style></head>
<body>
<div class='container'>
<div class='header-bar'>
  <h1>RESS Logger - ESP32 Gateway</h1>
  <span class='live-tag'><span id='live-dot' class='live-dot'></span>Live Stream</span>
</div>
<nav><a href='/'>Overview</a><a href='/settings'>Settings</a></nav>

<h2>System Status</h2>
<table>
  <tr><th style='width:35%;'>Parameter</th><th>Value</th></tr>
  <tr><td>Firmware Version</td><td id='val-fw'>-</td></tr>
  <tr><td>Board ID</td><td id='val-board'>-</td></tr>
  <tr><td>WiFi Network (SSID)</td><td id='val-wifi'>-</td></tr>
  <tr><td>Device IP Address</td><td id='val-ip'>-</td></tr>
  <tr><td>MQTT Broker</td><td id='val-mqtt-srv'>-</td></tr>
  <tr><td>MQTT Connection</td><td><span id='val-mqtt' class='badge-off'>CHECKING...</span></td></tr>
  <tr><td>GoodWe Acquisition</td><td><label class='switch'><input type='checkbox' id='goodwe-switch' onchange='toggleGoodWe(this.checked)'><span class='slider'></span></label> <span id='goodwe-enabled-label' class='badge-off'>OFF</span></td></tr>
     <tr>
    <td>NTP Clock Time</td>
    <td>
      <div style='display:flex;align-items:center;justify-content:space-between;flex-wrap:wrap;gap:8px;'>
        <span id='val-ntp'>-</span>
        <div>
          <button type='button' class='btn btn-primary btn-sm' id='btn-ntp-sync' onclick='triggerNtpSync()'>🔄 Sync NTP to STM32</button>
          <span id='ntp-sync-msg' style='margin-left:8px;font-size:12px;font-weight:600;'></span>
        </div>
      </div>
    </td>
  </tr>
  <tr><td>Browser Local Time</td><td id='val-browser-time'>-</td></tr>
  <tr><td>Last Telemetry Seq</td><td id='val-seq'>-</td></tr>
</table>

<h2>Relay Control (STM32 Output Pins PE7 - PE10)</h2>
<table>
  <tr><th>Channel</th><th>Pin</th><th>Status</th><th>Switch Toggle (iOS Style)</th></tr>
  <tr>
    <td>Relay 1</td><td>PE7</td>
    <td><span id='relay-label-1' class='badge-off'>OFF</span></td>
    <td><label class='switch'><input type='checkbox' id='relay-switch-1' onchange='toggleRelay(1, this.checked)'><span class='slider'></span></label></td>
  </tr>
  <tr>
    <td>Relay 2</td><td>PE8</td>
    <td><span id='relay-label-2' class='badge-off'>OFF</span></td>
    <td><label class='switch'><input type='checkbox' id='relay-switch-2' onchange='toggleRelay(2, this.checked)'><span class='slider'></span></label></td>
  </tr>
  <tr>
    <td>Relay 3</td><td>PE9</td>
    <td><span id='relay-label-3' class='badge-off'>OFF</span></td>
    <td><label class='switch'><input type='checkbox' id='relay-switch-3' onchange='toggleRelay(3, this.checked)'><span class='slider'></span></label></td>
  </tr>
  <tr>
    <td>Relay 4</td><td>PE10</td>
    <td><span id='relay-label-4' class='badge-off'>OFF</span></td>
    <td><label class='switch'><input type='checkbox' id='relay-switch-4' onchange='toggleRelay(4, this.checked)'><span class='slider'></span></label></td>
  </tr>
</table>

<h2>Live Devices Snapshot</h2>
<table>
  <tr><th style='width:25%;'>Device</th><th style='width:15%;'>Status</th><th>Key Metrics</th></tr>
  <tr>
    <td>GoodWe Inverter</td>
    <td id='goodwe-status'><span class='badge-off'>OFFLINE</span></td>
    <td id='goodwe-metrics'>Waiting for data...</td>
  </tr>
</table>

<div class='footer'>RESS Industrial Logger Gateway - Build 2026</div>
</div>

<script>
var isPolling = false;
function toggleRelay(ch, state) {
  var sw = document.getElementById('relay-switch-' + ch);
  var lbl = document.getElementById('relay-label-' + ch);
  sw.disabled = true;
  var body = 'ch=' + encodeURIComponent(ch) + '&st=' + (state ? 1 : 0);
  fetch('/relay?ajax=1', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: body
  })
  .then(function(r) {
    if (!r.ok) throw new Error('Relay toggle failed');
    sw.disabled = false;
    lbl.textContent = state ? 'ON' : 'OFF';
    lbl.className = state ? 'badge-on' : 'badge-off';
  })
  .catch(function(e) {
    sw.disabled = false;
    sw.checked = !state;
    alert('Failed to send relay command');
  });
}

function toggleGoodWe(enabled) {
  var sw = document.getElementById('goodwe-switch');
  sw.disabled = true;
  fetch('/goodwe?ajax=1', {
    method: 'POST',
    headers: {'Content-Type': 'application/x-www-form-urlencoded'},
    body: 'enabled=' + (enabled ? 1 : 0)
  })
  .then(function(r) {
    if (!r.ok) throw new Error('GoodWe toggle failed');
    sw.disabled = false;
    updateDashboard();
  })
  .catch(function() {
    sw.disabled = false;
    sw.checked = !enabled;
    alert('Failed to change GoodWe acquisition');
  });
}

function updateDashboard() {
  if (isPolling || document.hidden) return;
  isPolling = true;
  fetch('/api/status')
    .then(function(r) { return r.json(); })
    .then(function(d) {
      document.getElementById('val-fw').textContent = d.sys.fw;
      document.getElementById('val-board').textContent = d.sys.board_id;
      if (document.getElementById('val-mqtt-srv')) {
        document.getElementById('val-mqtt-srv').textContent = d.sys.mqtt_srv || '';
      }
      var wifiTxt = d.sys.wifi_ssid;
      if (d.sys.rssi) {
        var q = 'Weak';
        if (d.sys.rssi >= -55) q = 'Excellent';
        else if (d.sys.rssi >= -67) q = 'Good';
        else if (d.sys.rssi >= -78) q = 'Fair';
        wifiTxt += ' (' + d.sys.rssi + ' dBm - ' + q + ')';
      }
      document.getElementById('val-wifi').textContent = wifiTxt;
      document.getElementById('val-ip').innerHTML = '<b>' + d.sys.ip + '</b>';
      var mqttEl = document.getElementById('val-mqtt');
      if (d.sys.mqtt) {
        mqttEl.className = 'badge-on';
        mqttEl.textContent = 'CONNECTED';
      } else {
        mqttEl.className = 'badge-off';
        mqttEl.textContent = 'DISCONNECTED';
      }
      document.getElementById('val-ntp').textContent = d.sys.ntp;
      document.getElementById('val-seq').textContent = d.sys.seq;
      var gwSw = document.getElementById('goodwe-switch');
      var gwEnableLabel = document.getElementById('goodwe-enabled-label');
      if (gwSw && !gwSw.disabled) gwSw.checked = d.sys.goodwe_enabled;
      if (gwEnableLabel) {
        gwEnableLabel.textContent = d.sys.goodwe_enabled ? 'ON' : 'OFF';
        gwEnableLabel.className = d.sys.goodwe_enabled ? 'badge-on' : 'badge-off';
      }

      for (var i = 1; i <= 4; i++) {
        var rVal = d.relays['r' + i];
        var sw = document.getElementById('relay-switch-' + i);
        var lbl = document.getElementById('relay-label-' + i);
        if (sw && !sw.disabled) {
          sw.checked = (rVal == 1);
        }
        if (lbl) {
          lbl.textContent = (rVal == 1) ? 'ON' : 'OFF';
          lbl.className = (rVal == 1) ? 'badge-on' : 'badge-off';
        }
      }

      var gwStat = document.getElementById('goodwe-status');
      var gwMet = document.getElementById('goodwe-metrics');
      if (d.goodwe.online) {
        gwStat.innerHTML = "<span class='badge-on'>ONLINE</span>";
        var g = '';
        if (d.goodwe.has_mppt1_voltage) g += 'MPPT1=' + d.goodwe.mppt1_voltage.toFixed(1) + ' V, ';
        if (d.goodwe.has_pv_power_total) g += 'PV=' + d.goodwe.pv_power_total.toFixed(0) + ' W, ';
        if (d.goodwe.has_energy_total) g += 'Energy=' + d.goodwe.energy_total.toFixed(1) + ' kWh, ';
        if (d.goodwe.has_active_power) g += 'AC=' + d.goodwe.active_power.toFixed(0) + ' W, ';
        if (d.goodwe.has_inverter_temperature) g += 'Temp=' + d.goodwe.inverter_temperature.toFixed(1) + ' C, ';
        if (d.goodwe.has_working_status) g += 'Status=' + d.goodwe.working_status;
        gwMet.textContent = g;
      } else {
        gwStat.innerHTML = "<span class='badge-off'>OFFLINE</span>";
        gwMet.textContent = 'No data received';
      }

      var dot = document.getElementById('live-dot');
      if (dot) {
        dot.style.opacity = '1';
        setTimeout(function() { dot.style.opacity = '0.3'; }, 400);
      }
    })
    .catch(function(e) { console.warn('Poll error:', e); })
    .finally(function() { isPolling = false; });
}

function updateBrowserTime() {
  var el = document.getElementById('val-browser-time');
  if (el) {
    var now = new Date();
    el.textContent = now.getFullYear() + '-' +
      String(now.getMonth()+1).padStart(2,'0') + '-' +
      String(now.getDate()).padStart(2,'0') + ' ' +
      String(now.getHours()).padStart(2,'0') + ':' +
      String(now.getMinutes()).padStart(2,'0') + ':' +
      String(now.getSeconds()).padStart(2,'0');
  }
}

function triggerNtpSync() {
  var btn = document.getElementById('btn-ntp-sync');
  var msg = document.getElementById('ntp-sync-msg');
  btn.disabled = true;
  btn.textContent = '⏳ Syncing...';
  msg.style.color = '#3182ce';
  msg.textContent = 'Fetching NTP & forwarding to STM32...';

  fetch('/api/ntp-sync?epoch=' + Math.floor(Date.now() / 1000), { method: 'POST' })
    .then(function(r) { return r.json(); })
    .then(function(res) {
      btn.disabled = false;
      btn.textContent = '🔄 Sync NTP to STM32';
      if (res.status === 'ok') {
        msg.style.color = '#38a169';
        msg.textContent = '✔ ' + res.message;
        updateDashboard();
      } else {
        msg.style.color = '#e53e3e';
        msg.textContent = '✖ ' + (res.error || 'Sync failed');
      }
      setTimeout(function() { msg.textContent = ''; }, 6000);
    })
    .catch(function(err) {
      btn.disabled = false;
      btn.textContent = '🔄 Sync NTP to STM32';
      msg.style.color = '#e53e3e';
      msg.textContent = '✖ Connection error';
      setTimeout(function() { msg.textContent = ''; }, 6000);
    });
}

document.addEventListener('DOMContentLoaded', function() {
  updateBrowserTime();
  setInterval(updateBrowserTime, 1000);
  updateDashboard();
  setInterval(updateDashboard, 3000);
});
</script>
</body></html>)html";

void WebServerManager::setOnRelayToggle(WebRelayToggleCallback cb) {
    s_relay_cb = cb;
}

void WebServerManager::setOnGoodWeToggle(WebGoodWeToggleCallback cb) {
    s_goodwe_cb = cb;
}

void WebServerManager::init(SystemConfig &cfg) {
    s_pcfg = &cfg;

    s_server.on("/", HTTP_GET, handleRoot);
    s_server.on("/settings", HTTP_GET, handleSettings);
    s_server.on("/settings", HTTP_POST, handleSaveSettings);
    s_server.on("/relay", HTTP_POST, handleRelayToggle);
    s_server.on("/goodwe", HTTP_POST, handleGoodWeToggle);
    s_server.on("/restart", HTTP_POST, handleRestart);
    s_server.on("/api/status", HTTP_GET, handleApiStatus);
    s_server.on("/api/wifi-scan", HTTP_GET, handleWifiScan);
    s_server.on("/api/ntp-sync", HTTP_POST, handleNtpSync);
    s_server.onNotFound(handleNotFound);

    s_server.begin();
    Serial.println("[WEB] HTTP Server started on port 80");
}

void WebServerManager::process() {
    s_server.handleClient();
}

void WebServerManager::handleRoot() {
    // Zero-allocation instantaneous response directly from PROGMEM
    s_server.send_P(200, "text/html", PAGE_OVERVIEW);
}

void WebServerManager::handleSettings() {
    bool is_sta = (WiFi.status() == WL_CONNECTED);
    int rssi = is_sta ? WiFi.RSSI() : 0;
    String ip_str = is_sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();

    String rssi_str = is_sta ? (String(rssi) + " dBm") : "N/A";
    if (is_sta) {
        if (rssi >= -55) rssi_str += " (Excellent)";
        else if (rssi >= -67) rssi_str += " (Good)";
        else if (rssi >= -78) rssi_str += " (Fair)";
        else rssi_str += " (Weak)";
    }

    String html;
    html.reserve(4096);

    html += "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<title>Configuration - RESS Gateway</title><style>" + String(FPSTR(CSS_COMMON)) + "</style></head><body>";
    html += "<div class='container'>";
    html += "<h1>System Configuration</h1>";
    html += "<nav><a href='/'>Overview</a><a href='/settings'>Settings</a></nav>";

    // Info Card showing IP & RSSI
    html += "<div class='info-card'>";
    html += "<div><b>Device IP:</b> " + ip_str + "</div>";
    html += "<div><b>WiFi Signal:</b> " + rssi_str + "</div>";
    html += "<div><b>Current Mode:</b> " + String(is_sta ? "Station (STA)" : "Access Point (AP)") + "</div>";
    html += "</div>";

    html += "<form method='POST' action='/settings'>";

    // WiFi
    html += "<h2>WiFi Settings</h2>";
    html += "<div class='form-group'>";
    html += "<div style='display:flex;justify-content:space-between;align-items:center;margin-bottom:6px;'>";
    html += "<label style='margin:0;'>WiFi SSID</label>";
    html += "<button type='button' class='btn btn-primary btn-sm' id='btn-scan' onclick='scanWifi()'>🔍 Scan WiFi</button>";
    html += "</div>";

    // Scan Box Container
    html += "<div id='scan-box' style='display:none;margin-bottom:12px;border:1px solid #cbd5e0;border-radius:6px;padding:10px;background:#f7fafc;'>";
    html += "<div id='scan-status' style='font-size:13px;color:#4a5568;'>Scanning...</div>";
    html += "<table id='scan-table' style='margin:8px 0 0 0;font-size:12px;display:none;width:100%;'>";
    html += "<thead><tr><th>SSID</th><th>Signal</th><th>Lock</th><th>Action</th></tr></thead>";
    html += "<tbody id='scan-list'></tbody>";
    html += "</table>";
    html += "</div>";

    html += "<input type='text' id='wifi_ssid' name='wifi_ssid' value='" + String(s_pcfg->wifi_ssid) + "' required></div>";

    // WiFi Password with Show/Hide Button
    html += "<div class='form-group'><label>WiFi Password</label>";
    html += "<div class='pwd-wrap'>";
    html += "<input type='password' id='wifi_pass' name='wifi_pass' value='" + String(s_pcfg->wifi_pass) + "'>";
    html += "<button type='button' class='pwd-btn' onclick='togglePwd(\"wifi_pass\", this)'>Show</button>";
    html += "</div></div>";

    // MQTT
    html += "<h2>MQTT Broker Settings</h2>";
    html += "<div class='form-group'><label>Broker Host / IP</label><input type='text' name='mqtt_server' value='" + String(s_pcfg->mqtt_server) + "' required></div>";
    html += "<div class='form-group'><label>Broker Port</label><input type='number' name='mqtt_port' value='" + String(s_pcfg->mqtt_port) + "' required></div>";
    html += "<div class='form-group'><label>Board ID (14 Chars, e.g. RB261107RFT001)</label><input type='text' name='board_id' value='" + String(s_pcfg->board_id) + "' required></div>";
    html += "<div class='form-group'><label>MQTT Username</label><input type='text' name='mqtt_user' value='" + String(s_pcfg->mqtt_user) + "'></div>";

    // MQTT Password with Show/Hide Button
    html += "<div class='form-group'><label>MQTT Password</label>";
    html += "<div class='pwd-wrap'>";
    html += "<input type='password' id='mqtt_pass' name='mqtt_pass' value='" + String(s_pcfg->mqtt_pass) + "'>";
    html += "<button type='button' class='pwd-btn' onclick='togglePwd(\"mqtt_pass\", this)'>Show</button>";
    html += "</div></div>";

    // Intervals & Topics
    html += "<h2>Transmission & Topics</h2>";
    html += "<div class='form-group'><label>GoodWe Modbus Acquisition</label><label class='switch'><input type='checkbox' name='goodwe_enabled' value='1'";
    if (s_pcfg->goodwe_enabled) html += " checked";
    html += "><span class='slider'></span></label><div style='font-size:12px;color:#718096;margin-top:6px;'>When disabled, Modbus polling, CSV rows, and MQTT telemetry stop.</div></div>";
    html += "<div class='form-group'><label>Telemetry Send Interval (Minutes)</label><input type='number' name='send_interval' min='1' max='60' value='" + String(s_pcfg->send_interval_min) + "' required></div>";
    html += "<div class='form-group'><label>Telemetry Topic</label><input type='text' name='topic_telemetry' value='" + String(s_pcfg->topic_telemetry) + "'></div>";
    html += "<div class='form-group'><label>Status / LWT Topic</label><input type='text' name='topic_status' value='" + String(s_pcfg->topic_status) + "'></div>";
    html += "<div class='form-group'><label>Relay Command Topic</label><input type='text' name='topic_cmd' value='" + String(s_pcfg->topic_cmd) + "'></div>";

    html += "<div style='margin-top:20px;'><button class='btn btn-primary' type='submit'>Save and Reboot</button></div>";
    html += "</form>";

    html += "<hr style='margin:30px 0 20px;'>";
    html += "<form method='POST' action='/restart'><button class='btn btn-danger' type='submit'>Reboot System</button></form>";

    // Scripts for Password Toggle & WiFi Scanner
    html += "<script>\n"
            "function togglePwd(id, btn) {\n"
            "  var el = document.getElementById(id);\n"
            "  if (el.type === 'password') {\n"
            "    el.type = 'text';\n"
            "    btn.textContent = 'Hide';\n"
            "  } else {\n"
            "    el.type = 'password';\n"
            "    btn.textContent = 'Show';\n"
            "  }\n"
            "}\n"
            "function scanWifi() {\n"
            "  var btn = document.getElementById('btn-scan');\n"
            "  var box = document.getElementById('scan-box');\n"
            "  var stat = document.getElementById('scan-status');\n"
            "  var tbl = document.getElementById('scan-table');\n"
            "  var list = document.getElementById('scan-list');\n"
            "  btn.disabled = true;\n"
            "  btn.textContent = 'Scanning...';\n"
            "  box.style.display = 'block';\n"
            "  stat.style.display = 'block';\n"
            "  stat.textContent = 'Scanning available WiFi networks, please wait...';\n"
            "  tbl.style.display = 'none';\n"
            "  list.innerHTML = '';\n"
            "  fetch('/api/wifi-scan')\n"
            "    .then(function(r) {\n"
            "      if (!r.ok) throw new Error('Scan failed');\n"
            "      return r.json();\n"
            "    })\n"
            "    .then(function(nets) {\n"
            "      btn.disabled = false;\n"
            "      btn.textContent = '🔍 Scan WiFi';\n"
            "      if (!nets || nets.length === 0) {\n"
            "        stat.textContent = 'No WiFi networks found.';\n"
            "        return;\n"
            "      }\n"
            "      stat.style.display = 'none';\n"
            "      tbl.style.display = 'table';\n"
            "      nets.forEach(function(n) {\n"
            "        var tr = document.createElement('tr');\n"
            "        var q = 'Weak';\n"
            "        if (n.rssi >= -55) q = 'Excellent';\n"
            "        else if (n.rssi >= -67) q = 'Good';\n"
            "        else if (n.rssi >= -78) q = 'Fair';\n"
            "        var tdS = document.createElement('td');\n"
            "        var b = document.createElement('b');\n"
            "        b.textContent = n.ssid;\n"
            "        tdS.appendChild(b);\n"
            "        var tdR = document.createElement('td');\n"
            "        tdR.textContent = n.rssi + ' dBm (' + q + ')';\n"
            "        var tdL = document.createElement('td');\n"
            "        tdL.textContent = n.secure ? '🔒' : '🔓';\n"
            "        var tdA = document.createElement('td');\n"
            "        var btnSel = document.createElement('button');\n"
            "        btnSel.type = 'button';\n"
            "        btnSel.className = 'btn btn-primary';\n"
            "        btnSel.style.cssText = 'padding:3px 8px;font-size:12px;';\n"
            "        btnSel.textContent = 'Select';\n"
            "        btnSel.onclick = function() { selectSSID(n.ssid); };\n"
            "        tdA.appendChild(btnSel);\n"
            "        tr.appendChild(tdS);\n"
            "        tr.appendChild(tdR);\n"
            "        tr.appendChild(tdL);\n"
            "        tr.appendChild(tdA);\n"
            "        list.appendChild(tr);\n"
            "      });\n"
            "    })\n"
            "    .catch(function(e) {\n"
            "      btn.disabled = false;\n"
            "      btn.textContent = '🔍 Scan WiFi';\n"
            "      stat.textContent = 'WiFi scan error. Please try again.';\n"
            "    });\n"
            "}\n"
            "function selectSSID(ssid) {\n"
            "  document.getElementById('wifi_ssid').value = ssid;\n"
            "  var p = document.getElementById('wifi_pass');\n"
            "  if (p) p.focus();\n"
            "}\n"
            "</script>\n";

    html += "<div class='footer'>RESS Industrial Logger Gateway - Build 2026</div>";
    html += "</div></body></html>";

    s_server.send(200, "text/html", html);
}

void WebServerManager::handleSaveSettings() {
    if (s_server.hasArg("wifi_ssid")) {
        strlcpy(s_pcfg->wifi_ssid, s_server.arg("wifi_ssid").c_str(), sizeof(s_pcfg->wifi_ssid));
    }
    if (s_server.hasArg("wifi_pass")) {
        strlcpy(s_pcfg->wifi_pass, s_server.arg("wifi_pass").c_str(), sizeof(s_pcfg->wifi_pass));
    }
    if (s_server.hasArg("mqtt_server")) {
        strlcpy(s_pcfg->mqtt_server, s_server.arg("mqtt_server").c_str(), sizeof(s_pcfg->mqtt_server));
    }
    if (s_server.hasArg("mqtt_port")) {
        s_pcfg->mqtt_port = (uint16_t)s_server.arg("mqtt_port").toInt();
    }
    if (s_server.hasArg("board_id")) {
        strlcpy(s_pcfg->board_id, s_server.arg("board_id").c_str(), sizeof(s_pcfg->board_id));
    }
    if (s_server.hasArg("mqtt_user")) {
        strlcpy(s_pcfg->mqtt_user, s_server.arg("mqtt_user").c_str(), sizeof(s_pcfg->mqtt_user));
    }
    if (s_server.hasArg("mqtt_pass")) {
        strlcpy(s_pcfg->mqtt_pass, s_server.arg("mqtt_pass").c_str(), sizeof(s_pcfg->mqtt_pass));
    }
    if (s_server.hasArg("send_interval")) {
        s_pcfg->send_interval_min = (uint16_t)s_server.arg("send_interval").toInt();
        if (s_pcfg->send_interval_min == 0) s_pcfg->send_interval_min = 1;
    }
    s_pcfg->goodwe_enabled = s_server.hasArg("goodwe_enabled");
    if (s_server.hasArg("topic_telemetry")) {
        strlcpy(s_pcfg->topic_telemetry, s_server.arg("topic_telemetry").c_str(), sizeof(s_pcfg->topic_telemetry));
    }
    if (s_server.hasArg("topic_status")) {
        strlcpy(s_pcfg->topic_status, s_server.arg("topic_status").c_str(), sizeof(s_pcfg->topic_status));
    }
    if (s_server.hasArg("topic_cmd")) {
        strlcpy(s_pcfg->topic_cmd, s_server.arg("topic_cmd").c_str(), sizeof(s_pcfg->topic_cmd));
    }

    NVSConfigManager::saveConfig(*s_pcfg);

    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta http-equiv='refresh' content='8;url=/'>";
    html += "<title>Saved</title><style>" + String(FPSTR(CSS_COMMON)) + "</style></head><body>";
    html += "<div class='container'><h1>Configuration Saved</h1><p>Parameters updated in permanent storage. The gateway is restarting now. Redirecting in 8 seconds...</p></div></body></html>";
    s_server.send(200, "text/html", html);

    delay(1000);
    ESP.restart();
}

void WebServerManager::handleRelayToggle() {
    bool ok = false;
    if (s_server.hasArg("ch") && s_server.hasArg("st")) {
        uint8_t ch = (uint8_t)s_server.arg("ch").toInt();
        uint8_t st = (uint8_t)s_server.arg("st").toInt();
        if (s_relay_cb) {
            s_relay_cb(ch, st);
            ok = true;
        }
    }

    if (s_server.hasArg("ajax") || s_server.header("X-Requested-With") == "XMLHttpRequest") {
        s_server.send(ok ? 200 : 400, "application/json", ok ? "{\"status\":\"ok\"}" : "{\"error\":\"invalid params\"}");
    } else {
        s_server.sendHeader("Location", "/");
        s_server.send(303);
    }
}

void WebServerManager::handleGoodWeToggle() {
    bool ok = false;
    if (s_server.hasArg("enabled") && s_pcfg) {
        String value = s_server.arg("enabled");
        if (value == "0" || value == "1") {
            bool enabled = (value == "1");
            s_pcfg->goodwe_enabled = enabled;
            NVSConfigManager::saveConfig(*s_pcfg);
            if (s_goodwe_cb) s_goodwe_cb(enabled);
            ok = true;
        }
    }
    s_server.send(ok ? 200 : 400, "application/json",
                  ok ? "{\"status\":\"ok\"}" : "{\"error\":\"invalid params\"}");
}

void WebServerManager::handleApiStatus() {
    const TelemetrySnapshot &snap = STM32UARTBridge::getLatestSnapshot();
    const RelayState &relays = STM32UARTBridge::getLatestRelay();

    bool is_sta = (WiFi.status() == WL_CONNECTED);

    JsonDocument doc;

    // Sys
    JsonObject sys = doc["sys"].to<JsonObject>();
    sys["fw"] = FIRMWARE_VERSION;
    sys["board_id"] = s_pcfg ? s_pcfg->board_id : "";
    sys["wifi_ssid"] = is_sta ? (s_pcfg ? s_pcfg->wifi_ssid : "") : ("AP: " DEFAULT_AP_SSID);
    sys["rssi"] = is_sta ? WiFi.RSSI() : 0;
    sys["ip"] = is_sta ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
    sys["mqtt"] = MQTTManager::isConnected();
    if (s_pcfg) {
        char srvBuf[160];
        snprintf(srvBuf, sizeof(srvBuf), "%s:%u", s_pcfg->mqtt_server, s_pcfg->mqtt_port);
        sys["mqtt_srv"] = srvBuf;
    }
    sys["ntp"] = NTPTimeManager::getFormattedTime();
    sys["seq"] = snap.seq;
    sys["goodwe_enabled"] = s_pcfg ? s_pcfg->goodwe_enabled : false;

    // Relays
    JsonObject rel = doc["relays"].to<JsonObject>();
    rel["r1"] = relays.r1;
    rel["r2"] = relays.r2;
    rel["r3"] = relays.r3;
    rel["r4"] = relays.r4;
    rel["valid"] = relays.valid;

    // GoodWe
    JsonObject gw = doc["goodwe"].to<JsonObject>();
    gw["online"] = snap.goodwe.online;
    gw["has_mppt1_voltage"] = snap.goodwe.has_mppt1_voltage;
    gw["mppt1_voltage"] = snap.goodwe.mppt1_voltage;
    gw["has_pv_power_total"] = snap.goodwe.has_pv_power_total;
    gw["pv_power_total"] = snap.goodwe.pv_power_total;
    gw["has_energy_total"] = snap.goodwe.has_energy_total;
    gw["energy_total"] = snap.goodwe.energy_total;
    gw["has_active_power"] = snap.goodwe.has_active_power;
    gw["active_power"] = snap.goodwe.active_power;
    gw["has_working_status"] = snap.goodwe.has_working_status;
    gw["working_status"] = snap.goodwe.working_status;
    gw["has_inverter_temperature"] = snap.goodwe.has_inverter_temperature;
    gw["inverter_temperature"] = snap.goodwe.inverter_temperature;

    String jsonStr;
    jsonStr.reserve(1024);
    serializeJson(doc, jsonStr);
    s_server.send(200, "application/json", jsonStr);
}

void WebServerManager::handleWifiScan() {
    wifi_mode_t curMode = WiFi.getMode();
    if (curMode == WIFI_MODE_NULL || curMode == WIFI_MODE_AP) {
        WiFi.mode(WIFI_MODE_APSTA);
    }

    int n = WiFi.scanNetworks();
    if (n < 0) {
        s_server.send(500, "application/json", "[]");
        return;
    }

    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();

    for (int i = 0; i < n; ++i) {
        JsonObject item = arr.add<JsonObject>();
        item["ssid"] = WiFi.SSID(i);
        item["rssi"] = WiFi.RSSI(i);
        item["secure"] = (WiFi.encryptionType(i) != WIFI_AUTH_OPEN);
        item["channel"] = WiFi.channel(i);
    }
    WiFi.scanDelete();

    String response;
    response.reserve(1024);
    serializeJson(doc, response);
    s_server.send(200, "application/json", response);
}

void WebServerManager::handleRestart() {
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><meta http-equiv='refresh' content='8;url=/'>";
    html += "<title>Rebooting</title><style>" + String(FPSTR(CSS_COMMON)) + "</style></head><body>";
    html += "<div class='container'><h1>Rebooting Gateway...</h1><p>Device is restarting. Redirecting in 8 seconds...</p></div></body></html>";
    s_server.send(200, "text/html", html);

    delay(1000);
    ESP.restart();
}

void WebServerManager::handleNtpSync() {
    bool synced = false;

    if (WiFi.status() == WL_CONNECTED) {
        NTPTimeManager::forceSync();

        // Wait up to 3 seconds for network time
        uint32_t start_w = millis();
        while (millis() - start_w < 3000) {
            if (NTPTimeManager::isTimeSynchronized()) {
                synced = true;
                break;
            }
            delay(100);
        }
    } else {
        synced = NTPTimeManager::isTimeSynchronized();
    }

    // Last resort (no internet / AP mode): use the clock of the browser.
    if (!synced && s_server.hasArg("epoch")) {
        uint32_t epoch = (uint32_t)strtoul(s_server.arg("epoch").c_str(), nullptr, 10);
        synced = NTPTimeManager::setFromEpoch(epoch, "browser");
    }

    if (!synced) {
        s_server.send(504, "application/json", "{\"status\":\"error\",\"error\":\"No time source available (NTP unreachable)\"}");
        return;
    }

    bool stm_ok = NTPTimeManager::syncToSTM32();

    JsonDocument doc;
    doc["status"] = "ok";
    doc["time"] = NTPTimeManager::getFormattedTime();
    doc["epoch"] = NTPTimeManager::getEpochSeconds();
    doc["source"] = NTPTimeManager::getSource();
    doc["stm32_synced"] = stm_ok;
    doc["message"] = stm_ok ? "Synchronized with NTP and dispatched to STM32!"
                            : "NTP time acquired, but STM32 UART link was busy.";

    String out;
    serializeJson(doc, out);
    s_server.send(200, "application/json", out);
}

void WebServerManager::handleNotFound() {
    s_server.send(404, "text/plain", "Page Not Found");
}
