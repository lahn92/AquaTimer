#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include "time.h"

const char *ap_ssid = "AquaTimerAP";
const char *ap_password = "123456789";

// PWM Configuration
const int PWM_PIN = 2;
const int PWM_CHANNEL = 0;
const int PWM_FREQ = 5000;     // 5 kHz
const int PWM_RESOLUTION = 12; // 12-bit resolution (0-4095)
float currentDutyPWM = 0.0;    // actual PWM applied
float fadeStep = 0.2;          // max change per update in percent (adjust for smoothness)

Preferences preferences;
WebServer server(80);

String sta_ssid;
String sta_pass;
int timezoneOffset = 0;

// Schedule data
struct SchedulePoint
{
  float time; // Time in hours (0-24)
  int duty;   // Duty cycle 0-100%
};

std::vector<SchedulePoint> schedulePoints;
unsigned long lastNTPSync = 0;
const unsigned long NTP_SYNC_INTERVAL = 3600000; // 1 hour in milliseconds

void setupPWM()
{
  ledcSetup(PWM_CHANNEL, PWM_FREQ, PWM_RESOLUTION);
  ledcAttachPin(PWM_PIN, PWM_CHANNEL);
  ledcWrite(PWM_CHANNEL, 0);
  Serial.println("PWM initialized on pin 2");
}

void setPWMDuty(float dutyPercent)
{
  // Clamp duty cycle to 0-100%
  dutyPercent = constrain(dutyPercent, 0.0, 100.0);

  // Convert percentage to 12-bit value (0-4095) with high precision
  int pwmValue = (int)((dutyPercent / 100.0) * 4095.0 + 0.5);
  ledcWrite(PWM_CHANNEL, pwmValue);

  Serial.print("PWM set to ");
  Serial.print(dutyPercent, 2);
  Serial.print("% (");
  Serial.print(pwmValue);
  Serial.println("/4095)");
}

float getCurrentTimeInHours()
{
  time_t now = time(nullptr);
  now += timezoneOffset * 3600;
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  float hours = timeinfo.tm_hour + (timeinfo.tm_min / 60.0) + (timeinfo.tm_sec / 3600.0);
  return hours;
}

float calculateCurrentDuty()
{
  if (schedulePoints.size() == 0)
  {
    return 0.0; // No schedule, lights off
  }

  float currentTime = getCurrentTimeInHours();

  // Sort schedule points by time
  std::sort(schedulePoints.begin(), schedulePoints.end(),
            [](const SchedulePoint &a, const SchedulePoint &b)
            { return a.time < b.time; });

  // Find the two points to interpolate between
  SchedulePoint before = {0, 0};
  SchedulePoint after = {24, 0};
  bool foundBefore = false;
  bool foundAfter = false;

  for (size_t i = 0; i < schedulePoints.size(); i++)
  {
    if (schedulePoints[i].time <= currentTime)
    {
      before = schedulePoints[i];
      foundBefore = true;
    }
    if (schedulePoints[i].time >= currentTime && !foundAfter)
    {
      after = schedulePoints[i];
      foundAfter = true;
    }
  }

  // Handle edge cases
  if (!foundBefore && !foundAfter)
  {
    return 0.0; // No points at all
  }

  if (!foundBefore)
  {
    // Before first point - use 0 to first point interpolation
    before = {0, 0};
    after = schedulePoints[0];
  }

  if (!foundAfter)
  {
    // After last point - use last point to end of day
    before = schedulePoints[schedulePoints.size() - 1];
    after = {24, 0};
  }

  // Linear interpolation with floating point precision
  if (before.time == after.time)
  {
    return (float)before.duty;
  }

  float timeDiff = after.time - before.time;
  float timeFromBefore = currentTime - before.time;
  float ratio = timeFromBefore / timeDiff;

  // High precision linear interpolation
  float duty = (float)before.duty + ((float)after.duty - (float)before.duty) * ratio;

  return constrain(duty, 0.0, 100.0);
}

void updatePWMFromSchedule()
{
  float targetDuty = calculateCurrentDuty();

  // Smooth fade
  if (abs(targetDuty - currentDutyPWM) <= fadeStep)
  {
    currentDutyPWM = targetDuty; // close enough
  }
  else if (targetDuty > currentDutyPWM)
  {
    currentDutyPWM += fadeStep;
  }
  else
  {
    currentDutyPWM -= fadeStep;
  }

  setPWMDuty(currentDutyPWM);
}

