#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>

const char* sta_ssid     = "YOUR_WIFI_SSID";
const char* sta_password = "YOUR_WIFI_PASSWORD";

const char* ap_ssid = "BatteryMonitor";         // AP mode SSID
const char* ap_password = "12345678";           // AP password (min 8 chars)

const unsigned long wifiTimeout = 15000;        // 15 seconds timeout

AsyncWebServer server(80);
const uint8_t BATTERY_ADDR = 0x0B;  // Try 0x16 if no response

// Commands unchanged...
#define CMD_VOLTAGE              0x09
#define CMD_CURRENT              0x0A
#define CMD_TEMPERATURE          0x08
#define CMD_RELATIVE_SOC         0x0D
#define CMD_REMAINING_CAPACITY   0x0F
#define CMD_FULL_CHARGE_CAPACITY 0x10
#define CMD_CYCLE_COUNT          0x17
#define CMD_RUN_TIME_TO_EMPTY    0x11
#define CMD_AVERAGE_TIME_TO_FULL 0x13
#define CMD_CHARGING_CURRENT     0x14
#define CMD_CHARGING_VOLTAGE     0x15
#define CMD_BATTERY_STATUS       0x16
#define CMD_DESIGN_CAPACITY      0x18
#define CMD_DESIGN_VOLTAGE       0x19
#define CMD_MAX_ERROR            0x0C
#define CMD_MANUFACTURER_NAME    0x20
#define CMD_DEVICE_NAME          0x21
#define CMD_DEVICE_CHEMISTRY     0x22
#define CMD_MANUFACTURE_DATE     0x1B
#define CMD_SERIAL_NUMBER        0x1C
#define CMD_VOLTAGE              0x09
// New extended command for cell voltages
#define CMD_MANUFACTURER_ACCESS  0x00
#define SUBCMD_VOLTAGES          0x0071  // Returns block with individual cell voltages

#define CELL_VOLT_1 0x3F  // Common for Cell 1 on BQ40Zxx
#define CELL_VOLT_2 0x3E
#define CELL_VOLT_3 0x3D
#define CELL_VOLT_4 0x3C


struct BatteryData {
  float voltage = -1;
  int current = 0;
  float temperature = -1;
  uint16_t soc = 0;
  uint32_t remaining = 0;
  uint32_t full = 0;
  uint32_t designCapacity = 0;
  uint16_t designVoltage = 0;
  uint16_t cycles = 0;
  uint16_t runtimeEmpty = 0;
  uint16_t timeToFull = 0;
  uint16_t chargingCurrent = 0;
  uint16_t chargingVoltage = 0;
  uint16_t maxError = 0;
  String manufacturer = "N/A";
  String deviceName = "N/A";
  String chemistry = "N/A";
  String manufactureDate = "N/A";
  uint16_t serialNumber = 0;
  float health = -1;
  String statusFlags = "N/A";

  // New: Individual cells (mV, 0 = invalid/not present)
  uint16_t cell1 = 0;
  uint16_t cell2 = 0;
  uint16_t cell3 = 0;
  uint16_t cell4 = 0;
};


BatteryData bat;
bool i2cError = false;

void recoverI2C() {
  Wire.end();
  pinMode(21, OUTPUT);  // SDA default pin
  pinMode(22, OUTPUT);  // SCL default pin
  digitalWrite(21, HIGH);
  digitalWrite(22, HIGH);
  delay(10);
  // Send 9 clock pulses to release stuck slave
  for (int i = 0; i < 9; i++) {
    digitalWrite(22, LOW);  delayMicroseconds(5);
    digitalWrite(22, HIGH); delayMicroseconds(5);
  }
  pinMode(21, INPUT);
  pinMode(22, INPUT);
  delay(100);
  Wire.begin();
  Wire.setClock(50000);  // 50 kHz
  i2cError = false;
  Serial.println("[I2C] Bus recovered with clock pulses");
}

