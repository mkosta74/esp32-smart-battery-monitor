#include <WiFi.h>
#include <ImprovWiFiLibrary.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <Wire.h>


const unsigned long wifiTimeout = 15000;        // 15 seconds timeout

bool serverStarted = false;  // Flag to track if server has been started


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
  if (status & 0x0008) flags += "<span class='badge bg-info'>EC3</span> ";
  if (status & 0x0004) flags += "<span class='badge bg-info'>EC2</span> ";
  if (status & 0x0002) flags += "<span class='badge bg-info'>EC1</span> ";
  if (status & 0x0001) flags += "<span class='badge bg-info'>EC0</span> ";
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
  bat.cell1 = readWord(CELL_VOLT_1);
  bat.cell2 = readWord(CELL_VOLT_2);
  bat.cell3 = readWord(CELL_VOLT_3);
  bat.cell4 = readWord(CELL_VOLT_4);
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
  <title>ESP32 Smart Battery Monitor</title>
  <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css" rel="stylesheet">
  <script src="https://cdn.jsdelivr.net/npm/chart.js"></script>
  <style>
    :root {
      --bg-color: #f8f9fa;
      --card-bg: #ffffff;
      --text-color: #212529;
      --border-color: #dee2e6;
      --gauge-fill: #28a745;
      --gauge-empty: #e9ecef;
    }
    
    @media (prefers-color-scheme: dark) {
      :root {
        --bg-color: #121212;
        --card-bg: #1e1e1e;
        --text-color: #e0e0e0;
        --border-color: #333333;
        --gauge-fill: #28a745;
        --gauge-empty: #2d2d2d;
      }
      body { background-color: var(--bg-color); color: var(--text-color); }
      .card { background-color: var(--card-bg); border-color: var(--border-color); color: var(--text-color); }
      .text-muted { color: #aaaaaa !important; }
    }
    
    body { background-color: var(--bg-color); color: var(--text-color); padding: 20px 0; transition: background 0.3s, color 0.3s; }
    .card { background-color: var(--card-bg); border: 1px solid var(--border-color); box-shadow: 0 4px 8px rgba(0,0,0,0.1); }
    .gauge-container { position: relative; width: 260px; height: 160px; margin: 0 auto; }
    .gauge { width: 100%; height: 100%; }
    .soc-text { position: absolute; top: 52%; left: 50%; transform: translate(-50%, -50%); font-size: 3rem; font-weight: bold; color: var(--text-color); }
    .cell-card { text-align: center; padding: 15px; border-radius: 10px; color: white; }
    .cell-green { background: #28a745; }
    .cell-yellow { background: #ffc107; color: black; }
    .cell-red { background: #dc3545; }
    .cell-na { background: #6c757d; }
  </style>
</head>
<body>
  <div class="container">
    <h1 class="text-center mb-5 text-primary">Smart Battery Monitor</h1>

    <div class="row g-4 mb-5">
      <div class="col-lg-4 col-md-6">
        <div class="card shadow text-center p-4">
          <h5>State of Charge</h5>
          <div class="gauge-container">
            <canvas id="socGauge"></canvas>
            <div id="socText" class="soc-text">--%</div>
          </div>
          <small class="text-muted mt-2">Health: <span id="health">--</span>% | Error: <span id="maxError">--</span>%</small>
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
      <div class="col-md-2">
        <div class="card shadow p-4">
          <h5>Battery Details</h5>
          Manufacturer: <span id="manufacturer">--</span><br>
          Device: <span id="device">--</span><br>
          Chemistry: <span id="chemistry">--</span><br>
          Manufacture Date: <span id="manufactureDate">--</span><br>
          Serial: <span id="serialNumber">--</span>
        </div>
      </div>
      <div class="col-md-2">
        <div class="card shadow p-4">
          <h5>Battery Status Flags</h5>
          <div id="statusFlags">Loading...</div>
        </div>
      </div>
      <div class="col-md-8">
        <div class="card shadow p-4">
          <h5>Battery History (Last Hour)</h5>
          <canvas id="historyChart"></canvas>
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

    <div class="text-center mt-5">
      <a href="/advanced" class="btn btn-secondary">Advanced scan tools</a>
    </div>

    <div class="text-center text-muted small">Auto-updates every 5s • Dark mode follows OS setting</div>
  </div>

  <script>
    var gauge;
    var createGauge = function() {
      gauge = new Chart(document.getElementById('socGauge'), {
        type: 'doughnut',
        data: { datasets: [{ data: [0, 100], backgroundColor: [getComputedStyle(document.documentElement).getPropertyValue('--gauge-fill'), getComputedStyle(document.documentElement).getPropertyValue('--gauge-empty')], borderWidth: 0 }] },
        options: { cutout: '75%', responsive: true, maintainAspectRatio: false, plugins: { legend: { display: false }, tooltip: { enabled: false } } }
      });
    };
    createGauge();

    const ctx = document.getElementById('historyChart').getContext('2d');
    const historyChart = new Chart(ctx, {
      type: 'line',
      data: { labels: [], datasets: [
        { label: 'SOC %', data: [], borderColor: '#28a745', yAxisID: 'y' },
        { label: 'Voltage V', data: [], borderColor: '#007bff', yAxisID: 'y1' },
        { label: 'Current mA', data: [], borderColor: '#dc3545', yAxisID: 'y1' }
      ]},
      options: { scales: { y: { position: 'left' }, y1: { position: 'right' } } }
    });

    var updateData = function() {




      function updateCell(cardId, valueId, voltage) {
        const card = document.getElementById(cardId);
        const valEl = document.getElementById(valueId);
        if (voltage === null || voltage < 2.5 || voltage > 10) {
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
        document.getElementById('timeToFull').innerText = d.timeToFull == 0 ? 'N/A' : d.timeToFull;
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
        document.getElementById('socText').innerText = d.soc + '%';
        gauge.data.datasets[0].data = [d.soc, 100 - d.soc];
        gauge.data.datasets[0].backgroundColor[0] = d.soc > 30 ? '#28a745' : (d.soc > 10 ? '#ffc107' : '#dc3545');
        gauge.update();
        updateCell('cell1Card', 'cell1', d.cell1);
        updateCell('cell2Card', 'cell2', d.cell2);
        updateCell('cell3Card', 'cell3', d.cell3);
        updateCell('cell4Card', 'cell4', d.cell4);

        // Push new data point with timestamp
        historyChart.data.labels.push(new Date().toLocaleTimeString());
        historyChart.data.datasets[0].data.push(d.soc);
        historyChart.data.datasets[1].data.push(d.voltage);
        historyChart.data.datasets[2].data.push(d.current);
        if (historyChart.data.labels.length > 600) {  // Keep last 60 points
          historyChart.data.labels.shift();
          historyChart.data.datasets.forEach(ds => ds.data.shift());
        }
        historyChart.update();

      }).catch(() => {});
    };


    setInterval(updateData, 3000);
    updateData();
  </script>
</body>
</html>
)rawliteral";

const char advanced_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>Advanced SMBus Tools</title>
  <link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.3/dist/css/bootstrap.min.css" rel="stylesheet">
  <style>
    :root { --bg-color: #f8f9fa; --card-bg: #ffffff; --text-color: #212529; --border-color: #dee2e6; }
    @media (prefers-color-scheme: dark) {
      :root { --bg-color: #121212; --card-bg: #1e1e1e; --text-color: #e0e0e0; --border-color: #333333; }
      body { background-color: var(--bg-color); color: var(--text-color); }
      .card, .form-control, .form-select, table { background-color: var(--card-bg); border-color: var(--border-color); color: var(--text-color); }
      .table { --bs-table-bg: var(--card-bg); }
      .form-control::placeholder { color: #aaa; }
    }
    body { background-color: var(--bg-color); color: var(--text-color); padding: 20px 0; }
    .card { background-color: var(--card-bg); border: 1px solid var(--border-color); }
    pre, table { background: #2d2d2d; color: #f8f8f2; border-radius: 6px; }
    .btn-primary { margin-top: 10px; }
    #batchProgress { display: none; }
  </style>
</head>
<body>
  <div class="container">
    <h1 class="text-center mb-4 text-primary">Advanced SMBus Tools</h1>
    <p class="text-center text-muted mb-5">Scanner • Single Command • Batch Range • Use carefully!</p>

    <div class="row g-4 mb-4">
      <!-- I2C Scanner -->
      <div class="col-lg-4">
        <div class="card shadow p-4">
          <h4>I2C Bus Scanner</h4>
          <button class="btn btn-warning w-100" onclick="scanI2C()">Scan Bus</button>
          <pre id="scanResult" class="mt-3">Click Scan to begin...</pre>
        </div>
           <!-- New: Read Extended Word -->
        <div class="card shadow p-4">
          <h4>Read Extended Word</h4>
          <div class="mb-3">
            <label class="form-label">Battery Address (hex)</label>
            <input type="text" onchange="readExtendedWord()" class="form-control" id="wordAddr" value="0B">
          </div>
          <div class="mb-3">
            <label class="form-label">Subcommand (hex)</label>
            <input type="text" onchange="readExtendedWord()" class="form-control" id="wordSubcmd" value="0001" placeholder="e.g. 0001 Device Type">
          </div>
          <button class="btn btn-info w-100" onclick="readExtendedWord()">Read Word</button>
          <pre id="wordResult" class="mt-3 flex-grow-1">Result will appear here...</pre>
        </div>        
      </div>

      <!-- Single Command -->
      <div class="col-lg-4">
        <div class="card shadow p-4">
          <h4>Single Command</h4>
          <input type="text" onchange="sendSingle()" class="form-control mb-2" id="singleAddr" value="0B" placeholder="Address (hex)">
          <input type="text" onchange="sendSingle()" class="form-control mb-2" id="singleCmd" value="23" placeholder="Command (hex)">
          <select class="form-select mb-3" id="singleType">
            <option value="block">Block</option>
            <option value="word">Word</option>
            <option value="auto">Auto</option>
          </select>
          <button class="btn btn-primary w-100" onclick="sendSingle()">Send</button>
          <pre id="singleResult" class="mt-3">Result appears here</pre>
        </div>
      </div>
      <!-- Unseal Battery with Verification -->
      <div class="col-lg-4">
        <div class="card shadow p-4 h-100 warning-card">
          <h4 class="text-danger">⚠ Unseal Battery (bq2084)</h4>
          <p class="small text-muted">Default key: 0x0414 → 0x3672<br>Then full access 0xFFFF → 0xFFFF</p>
          <button class="btn btn-danger w-100 mb-3" onclick="unsealAndVerify()">Attempt Unseal + Verify</button>
          <pre id="unsealResult" class="mt-3 flex-grow-1">Status: Ready</pre>
          <div id="verifyStatus" class="mt-3 text-center fw-bold"></div>
        </div>
      </div>
      </div>
      </div>
    <div class="row g-4 mb-4">
      <!-- Batch Range -->
      <div class="col-12">
        <div class="card shadow p-4">
          <h4>Batch Command Range</h4>
          <input type="text" class="form-control mb-2" id="batchAddr" value="0B" placeholder="Address (hex)">
          <div class="row">
            <div class="col">
              <input type="text" class="form-control mb-2" id="startCmd" value="00" placeholder="Start (hex)">
            </div>
            <div class="col">
              <input type="text" class="form-control mb-2" id="endCmd" value="30" placeholder="End (hex)">
            </div>
          </div>
          <div class="progress mb-3" id="batchProgress">
            <div class="progress-bar progress-bar-striped progress-bar-animated" style="width: 0%"></div>
          </div>
          <button class="btn btn-success w-100" onclick="sendBatch()">Scan Range</button>
          <div id="batchResult" class="mt-3" style="overflow-y: auto;"></div>
        </div>
      </div>
    </div>

    <div class="text-center mt-5">
      <a href="/" class="btn btn-secondary">← Back to Dashboard</a>
    </div>
  </div>

<script>
  let baseline = null;  // Stores first scan results: {cmd: {hex, dec, str}}


  // Known ManufacturerInfo block decoding (13 bytes, TI bq20zxx/bq30zxx series)
  function decodeManufacturerInfo(hexString) {
    // Remove spaces and convert to array of bytes
    let bytes = hexString.trim().split(/\s+/).map(b => parseInt(b, 16));
    if (bytes.length !== 13 && bytes.length !== 10) return "Invalid length (expected 13 bytes)";

    let packLot    = (bytes[0] << 8) | bytes[1];   // bytes 0-1
    let pcbLot     = (bytes[2] << 8) | bytes[3];   // bytes 2-3
    let firmware   = bytes[4];
    let hardware   = bytes[5];
    let cellRev    = bytes[6];
    let partialRes = bytes[7];
    let fullRes    = bytes[8];
    let wdtRes     = bytes[9];
    let checksum   = (bytes[11] << 8) | bytes[12]; // bytes 11-12

    let html = `
      <table class="table table-sm table-bordered mt-3">
        <thead class="table-dark"><tr><th>Field</th><th>Value</th></tr></thead>
        <tbody class="table-dark">
          <tr><td>Pack Lot Code</td><td>${packLot}</td></tr>
          <tr><td>PCB Lot Code</td><td>${pcbLot}</td></tr>
          <tr><td>Firmware Version</td><td>${firmware}</td></tr>
          <tr><td>Hardware Revision</td><td>${hardware}</td></tr>
          <tr><td>Cell Revision</td><td>${cellRev}</td></tr>
          <tr><td>Partial Reset Counter</td><td>${partialRes}</td></tr>
          <tr><td>Full Reset Counter</td><td>${fullRes}</td></tr>
          <tr><td>Watchdog Reset Counter</td><td>${wdtRes}</td></tr>
          <tr><td>Checksum</td><td>0x${checksum.toString(16).toUpperCase().padStart(4,'0')}</td></tr>
        </tbody>
      </table>`;

    return html;
  }

  // Auto-detect and decode ManufacturerInfo when response contains 13-byte block
  function tryDecodeManufacturerInfo(responseText) {
    // Look for hex line like "Hex: 06 75 0A B0 ..."
    let hexMatch = responseText.match(/Hex:\s*([0-9A-F\s]+)/i);
    if (hexMatch) {
      let hexLine = hexMatch[1];
      let byteCount = hexLine.trim().split(/\s+/).length;
      if (byteCount === 13 || byteCount === 10) {
        return decodeManufacturerInfo(hexLine);
      }
    }
    return null;
  }

  // Update single result display with decoding
  function displaySingleResult(text) {
    let decoded = tryDecodeManufacturerInfo(text);
    if (decoded) {
      document.getElementById('singleResult').innerHTML = 
        `<pre>${text}</pre><div class="mt-3"><strong>Decoded Manufacturer Info:</strong>${decoded}</div>`;
    } else {
      document.getElementById('singleResult').innerText = text;
    }
  }


  function scanI2C() {
    document.getElementById('scanResult').innerText = 'Scanning...';
    fetch('/scan').then(r => r.text()).then(t => {
      document.getElementById('scanResult').innerText = t;
    });
  }

  function sendSingle() {
        let addr = parseInt(document.getElementById('singleAddr').value, 16);
        let cmd = parseInt(document.getElementById('singleCmd').value, 16);
        let type = document.getElementById('singleType').value;
        if (isNaN(addr) || isNaN(cmd)) return alert("Invalid hex");

        document.getElementById('singleResult').innerText = 'Sending...';
        fetch(`/cmd?addr=${addr}&command=${cmd}&type=${type}`)
        .then(r => r.text())
        .then(t => displaySingleResult(t))
        .catch(() => document.getElementById('singleResult').innerText = 'Error');
    }



        // New: Read Extended Word function
    function readExtendedWord() {
      let addr = parseInt(document.getElementById('wordAddr').value, 16);
      let subcmd = parseInt(document.getElementById('wordSubcmd').value, 16);
      if (isNaN(addr) || isNaN(subcmd)) return alert("Invalid hex value");

      document.getElementById('wordResult').innerText = 'Reading...';
      fetch(`/cmd?addr=${addr}&command=00&subcmd=${subcmd.toString(16).padStart(4,'0')}&type=word`)
        .then(r => r.text())
        .then(t => {
          if (t.includes('Word Response')) {
            document.getElementById('wordResult').innerHTML = '<strong>' + t + '</strong>';
          } else {
            document.getElementById('wordResult').innerText = t;
          }
        })
        .catch(() => document.getElementById('wordResult').innerText = 'Error');
    }

  async function sendBatch() {
    let addr = parseInt(document.getElementById('batchAddr').value, 16);
    let start = parseInt(document.getElementById('startCmd').value, 16);
    let end = parseInt(document.getElementById('endCmd').value, 16);
    if (isNaN(addr) || isNaN(start) || isNaN(end) || start > end) return alert("Invalid range");

    let resultDiv = document.getElementById('batchResult');
    let progressBar = document.getElementById('batchProgress').querySelector('.progress-bar');
    document.getElementById('batchProgress').style.display = 'block';
    
    // Header with Clear button
    resultDiv.innerHTML = `
      <div class="d-flex justify-content-between align-items-center mb-3">
        <h5>Batch Results (Addr 0x${addr.toString(16).toUpperCase().padStart(2,'0')})</h5>
        <button class="btn btn-sm btn-outline-danger" onclick="baseline=null; this.closest('div').nextElementSibling.remove();">Clear Baseline</button>
      </div>
      <table class="table table-sm table-striped">
        <thead class="table-dark"><tr><th>Cmd</th><th>Hex</th><th>Dec</th><th>String</th><th>Change</th></tr></thead>
        <tbody class="table-dark"></tbody>
      </table>`;
    
    let tbody = resultDiv.querySelector('tbody');
    let total = end - start + 1;
    let count = 0;

    let current = {};

    for (let cmd = start; cmd <= end; cmd++) {
      let response = await fetch(`/cmd?addr=${addr}&command=${cmd}&type=auto`).then(r => r.text());

      let row = tbody.insertRow();
      row.insertCell(0).textContent = '0x' + cmd.toString(16).toUpperCase().padStart(2,'0');

      let hexCell = row.insertCell(1);
      let decCell = row.insertCell(2);
      let strCell = row.insertCell(3);
      let changeCell = row.insertCell(4);

      let hex = '';
      let dec = 0;
      let str = '';

      if (response.includes('Word Response')) {
        hex = response.match(/0x([0-9A-F]+)/i)[1];
        dec = parseInt(hex, 16);
        hexCell.textContent = '0x' + hex;
        decCell.textContent = dec;
      } else if (response.includes('Block Length')) {
        str = response.split('String: ')[1]?.trim() || '';
        strCell.textContent = str;
      } else {
        hexCell.textContent = response;
      }

      current[cmd] = {hex, dec, str};

      // Change detection
      if (baseline && baseline[cmd]) {
        let old = baseline[cmd];
        if (dec !== old.dec) {
          let diff = dec - old.dec;
          changeCell.innerHTML = `<span style="color:${diff > 0 ? 'lime' : 'red'}; font-weight:bold;">${diff > 0 ? '↑' : '↓'} ${Math.abs(diff)}</span>`;
          row.style.backgroundColor = diff > 0 ? 'rgba(0,255,0,0.1)' : 'rgba(255,0,0,0.1)';
        } else if (str !== old.str) {
          changeCell.innerHTML = '<span style="color:orange; font-weight:bold;">≠</span>';
          row.style.backgroundColor = 'rgba(255,165,0,0.1)';
        } else {
          changeCell.textContent = '—';
        }
      } else {
        changeCell.textContent = baseline ? '?' : '—';  // First run
      }

      count++;
      progressBar.style.width = (count / total * 100) + '%';
    }

    // Save as baseline if none exists
    if (!baseline) {
      baseline = current;
      resultDiv.querySelector('h5').insertAdjacentHTML('beforeend', ' <span class="badge bg-success">Baseline Set</span>');
    }

    progressBar.style.width = '100%';
    setTimeout(() => document.getElementById('batchProgress').style.display = 'none', 1000);
  }

  function clearBaseline() {
    baseline = null;
  }
    // NEW: Unseal function
    function unsealBattery() {
      let addr = parseInt(document.getElementById('singleAddr').value, 16);
      document.getElementById('unsealResult').innerText = 'Sending unseal sequence...';

      // Step 1: Unseal key part 1 (0x0414)
      fetch(`/unseal?addr=${addr}&step=1`)
        .then(r => r.text())
        .then(t => {
          document.getElementById('unsealResult').innerText += '\n' + t;
          // Step 2: Unseal key part 2 (0x3672)
          return fetch(`/unseal?addr=${addr}&step=2`);
        })
        .then(r => r.text())
        .then(t => {
          document.getElementById('unsealResult').innerText += '\n' + t;
          // Step 3: Full access (0xFFFF x2)
          return fetch(`/unseal?addr=${addr}&step=full`);
        })
        .then(r => r.text())
        .then(t => {
          document.getElementById('unsealResult').innerText += '\n' + t + '\n\nUnseal sequence complete!';
        })
        .catch(() => {
          document.getElementById('unsealResult').innerText += '\nError during sequence';
        });
    }

function unsealAndVerify() {
  let addr = parseInt(document.getElementById('singleAddr').value, 16);

  let resultBox = document.getElementById('unsealResult');
  let verifyBox = document.getElementById('verifyStatus');
  
  resultBox.innerText = 'Starting unseal sequence...\n';
  verifyBox.innerHTML = '';

  // Step 1: Unseal part 1
  fetch(`/unseal?addr=${addr}&step=1`)
    .then(r => r.text())
    .then(t => {
      resultBox.innerText += t + '\n';
      // Step 2: Unseal part 2
      return fetch(`/unseal?addr=${addr}&step=2`);
    })
    .then(r => r.text())
    .then(t => {
      resultBox.innerText += t + '\n';
      // Step 3: Full access
      return fetch(`/unseal?addr=${addr}&step=full`);
    })
    .then(r => r.text())
    .then(t => {
      resultBox.innerText += t + '\n\nUnseal sequence complete!\n\nVerifying unsealed state...';
      
      // Verification: Read OperationStatus (0x0054) — only works when unsealed
      return fetch(`/cmd?addr=${addr}&command=00&subcmd=0054`);
    })
    .then(r => r.text())
    .then(resp => {
      if (resp.includes('Word Response') || resp.includes('Block')) {
        verifyBox.innerHTML = '<span style="color:lime;font-size:1.2em;">✓ UNSEALED SUCCESSFULLY!</span><br><small>OperationStatus accessible</small>';
        resultBox.innerText += '\n\nVerification: ' + resp;
      } else {
        verifyBox.innerHTML = '<span style="color:red;font-size:1.2em;">✗ STILL SEALED</span><br><small>No access to restricted data</small>';
        resultBox.innerText += '\n\nVerification failed: ' + resp;
      }
    })
    .catch(err => {
      resultBox.innerText += '\nError during process: ' + err;
      verifyBox.innerHTML = '<span style="color:orange;">Verification failed (network error)</span>';
    });
}

  window.onload = scanI2C;
</script>

</body>
</html>
)rawliteral";


ImprovWiFi improvSerial(&Serial);

void onImprovWiFiConnectedCb(const char *ssid, const char *password)
{
  // Save ssid and password here
  WiFi.persistent(true); 

  server.begin();
  serverStarted = true;

}

void setup() {

    // Optional: Set device info (shows in browser during setup)
  improvSerial.setDeviceInfo(ImprovTypes::ChipFamily::CF_ESP32, "My-Device-9a4c2b", "2.1.5", "My Device");

  improvSerial.onImprovConnected(onImprovWiFiConnectedCb);

  Serial.begin(115200);
  while (!Serial) delay(10);
  Serial.println("\n=== ESP32 Smart Battery Monitor Started ===");


  WiFi.begin();


  Wire.begin();
  Wire.setClock(50000);

  // setupWiFi();  // <--- New function


  Serial.println("\nWiFi Connected!");
  Serial.print("IP Address: http://");
  // Serial.println(WiFi.localIP());

  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", index_html);
  });

  server.on("/data", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "application/json", getJSON());
  });

  server.on("/advanced", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, "text/html", advanced_html);
  });
// I2C Scanner endpoint
server.on("/scan", HTTP_GET, [](AsyncWebServerRequest *request) {
  String result = "I2C Scanner Results:\n\n";
  byte error;
  int found = 0;
  
  for (uint8_t addr = 0x03; addr < 0x78; addr++) {
    Wire.beginTransmission(addr);
    error = Wire.endTransmission();
    
    if (error == 0) {
      result += "Found device at 0x" + String(addr, HEX) + "\n";
      found++;
    }
  }
  
  if (found == 0) result += "No I2C devices found.\nCheck wiring!";
  else result += "\n" + String(found) + " device(s) found.";
  
  request->send(200, "text/plain", result);
});

server.on("/unseal", HTTP_GET, [](AsyncWebServerRequest *request) {
  String response = "Invalid";

  if (request->hasParam("addr") && request->hasParam("step")) {
    uint8_t addr = request->getParam("addr")->value().toInt();
    String step = request->getParam("step")->value();

    uint16_t low, high;

    if (step == "1") { low = 0x0414; high = 0x0414; }
    else if (step == "2") { low = 0x3672; high = 0x3672; }
    else if (step == "full") { low = 0xFFFF; high = 0xFFFF; }
    else { response = "Unknown step"; request->send(200, "text/plain", response); return; }

    // Write first word
    Wire.beginTransmission(addr);
    Wire.write(0x00);
    Wire.write(lowByte(low));
    Wire.write(highByte(low));
    uint8_t err1 = Wire.endTransmission();
    delay(100);

    // Write second word
    Wire.beginTransmission(addr);
    Wire.write(0x00);
    Wire.write(lowByte(high));
    Wire.write(highByte(high));
    uint8_t err2 = Wire.endTransmission();

    if (err1 == 0 && err2 == 0) {
      response = "Sent: 0x" + String(low, HEX) + " → 0x" + String(high, HEX);
    } else {
      response = "Write error: " + String(err1) + "/" + String(err2);
    }
  }

  request->send(200, "text/plain", response);
});

server.on("/cmd", HTTP_GET, [](AsyncWebServerRequest *request) {
  String response = "Invalid parameters";
  
  
  if (request->hasParam("addr") && request->hasParam("command")) {
    uint8_t addr = request->getParam("addr")->value().toInt();
    uint8_t cmd = request->getParam("command")->value().toInt();
    String type = request->hasParam("type") ? request->getParam("type")->value() : "word";

  // Optional subcommand for ManufacturerAccess (0x00)
    uint16_t subcmd = 0;
    bool isManufAccess = (cmd == 0x00);
    if (isManufAccess && request->hasParam("subcmd")) {
      subcmd = strtol(request->getParam("subcmd")->value().c_str(), nullptr, 16);
    }

    if (i2cError) recoverI2C();

    // === SAFETY: Block protected subcommands 0x1D - 0x2F ===
    // if (type == "auto" && cmd >= 29 && cmd <= 48) {
    //   response = "Protected 0x1D-0x2F range";
    //   request->send(200, "text/plain", response);
    //   return;
    // }

    // === CASE 1: Extended ManufacturerAccess with subcommand ===
    if (isManufAccess && subcmd != 0) {
      // Step 1: Write subcommand to ManufacturerAccess (0x00)
      Wire.beginTransmission(addr);
      Wire.write(0x00);
      Wire.write(lowByte(subcmd));
      Wire.write(highByte(subcmd));
      uint8_t writeErr = Wire.endTransmission();
      if (writeErr != 0) {
        response = "MA Write error: " + String(writeErr);
        request->send(200, "text/plain", response);
        return;
      }
      delay(60);  // Critical: give gauge time to process (50-100ms typical)

      // Step 2: Try reading word response directly from 0x00
      Wire.beginTransmission(addr);
      Wire.write(0x00);
      if (Wire.endTransmission(false) == 0) {
        if (Wire.requestFrom(addr, (uint8_t)2) == 2) {
          uint16_t val = Wire.read() | (Wire.read() << 8);
          char hexBuf[5];
          sprintf(hexBuf, "%04X", val);
          response = "MA Word Response (sub 0x" + String(subcmd, HEX) + "):\n0x" + String(hexBuf) + " (" + String(val) + ")";
          request->send(200, "text/plain", response + ":using 0x00");
          return;
        }
      }

      // Step 3: Fallback - read block from ManufacturerData (0x23) or try general block
      Wire.beginTransmission(addr);
      Wire.write(0x23);  // Common: ManufacturerData
      if (Wire.endTransmission(false) == 0) {
        uint8_t len = Wire.requestFrom(addr, (uint8_t)33);
        if (len > 0) {
          uint8_t blockLen = Wire.read();
          if (blockLen > 0 && blockLen <= 32) {
            String hexDump = "";
            String ascii = "";
            for (uint8_t i = 0; i < blockLen; i++) {
              uint8_t b = Wire.read();
              char byteHex[3];
              sprintf(byteHex, "%02X", b);
              hexDump += String(byteHex) + " ";
              ascii += (b >= 32 && b <= 126) ? (char)b : '.';
            }
            response = "MA Block Response (sub 0x" + String(subcmd, HEX) + "):\nLen: " + String(blockLen) +
                       "\nHex: " + hexDump + "\nASCII: " + ascii;
            request->send(200, "text/plain", response + ":using 0x23");
            return;
          }
        }
      }

      response = "No response after MA subcmd 0x" + String(subcmd, HEX);
      request->send(200, "text/plain", response);
      return;
    }


    Wire.beginTransmission(addr);
    Wire.write(cmd);
    uint8_t err = Wire.endTransmission(false);

    if (err != 0) {
      response = "Transmission error: " + String(err);
    } else {
      // Try word read first
      if (type == "word" || type == "auto") {
        if (Wire.requestFrom(addr, (uint8_t)2) == 2) {
          uint16_t val = Wire.read() | (Wire.read() << 8);
          response = "Word Response: 0x" + String(val, HEX) + " (" + String(val) + ")";
          request->send(200, "text/plain", response);
          return;
        }
      }

      // If word failed or block requested, try block
      if (type == "block" || type == "auto") {
        uint8_t len = Wire.requestFrom(addr, (uint8_t)33);
        if (len > 0) {
          uint8_t blockLen = Wire.read();
          if (blockLen == 0 || blockLen > 32) {
            response = "Invalid block length: " + String(blockLen);
          } else {
            String ascii = "";
            String hexDump = "";
            for (uint8_t i = 0; i < blockLen; i++) {
              uint8_t b = Wire.read();
              // Hex: 2-digit uppercase with space
              char byteHex[3];
              sprintf(byteHex, "%02X", b);
              hexDump += String(byteHex) + " ";

              // ASCII: printable or '.'
              ascii += (b >= 32 && b <= 126) ? (char)b : '.';
            }
            // Trim trailing space in hex
            hexDump.trim();

            response = "Block Length: " + String(blockLen) +
                      "\nHex: " + hexDump +
                      "\nString: " + ascii;
          }
        } else {
          response = "No block data received";
        }
      }
    }
  }
  
  request->send(200, "text/plain", response);
});

  // server.begin();
  Serial.println("Web server running. Open IP in browser and watch Serial for JSON logs.");
}


void loop() {
  // All handled asynchronously

  improvSerial.handleSerial();
    if (improvSerial.isConnected() && !serverStarted) {
    
    server.begin();
    serverStarted = true;
    
    }


}