void loadScheduleFromPreferences()
{
  preferences.begin("schedule", true);
  String pointsJson = preferences.getString("points", "[]");
  preferences.end();

  schedulePoints.clear();

  StaticJsonDocument<2048> doc;
  DeserializationError error = deserializeJson(doc, pointsJson);

  if (error)
  {
    Serial.print("Failed to parse schedule: ");
    Serial.println(error.c_str());
    return;
  }

  JsonArray array = doc.as<JsonArray>();
  for (JsonObject obj : array)
  {
    String timeStr = obj["time"].as<String>();
    int duty = obj["duty"].as<int>();

    // Parse time string "HH:MM"
    int colonPos = timeStr.indexOf(':');
    if (colonPos > 0)
    {
      int hours = timeStr.substring(0, colonPos).toInt();
      int minutes = timeStr.substring(colonPos + 1).toInt();
      float timeInHours = hours + (minutes / 60.0);

      schedulePoints.push_back({timeInHours, duty});
    }
  }

  Serial.print("Loaded ");
  Serial.print(schedulePoints.size());
  Serial.println(" schedule points");
}

void setupTime()
{
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Waiting for NTP time sync...");
  time_t now = time(nullptr);
  int retry = 0;
  while (now < 8 * 3600 * 2 && retry < 10)
  {
    delay(1000);
    Serial.print(".");
    now = time(nullptr);
    retry++;
  }
  Serial.println(" done.");
  lastNTPSync = millis();
}

void syncTimeIfNeeded()
{
  if (millis() - lastNTPSync >= NTP_SYNC_INTERVAL)
  {
    Serial.println("Periodic NTP sync...");
    setupTime();
  }
}