// Helper to read extended word via ManufacturerAccess (0x00)
uint16_t readExtendedWord(uint16_t subcmd) {
  Wire.beginTransmission(BATTERY_ADDR);
  Wire.write(0x00);  // ManufacturerAccess
  Wire.write(lowByte(subcmd));
  Wire.write(highByte(subcmd));
  if (Wire.endTransmission(false) != 0) return 0;
  if (Wire.requestFrom(BATTERY_ADDR, (uint8_t)2) != 2) return 0;
  uint16_t val = Wire.read() | (Wire.read() << 8);
  return val;
}


  uint16_t readCellVoltage(uint16_t subcmd) {
    String block = readBlockData(CMD_MANUFACTURER_ACCESS, subcmd);
    if (block.length() >= 2) {
      uint16_t v = (uint8_t)block[0] | ((uint8_t)block[1] << 8);
      return (v >= 2500 && v <= 4300) ? v : 0;
    }
    return 0;
  }

uint16_t readWord(uint8_t cmd) {
  Wire.beginTransmission(BATTERY_ADDR);
  Wire.write(cmd);
  if (Wire.endTransmission(false) != 0) { i2cError = true; return 0xFFFF; }
  if (Wire.requestFrom(BATTERY_ADDR, (uint8_t)2) != 2) { i2cError = true; return 0xFFFF; }
  uint16_t val = Wire.read() | (Wire.read() << 8);
  return val;
}

String readBlock(uint8_t cmd) {
  Wire.beginTransmission(BATTERY_ADDR);
  Wire.write(cmd);
  if (Wire.endTransmission(false) != 0) return "N/A";
  uint8_t len = Wire.requestFrom(BATTERY_ADDR, (uint8_t)34);
  if (len < 1) return "N/A";
  uint8_t l = Wire.read();
  if (l == 0 || l > 32) return "N/A";
  String s = "";
  for (uint8_t i = 0; i < l; i++) {
    char c = Wire.read();
    if (c >= 32 && c <= 126) s += c;
  }
  while (Wire.available()) Wire.read();
  return (s.length() > 0) ? s : "N/A";
}

// New: Read block data for extended commands
String readBlockData(uint8_t cmd, uint16_t subcmd) {
  // Write subcommand to ManufacturerAccess
  Wire.beginTransmission(BATTERY_ADDR);
  Wire.write(cmd);
  Wire.write(lowByte(subcmd));
  Wire.write(highByte(subcmd));
  if (Wire.endTransmission(false) != 0) return "";

  // Read block
  uint8_t len = Wire.requestFrom(BATTERY_ADDR, (uint8_t)32);
  if (len < 1) return "";
  uint8_t blockLen = Wire.read();
  if (blockLen == 0 || blockLen > 30) return "";

  String data = "";
  for (uint8_t i = 0; i < blockLen; i++) {
    data += (char)Wire.read();
  }
  while (Wire.available()) Wire.read();
  return data;
}

String formatManufactureDate(uint16_t raw) {
  if (raw == 0xFFFF || raw == 0) return "N/A";
  uint8_t day = raw & 0x1F;
  uint8_t month = (raw >> 5) & 0x0F;
  uint16_t year = 1980 + ((raw >> 9) & 0x7F);
  if (day < 1 || day > 31 || month < 1 || month > 12) return "N/A";
  char buf[11];
  sprintf(buf, "%04d-%02d-%02d", year, month, day);
  return String(buf);
}

String decodeBatteryStatus(uint16_t status) {
  if (status == 0xFFFF) return "Read Error";
  // ... (same as before, unchanged for brevity)
  String flags = "";
  if (status & 0x8000) flags += "<span class='badge bg-danger'>OVER_CHARGED</span> ";
  if (status & 0x4000) flags += "<span class='badge bg-warning'>TERM_CHARGE</span> ";
  if (status & 0x1000) flags += "<span class='badge bg-danger'>OVER_TEMP</span> ";
  if (status & 0x0800) flags += "<span class='badge bg-warning'>TERM_DISCHARGE</span> ";
  if (status & 0x0200) flags += "<span class='badge bg-primary'>CAP_ALARM</span> ";
  if (status & 0x0100) flags += "<span class='badge bg-primary'>TIME_ALARM</span> ";
  if (status & 0x0080) flags += "<span class='badge bg-info'>INITIALIZED</span> ";
  if (status & 0x0040) flags += "<span class='badge bg-secondary'>DISCHARGING</span> ";
  if (status & 0x0020) flags += "<span class='badge bg-success'>FULLY_CHARGED</span> ";
  if (status & 0x0010) flags += "<span class='badge bg-danger'>FULLY_DISCHARGED</span> ";
  if (flags == "") flags = "<span class='badge bg-light text-dark'>OK</span>";
  return flags;
}

void updateBatteryData() {
  if (i2cError) recoverI2C();

  uint16_t raw;
  raw = readWord(CMD_VOLTAGE); bat.voltage = (raw < 500 || raw > 50000) ? -1 : raw / 1.0; delay(50);
  bat.current = (int16_t)readWord(CMD_CURRENT); delay(50);
  raw = readWord(CMD_TEMPERATURE); bat.temperature = (raw == 0xFFFF) ? -1 : (raw / 1.0) - 273.15; delay(50);
  bat.soc = readWord(CMD_RELATIVE_SOC); if (bat.soc > 100) bat.soc = 0; delay(50);
  bat.remaining = readWord(CMD_REMAINING_CAPACITY); delay(50);
  bat.full = readWord(CMD_FULL_CHARGE_CAPACITY); delay(50);
  bat.designCapacity = readWord(CMD_DESIGN_CAPACITY); delay(50);
  bat.designVoltage = readWord(CMD_DESIGN_VOLTAGE); delay(50);
  bat.cycles = readWord(CMD_CYCLE_COUNT); delay(50);
  bat.runtimeEmpty = readWord(CMD_RUN_TIME_TO_EMPTY); if (bat.runtimeEmpty == 0xFFFF) bat.runtimeEmpty = 0; delay(50);
  bat.timeToFull = readWord(CMD_AVERAGE_TIME_TO_FULL); if (bat.timeToFull == 0xFFFF) bat.timeToFull = 0; delay(50);
  bat.chargingCurrent = readWord(CMD_CHARGING_CURRENT); delay(50);
  bat.chargingVoltage = readWord(CMD_CHARGING_VOLTAGE); delay(50);
  bat.maxError = readWord(CMD_MAX_ERROR); delay(50);

  uint16_t statusRaw = readWord(CMD_BATTERY_STATUS);
  bat.statusFlags = decodeBatteryStatus(statusRaw);

  bat.manufacturer = readBlock(CMD_MANUFACTURER_NAME); delay(50);
  bat.deviceName = readBlock(CMD_DEVICE_NAME); delay(50);
  bat.chemistry = readBlock(CMD_DEVICE_CHEMISTRY); delay(50);
  bat.manufactureDate = formatManufactureDate(readWord(CMD_MANUFACTURE_DATE)); delay(50);
  bat.serialNumber = readWord(CMD_SERIAL_NUMBER);

  bat.health = (bat.designCapacity > 0 && bat.full > 0) ? (bat.full * 1000.0 / bat.designCapacity) : -1;



  // In updateBatteryData(), replace cell reading with:
  bat.cell1 = readExtendedWord(CELL_VOLT_1);
  bat.cell2 = readExtendedWord(CELL_VOLT_2);
  bat.cell3 = readExtendedWord(CELL_VOLT_3);
  bat.cell4 = readExtendedWord(CELL_VOLT_4);
  delay(50);
}