String getFormattedTime()
{
  time_t now = time(nullptr);
  now += timezoneOffset * 3600;
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[64];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

String generateNetworkList()
{
  String html = "<form method='POST' action='/save'>";
  html += "<label for='ssid'>WiFi Network:</label><br>";
  html += "<select name='ssid'>";
  int n = WiFi.scanNetworks();
  if (n == 0)
    html += "<option>No networks found</option>";
  else
  {
    for (int i = 0; i < n; ++i)
    {
      html += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + " (" + String(WiFi.RSSI(i)) + "dBm)</option>";
    }
  }
  html += "</select><br><br>";
  html += "<label for='password'>Password:</label><br>";
  html += "<input name='password' type='password'><br><br>";
  html += "<input type='submit' value='Save'>";
  html += "</form>";
  return html;
}

void handleRoot()
{
  server.send(200, "text/html", generateNetworkList());
}

void handleSave()
{
  if (server.hasArg("ssid") && server.hasArg("password"))
  {
    sta_ssid = server.arg("ssid");
    sta_pass = server.arg("password");

    preferences.begin("wifi", false);
    preferences.putString("ssid", sta_ssid);
    preferences.putString("password", sta_pass);
    preferences.end();

    String html = "<h2>WiFi credentials saved.</h2><p>Rebooting...</p>";
    server.send(200, "text/html", html);

    delay(2000);
    ESP.restart();
  }
  else
  {
    server.send(400, "text/plain", "Missing SSID or Password");
  }
}

void handleSetTimezone()
{
  if (server.hasArg("offset"))
  {
    timezoneOffset = server.arg("offset").toInt();
    preferences.begin("settings", false);
    preferences.putInt("timezone", timezoneOffset);
    preferences.end();
    server.sendHeader("Location", "/");
    server.send(303);
  }
  else
  {
    server.send(400, "text/plain", "Missing timezone offset");
  }
}

void handleSaveSchedule()
{
  if (server.hasArg("schedule"))
  {
    preferences.begin("schedule", false);
    preferences.putString("points", server.arg("schedule"));
    preferences.end();

    // Reload schedule and update PWM immediately
    loadScheduleFromPreferences();
    updatePWMFromSchedule();

    server.send(200, "text/plain", "Schedule saved");
  }
  else
  {
    server.send(400, "text/plain", "Missing schedule data");
  }
}

void handleLoadSchedule()
{
  preferences.begin("schedule", true);
  String points = preferences.getString("points", "[]");
  preferences.end();
  server.send(200, "application/json", points);
}

void handleStatus()
{
  float currentDuty = calculateCurrentDuty();
  float currentTime = getCurrentTimeInHours();

  StaticJsonDocument<256> doc;
  doc["currentTime"] = getFormattedTime();
  doc["currentTimeHours"] = currentTime;
  doc["currentDuty"] = currentDuty;
  doc["schedulePoints"] = schedulePoints.size();

  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleMain()
{
  String currentTime = getFormattedTime();
  float currentDuty = calculateCurrentDuty();

  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>AquaTimer Light Schedule</title>
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    body { font-family: Arial, sans-serif; background: #f8fcff; margin: 20px; text-align: center; }
    table { margin: 0 auto; border-collapse: collapse; }
    td, th { padding: 6px 10px; }
    input[type="time"], input[type="number"] { width: 100px; padding: 4px; border-radius: 4px; border: 1px solid #ccc; }
    button {
      background-color: #007bff;
      color: white;
      border: none;
      border-radius: 6px;
      padding: 8px 14px;
      margin: 6px;
      font-size: 14px;
      cursor: pointer;
    }
    button:hover { background-color: #0056b3; }
    .chart-wrap { max-width: 600px; width: 100%; margin: 20px auto 0; position: relative; }
    .chart-wrap::before { content: ""; display: block; padding-top: 50%; }
    .chart-wrap canvas { position: absolute !important; top: 0; left: 0; width: 100% !important; height: 100% !important; }
    .status { background: #e7f3ff; border: 2px solid #007bff; border-radius: 8px; padding: 15px; margin: 20px auto; max-width: 400px; }
    .status h3 { margin: 0 0 10px 0; }
    .duty-display { font-size: 2em; font-weight: bold; color: #007bff; }
  </style>
</head>
<body>
  <h1>AquaTimer Light Schedule</h1>
  
  <div class="status">
    <h3>Current Status</h3>
    <p>Time: <span id="currentTime">)rawliteral" +
                currentTime + R"rawliteral(</span></p>
    <p>Light Duty: <span class="duty-display" id="currentDuty">)rawliteral" +
                String(currentDuty) + R"rawliteral(%</span></p>
  </div>

  <form action='/settimezone' method='POST'>
    <label>Timezone:</label>
    <select name='offset'>
      <option value='-12'>UTC-12</option>
      <option value='-11'>UTC-11</option>
      <option value='-10'>UTC-10</option>
      <option value='-9'>UTC-9</option>
      <option value='-8'>UTC-8</option>
      <option value='-7'>UTC-7</option>
      <option value='-6'>UTC-6</option>
      <option value='-5'>UTC-5</option>
      <option value='-4'>UTC-4</option>
      <option value='-3'>UTC-3</option>
      <option value='-2'>UTC-2</option>
      <option value='-1'>UTC-1</option>
      <option value='0'>UTC</option>
      <option value='1'>UTC+1</option>
      <option value='2'>UTC+2</option>
      <option value='3'>UTC+3</option>
      <option value='4'>UTC+4</option>
      <option value='5'>UTC+5</option>
      <option value='6'>UTC+6</option>
      <option value='7'>UTC+7</option>
      <option value='8'>UTC+8</option>
      <option value='9'>UTC+9</option>
      <option value='10'>UTC+10</option>
      <option value='11'>UTC+11</option>
      <option value='12'>UTC+12</option>
    </select>
    <input type='submit' value='Set Timezone'>
  </form>

  <h3>Schedule Points</h3>
  <table id="pointsTable" border="1">
    <tr><th>Time (24h)</th><th>Duty (%)</th><th>Actions</th></tr>
  </table>

  <button onclick="addPoint()">Add Point</button>
  <br>
  <button onclick="saveSchedule()">Save Schedule</button>
  <button onclick="loadSchedule()">Load Schedule</button>

  <div class="chart-wrap">
    <canvas id="lightChart"></canvas>
  </div>

  <script>
    let points = [];
    const currentOffset = )rawliteral" +
                String(timezoneOffset) + R"rawliteral(;

    window.onload = () => {
      const select = document.querySelector('select[name="offset"]');
      if (select) select.value = currentOffset;
      updateStatus();
    };

    function updateStatus() {
      fetch('/status')
        .then(r => r.json())
        .then(data => {
          document.getElementById('currentTime').textContent = data.currentTime;
          document.getElementById('currentDuty').textContent = data.currentDuty.toFixed(2) + '%';
        });
    }

    setInterval(updateStatus, 5000); // Update every 5 seconds

    function renderTable() {
      const table = document.getElementById('pointsTable');
      table.innerHTML = '<tr><th>Time (24h)</th><th>Duty (%)</th><th>Actions</th></tr>';
      points.forEach((p, i) => {
        const row = table.insertRow();
        row.insertCell(0).innerHTML = '<input type="time" value="' + p.time + '" onchange="updatePoint(' + i + ', this.value, null)">';
        row.insertCell(1).innerHTML = '<input type="number" min="0" max="100" value="' + p.duty + '" onchange="updatePoint(' + i + ', null, this.value)">';
        row.insertCell(2).innerHTML = '<button onclick="deletePoint(' + i + ')">Delete</button>';
      });
      updateChart();
    }

    function addPoint() {
      points.push({ time: "12:00", duty: 50 });
      renderTable();
    }

    function deletePoint(index) {
      points.splice(index, 1);
      renderTable();
    }

    function updatePoint(index, time, duty) {
      if (time !== null) points[index].time = time;
      if (duty !== null) points[index].duty = duty;
      updateChart();
    }

    const currentTimeLine = {
      id: 'currentTimeLine',
      afterDraw(chart) {
        const now = new Date();
        const hours = now.getUTCHours() + currentOffset;
        const adjustedHours = (hours + 24) % 24 + now.getUTCMinutes() / 60;

        const xScale = chart.scales.x;
        const ctx = chart.ctx;
        const x = xScale.getPixelForValue(adjustedHours);

        ctx.save();
        ctx.beginPath();
        ctx.moveTo(x, chart.chartArea.top);
        ctx.lineTo(x, chart.chartArea.bottom);
        ctx.lineWidth = 2;
        ctx.strokeStyle = 'red';
        ctx.stroke();
        ctx.restore();
      }
    };

    const ctx = document.getElementById('lightChart').getContext('2d');
    const chart = new Chart(ctx, {
      type: 'line',
      data: { datasets: [{
        label: 'Duty Cycle (%)',
        data: [],
        borderColor: 'rgb(0,150,255)',
        backgroundColor: 'rgba(0,150,255,0.1)',
        tension: 0
      }] },
      options: {
        responsive: true,
        maintainAspectRatio: false,
        scales: {
          x: { type: 'linear', min: 0, max: 24, title: { display: true, text: 'Time (hours)' } },
          y: { min: 0, max: 100, title: { display: true, text: 'Duty Cycle (%)' } }
        },
        plugins: { legend: { display: false } }
      },
      plugins: [currentTimeLine]
    });

    function updateChart() {
      let fullData = [{ x: 0, y: 0 }, { x: 24, y: 0 }];
      points.forEach(p => {
        const [h, m] = p.time.split(':').map(Number);
        const x = h + m / 60;
        fullData.push({ x: x, y: parseInt(p.duty) });
      });
      fullData.sort((a, b) => a.x - b.x);
      chart.data.datasets[0].data = fullData;
      chart.update();
    }

    setInterval(() => chart.update(), 60000);

    function saveSchedule() {
      fetch('/saveschedule', {
        method: 'POST',
        headers: { 'Content-Type': 'application/x-www-form-urlencoded' },
        body: 'schedule=' + encodeURIComponent(JSON.stringify(points))
      }).then(r => r.text()).then(alert);
    }

    function loadSchedule() {
      fetch('/loadschedule')
        .then(r => r.json())
        .then(data => { points = data; renderTable(); });
    }

    loadSchedule();
  </script>
</body>
</html>
)rawliteral";

  server.send(200, "text/html", html);
}

void startAPMode()
{
  Serial.println("Starting Access Point...");
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  Serial.print("AP IP address: ");
  Serial.println(WiFi.softAPIP());

  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.begin();
  Serial.println("Connect to 'AquaTimerAP' and open http://192.168.4.1/");
}

void startSTAMode()
{
  preferences.begin("wifi", true);
  sta_ssid = preferences.getString("ssid", "");
  sta_pass = preferences.getString("password", "");
  preferences.end();

  if (sta_ssid == "")
  {
    Serial.println("No saved WiFi credentials, starting AP mode");
    startAPMode();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(sta_ssid.c_str(), sta_pass.c_str());
  Serial.print("Connecting to ");
  Serial.println(sta_ssid);
  unsigned long startAttemptTime = millis();

  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 15000)
  {
    delay(500);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("\nConnected!");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    if (MDNS.begin("aquatimer"))
    {
      Serial.println("MDNS responder started: http://aquatimer.local/");
    }

    preferences.begin("settings", true);
    timezoneOffset = preferences.getInt("timezone", 0);
    preferences.end();

    setupTime();
    loadScheduleFromPreferences();

    server.on("/", handleMain);
    server.on("/settimezone", HTTP_POST, handleSetTimezone);
    server.on("/saveschedule", HTTP_POST, handleSaveSchedule);
    server.on("/loadschedule", HTTP_GET, handleLoadSchedule);
    server.on("/status", HTTP_GET, handleStatus);
    server.begin();
  }
  else
  {
    Serial.println("\nFailed to connect, starting AP mode");
    startAPMode();
  }
}

void setup()
{
  Serial.begin(115200);
  delay(1000);

  setupPWM();
  startSTAMode();
  currentDutyPWM = calculateCurrentDuty();
  setPWMDuty(currentDutyPWM);
}

void loop()
{
  server.handleClient();

  // Update PWM output every second
  static unsigned long lastPWMUpdate = 0;
  if (millis() - lastPWMUpdate >= 1000)
  {
    lastPWMUpdate = millis();
    updatePWMFromSchedule();
    syncTimeIfNeeded();
  }
}