// getJSON and index_html updated to show "--" or "N/A" for invalid values
String getJSON() {
  updateBatteryData();

  String json = "{";
  json += "\"voltage\":" + (bat.voltage < 0 ? "null" : String(bat.voltage, 3)) + ",";
  json += "\"current\":" + String(bat.current) + ",";
  json += "\"temp\":" + (bat.temperature < 0 ? "null" : String(bat.temperature, 1)) + ",";
  json += "\"soc\":" + String(bat.soc) + ",";
  json += "\"remaining\":" + String(bat.remaining) + ",";
  json += "\"full\":" + String(bat.full) + ",";
  json += "\"designCapacity\":" + String(bat.designCapacity) + ",";
  json += "\"designVoltage\":" + String(bat.designVoltage) + ",";
  json += "\"cycles\":" + String(bat.cycles) + ",";
  json += "\"runtimeEmpty\":" + String(bat.runtimeEmpty) + ",";
  json += "\"timeToFull\":" + String(bat.timeToFull) + ",";
  json += "\"chargingCurrent\":" + String(bat.chargingCurrent) + ",";
  json += "\"chargingVoltage\":" + String(bat.chargingVoltage / 1000.0, 3) + ",";
  json += "\"maxError\":" + String(bat.maxError) + ",";
  json += "\"health\":" + (bat.health < 0 ? "null" : String(bat.health, 1)) + ",";
  json += "\"statusFlags\":\"" + bat.statusFlags + "\",";
  json += "\"manufacturer\":\"" + bat.manufacturer + "\",";
  json += "\"device\":\"" + bat.deviceName + "\",";
  json += "\"chemistry\":\"" + bat.chemistry + "\",";
  json += "\"manufactureDate\":\"" + bat.manufactureDate + "\",";
  json += "\"serialNumber\":\"" + String(bat.serialNumber) + "\",";
  json += "\"cell1\":" + (bat.cell1 == 0 ? "null" : String(bat.cell1 / 1000.0, 3)) + ",";
  json += "\"cell2\":" + (bat.cell2 == 0 ? "null" : String(bat.cell2 / 1000.0, 3)) + ",";
  json += "\"cell3\":" + (bat.cell3 == 0 ? "null" : String(bat.cell3 / 1000.0, 3)) + ",";
  json += "\"cell4\":" + (bat.cell4 == 0 ? "null" : String(bat.cell4 / 1000.0, 3)) + "";
  json += "}";

  // Serial logging unchanged...
  Serial.print("["); Serial.print(millis() / 1000); Serial.print("s] ");
  Serial.println(json);

  return json;
}

// HTML remains the same (unchanged from previous version)
// ... [paste the full index_html from the last working version here]

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>ESP32 Advanced Smart Battery Monitor</title>
  <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css" rel="stylesheet">
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    body { background: #f8f9fa; padding: 20px 0; }
    .gauge-container { position: relative; width: 260px; height: 160px; margin: 0 auto; }
    .gauge { width: 100%; height: 100%; }
    .soc-text { position: absolute; top: 52%; left: 50%; transform: translate(-50%, -50%); font-size: 3rem; font-weight: bold; }
    .badge { margin: 2px; }
        /* Add cell card styles */
    .cell-card { text-align: center; padding: 15px; border-radius: 10px; color: white; }
    .cell-green { background: #28a745; }
    .cell-yellow { background: #ffc107; color: black; }
    .cell-red { background: #dc3545; }
    .cell-na { background: #6c757d; }

  </style>
</head>
<body>
  <div class="container">
    <h1 class="text-center mb-5 text-primary">Advanced Smart Battery Monitor</h1>

    <div class="row g-4 mb-5">
      <div class="col-lg-4 col-md-6">
        <div class="card shadow text-center p-4">
          <h5>State of Charge</h5>
          <div class="gauge-container">
            <canvas id="socGauge"></canvas>
            <div id="socText" class="soc-text">--%</div>
          </div>
          <small class="text-muted mt-2">Health: <span id="health">--</span>% | Max Error: <span id="maxError">--</span>%</small>
        </div>
      </div>

      <div class="col-lg-8 col-md-6">
        <div class="row g-3">
          <div class="col-6 col-sm-4"><div class="card shadow p-3 text-center"><h6>Voltage</h6><h4 id="voltage">--</h4> V</div></div>
          <div class="col-6 col-sm-4"><div class="card shadow p-3 text-center"><h6>Current</h6><h4 id="current">--</h4> mA</div></div>
          <div class="col-6 col-sm-4"><div class="card shadow p-3 text-center"><h6>Temp</h6><h4 id="temp">--</h4> °C</div></div>
          <div class="col-6 col-sm-4"><div class="card shadow p-3 text-center"><h6>Remaining</h6><h4 id="remaining">--</h4> mAh</div></div>
          <div class="col-6 col-sm-4"><div class="card shadow p-3 text-center"><h6>Full Cap.</h6><h4 id="full">--</h4> mAh</div></div>
          <div class="col-6 col-sm-4"><div class="card shadow p-3 text-center"><h6>Cycles</h6><h4 id="cycles">--</h4></div></div>
        </div>
      </div>
    </div>

    <div class="row mb-4">
      <div class="col-md-6">
        <div class="card shadow p-4">
          <h5>Battery Details</h5>
          Manufacturer: <span id="manufacturer">--</span><br>
          Device: <span id="device">--</span><br>
          Chemistry: <span id="chemistry">--</span><br>
          Manufacture Date: <span id="manufactureDate">--</span><br>
          Serial: <span id="serialNumber">--</span>
        </div>
      </div>
      <div class="col-md-6">
        <div class="card shadow p-4">
          <h5>Battery Status Flags</h5>
          <div id="statusFlags">Loading...</div>
        </div>
      </div>
    </div>
    <!-- New Cell Voltages Section -->
    <h3 class="mb-3 mt-5">Individual Cell Voltages</h3>
    <div class="row g-3 mb-5">
      <div class="col-6 col-md-3">
        <div id="cell1Card" class="card shadow cell-card cell-na">
          <h5>Cell 1</h5>
          <h3 id="cell1">--</h3> V
        </div>
      </div>
      <div class="col-6 col-md-3">
        <div id="cell2Card" class="card shadow cell-card cell-na">
          <h5>Cell 2</h5>
          <h3 id="cell2">--</h3> V
        </div>
      </div>
      <div class="col-6 col-md-3">
        <div id="cell3Card" class="card shadow cell-card cell-na">
          <h5>Cell 3</h5>
          <h3 id="cell3">--</h3> V
        </div>
      </div>
      <div class="col-6 col-md-3">
        <div id="cell4Card" class="card shadow cell-card cell-na">
          <h5>Cell 4</h5>
          <h3 id="cell4">--</h3> V
        </div>
      </div>
    </div>

    <h3 class="mb-3">Advanced Metrics</h3>
    <div class="row g-3 mb-5">
      <div class="col-md-3 col-sm-6"><div class="card shadow p-3 text-center"><h6>Design Cap.</h6><h5 id="designCapacity">--</h5> mAh</div></div>
      <div class="col-md-3 col-sm-6"><div class="card shadow p-3 text-center"><h6>Design Volt.</h6><h5 id="designVoltage">--</h5> mV</div></div>
      <div class="col-md-3 col-sm-6"><div class="card shadow p-3 text-center"><h6>Time to Full</h6><h5 id="timeToFull">--</h5> min</div></div>
      <div class="col-md-3 col-sm-6"><div class="card shadow p-3 text-center"><h6>Run Time</h6><h5 id="runtimeEmpty">--</h5> min</div></div>
      <div class="col-md-3 col-sm-6"><div class="card shadow p-3 text-center"><h6>Req. Chg V</h6><h5 id="chargingVoltage">--</h5> V</div></div>
      <div class="col-md-3 col-sm-6"><div class="card shadow p-3 text-center"><h6>Req. Chg I</h6><h5 id="chargingCurrent">--</h5> mA</div></div>
    </div>

    <div class="text-center text-muted small">Auto-updates every 5s • JSON logged to Serial</div>
  </div>

  <script>
    var gauge;
    var createGauge = function() {
      gauge = new Chart(document.getElementById('socGauge'), {
        type: 'doughnut',
        data: { datasets: [{ data: [0, 100], backgroundColor: ['#28a745', '#e9ecef'], borderWidth: 0 }] },
        options: { cutout: '75%', responsive: true, maintainAspectRatio: false, plugins: { legend: { display: false }, tooltip: { enabled: false } } }
      });
    };
    createGauge();

    var updateData = function() {
      function updateCell(cardId, valueId, voltage) {
        const card = document.getElementById(cardId);
        const valEl = document.getElementById(valueId);
        if (voltage === null || voltage < 2.5) {
          valEl.innerText = '--';
          card.className = 'card shadow cell-card cell-na';
        } else {
          valEl.innerText = voltage.toFixed(3);
          if (voltage > 3.6) card.className = 'card shadow cell-card cell-green';
          else if (voltage > 3.3) card.className = 'card shadow cell-card cell-yellow';
          else card.className = 'card shadow cell-card cell-red';
        }
      }
      
      fetch('/data').then(r => r.json()).then(d => {
        document.getElementById('voltage').innerText = (d.voltage === null ? '--' : (d.voltage / 1000).toFixed(3));
        document.getElementById('current').innerText = d.current;
        document.getElementById('temp').innerText = (d.temp === null ? '--' : (d.temp / 100).toFixed(1));
        document.getElementById('remaining').innerText = d.remaining;
        document.getElementById('full').innerText = d.full;
        document.getElementById('cycles').innerText = d.cycles;
        document.getElementById('runtimeEmpty').innerText = d.runtimeEmpty;
        document.getElementById('timeToFull').innerText = (d.timeToFull == 0 ? 'N/A' : d.timeToFull);
        document.getElementById('health').innerText = (d.health === null ? '--' : (d.health / 10).toFixed(1));
        document.getElementById('maxError').innerText = d.maxError;
        document.getElementById('manufacturer').innerText = d.manufacturer;
        document.getElementById('device').innerText = d.device;
        document.getElementById('chemistry').innerText = d.chemistry;
        document.getElementById('manufactureDate').innerText = d.manufactureDate;
        document.getElementById('serialNumber').innerText = d.serialNumber;
        document.getElementById('designCapacity').innerText = d.designCapacity;
        document.getElementById('designVoltage').innerText = d.designVoltage;
        document.getElementById('chargingCurrent').innerText = d.chargingCurrent;
        document.getElementById('chargingVoltage').innerText = d.chargingVoltage;
        document.getElementById('statusFlags').innerHTML = d.statusFlags;
        updateCell('cell1Card', 'cell1', d.cell1 === null ? null : d.cell1);
        updateCell('cell2Card', 'cell2', d.cell2 === null ? null : d.cell2);
        updateCell('cell3Card', 'cell3', d.cell3 === null ? null : d.cell3);
        updateCell('cell4Card', 'cell4', d.cell4 === null ? null : d.cell4);
        document.getElementById('socText').innerText = d.soc + '%';
        gauge.data.datasets[0].data = [d.soc, 100 - d.soc];
        gauge.data.datasets[0].backgroundColor[0] = d.soc > 30 ? '#28a745' : (d.soc > 10 ? '#ffc107' : '#dc3545');
        gauge.update();
      });
    };
    setInterval(updateData, 5000);
    updateData();
  </script>
</body>
</html>
)rawliteral";

void setupWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(sta_ssid, sta_password);
  
  Serial.print("Connecting to WiFi");
  
  unsigned long startAttemptTime = millis();
  
  while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < wifiTimeout) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi Connected!");
    Serial.print("IP Address: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\nWiFi connection failed. Starting AP mode...");
    
    WiFi.mode(WIFI_AP);
    WiFi.softAP(ap_ssid, ap_password);
    
    Serial.println("Access Point started");
    Serial.print("AP SSID: ");
    Serial.println(ap_ssid);
    Serial.print("AP Password: ");
    Serial.println(ap_password);
    Serial.print("AP IP Address: http://");
    Serial.println(WiFi.softAPIP());
  }
}


void setup() {
  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println("\n=== ESP32 Smart Battery Monitor Started ===");

  Wire.begin();
  Wire.setClock(50000);

  setupWiFi();  // <--- New function


  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: http://");
  Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });

  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", getJSON());
  });

  server.begin();
  Serial.println("Web server running. Open IP in browser and watch Serial for JSON logs.");
}

void loop() {
  // All handled asynchronously
}