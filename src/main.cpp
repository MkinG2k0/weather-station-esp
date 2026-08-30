#include <Arduino.h>
#include <Adafruit_BME280.h>
#include <Adafruit_BMP280.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <PNGdec.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cmath>
#include <cstring>
#include <ctime>
#include <vector>

#include "driver/gpio.h"
#include "driver/rtc_io.h"

#include <GxEPD2_3C.h>

#include "GxEPD2_750c_86BF.h"
#include "secrets.h"

// FireBeetle 2 ESP32-E (DFR0654) -> DESPI-C02
// MOSI / 23 -> SDI, SCK / 18 -> SCK, D7 / 13 -> CS
// SCL / 22 -> D/C, SDA / 21 -> RES, D6 / 14 -> BUSY
constexpr int EPD_SCK = 18;
constexpr int EPD_MOSI = 23;
constexpr int EPD_CS = 13;
constexpr int EPD_DC = 22;
constexpr int EPD_RST = 21;
constexpr int EPD_BUSY = 14;

// Optional BME280/BMP280 from the wiring diagram. GPIO 12 (D13) switches the
// sensor power so the module does not consume energy while the ESP32 is sleeping.
constexpr int ENV_SENSOR_POWER = 12; // D13
constexpr int ENV_SENSOR_SCL = 16;   // D11
constexpr int ENV_SENSOR_SDA = 17;   // D10
constexpr uint32_t ENV_SENSOR_I2C_FREQUENCY = 50000;
constexpr uint16_t ENV_SENSOR_I2C_TIMEOUT_MS = 200;
constexpr uint8_t ENV_SENSOR_ADDRESS_LOW = 0x76;
constexpr uint8_t ENV_SENSOR_ADDRESS_HIGH = 0x77;
constexpr uint8_t BME280_CHIP_ID = 0x60;
constexpr uint8_t BMP280_CHIP_ID = 0x58;
constexpr uint8_t ENV_SENSOR_SAMPLE_COUNT = 3;

// FireBeetle 2 ESP32-E (DFR0654): onboard pack sense is A2 / GPIO34 through a
// 1 MΩ + 1 MΩ divider, so pack voltage is twice the ADC millivolts. A0/GPIO36
// is not wired to the PH2.0 connector. No pack → leave the query parameter
// off, same as a missing BMP/BME sensor. Max includes 4.4 V high-voltage cells.
constexpr int BATTERY_ADC_PIN = 34;
constexpr uint32_t BATTERY_DIVIDER = 2;
constexpr uint32_t BATTERY_SAMPLE_COUNT = 8;
constexpr float BATTERY_PRESENT_MIN_V = 3.05f;
constexpr float BATTERY_PRESENT_MAX_V = 4.50f;

constexpr uint32_t WIFI_TIMEOUT_MS = 30000;
constexpr uint32_t DOWNLOAD_TIMEOUT_MS = 30000;
constexpr uint32_t DEFAULT_REFRESH_INTERVAL_SECONDS = 10UL * 60UL;
constexpr uint32_t RETRY_INTERVAL_SECONDS = 30UL;
constexpr uint32_t MIN_REFRESH_INTERVAL_SECONDS = 5UL * 60UL;
constexpr uint32_t MAX_REFRESH_INTERVAL_SECONDS = 24UL * 60UL * 60UL;
constexpr size_t MAX_PNG_SIZE = 64UL * 1024UL;
constexpr int IMAGE_WIDTH = 800;
constexpr int IMAGE_HEIGHT = 480;
// Long history lives on the server. Keep a small catch-up buffer in DRAM/NVS
// so a missed screen.png still uploads recent BMP samples without overflowing
// ESP32 RAM (~8.6 KB for 1440 packed points).
constexpr uint16_t TEMP_LOG_MAX_POINTS = 48;
constexpr uint32_t TEMP_LOG_MAX_AGE_SEC = 2UL * 24UL * 3600UL;
constexpr uint16_t TEMP_LOG_SEND_POINTS = 48;
constexpr uint32_t NTP_MIN_UNIX = 1600000000UL;
constexpr int STATUS_LED = 2; // FireBeetle onboard LED, D9 / IO2

// 80 display rows per page keep the PNG decoder in static memory and avoid
// heap fragmentation. The complete 800x480 image is rendered in 6 passes.
GxEPD2_3C<GxEPD2_750c_86BF, 80> display(
    GxEPD2_750c_86BF(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
PNG* activePng = nullptr;
PNG pngDecoder;
TwoWire envI2c(1);
Adafruit_BME280 bme280;
Adafruit_BMP280 bmp280(&envI2c);
uint16_t rgbLine[IMAGE_WIDTH];
std::vector<uint8_t> downloadBuffer;

RTC_DATA_ATTR char lastScreenEtag[72] = "";
RTC_DATA_ATTR uint32_t lastImageHash = 0;
RTC_DATA_ATTR bool hasLastImageHash = false;
volatile uint8_t lastWiFiDisconnectReason = 0;

enum class DownloadResult
{
  downloaded,
  notModified,
  failed,
};

struct EnvironmentReading
{
  bool available = false;
  bool hasHumidity = false;
  float temperatureC = 0.0f;
  float pressureHpa = 0.0f;
  float humidityPercent = 0.0f;
  float altitudeM = 0.0f;
};

EnvironmentReading latestEnvironment;

struct BatteryReading
{
  bool available = false;
  float voltage = 0.0f;
  int percent = 0;
};

BatteryReading latestBattery;

struct TempSample
{
  uint32_t unix;
  int16_t tenth;
} __attribute__((packed));

TempSample tempLog[TEMP_LOG_MAX_POINTS];
uint16_t tempLogCount = 0;

void loadTemperatureLog()
{
  tempLogCount = 0;
  Preferences prefs;
  if (!prefs.begin("templog", true))
  {
    return;
  }
  const size_t length = prefs.getBytesLength("blob");
  if (length >= sizeof(TempSample) && (length % sizeof(TempSample)) == 0 &&
      length <= sizeof(tempLog))
  {
    prefs.getBytes("blob", tempLog, length);
    tempLogCount = static_cast<uint16_t>(length / sizeof(TempSample));
  }
  prefs.end();
}

void saveTemperatureLog()
{
  Preferences prefs;
  if (!prefs.begin("templog", false))
  {
    return;
  }
  prefs.putBytes("blob", tempLog, tempLogCount * sizeof(TempSample));
  prefs.end();
}

void pruneTemperatureLog(uint32_t nowUnix)
{
  uint16_t write = 0;
  for (uint16_t read = 0; read < tempLogCount; ++read)
  {
    if (nowUnix >= tempLog[read].unix && nowUnix - tempLog[read].unix <= TEMP_LOG_MAX_AGE_SEC)
    {
      tempLog[write++] = tempLog[read];
    }
  }
  tempLogCount = write;
}

void appendTemperatureSample(uint32_t nowUnix, float temperatureC)
{
  loadTemperatureLog();
  pruneTemperatureLog(nowUnix);
  const int16_t tenth = static_cast<int16_t>(lroundf(temperatureC * 10.0f));
  if (tempLogCount > 0 && tempLog[tempLogCount - 1].unix / 60 == nowUnix / 60)
  {
    tempLog[tempLogCount - 1].tenth = tenth;
    tempLog[tempLogCount - 1].unix = nowUnix;
  }
  else if (tempLogCount < TEMP_LOG_MAX_POINTS)
  {
    tempLog[tempLogCount++] = TempSample{nowUnix, tenth};
  }
  else
  {
    memmove(tempLog, tempLog + 1, (TEMP_LOG_MAX_POINTS - 1) * sizeof(TempSample));
    tempLog[TEMP_LOG_MAX_POINTS - 1] = TempSample{nowUnix, tenth};
  }
  saveTemperatureLog();
}

String encodeTemperatureHist()
{
  if (tempLogCount == 0)
  {
    loadTemperatureLog();
  }
  if (tempLogCount == 0)
  {
    return "";
  }
  const uint16_t start = tempLogCount > TEMP_LOG_SEND_POINTS ? tempLogCount - TEMP_LOG_SEND_POINTS : 0;
  String encoded;
  encoded.reserve((tempLogCount - start) * 10);
  encoded += String(tempLog[start].unix);
  encoded += ',';
  encoded += String(tempLog[start].tenth);
  for (uint16_t index = start + 1; index < tempLogCount; ++index)
  {
    int32_t minutes = static_cast<int32_t>(tempLog[index].unix - tempLog[index - 1].unix) / 60;
    if (minutes < 0)
    {
      minutes = 0;
    }
    encoded += ',';
    encoded += String(minutes);
    encoded += ',';
    encoded += String(tempLog[index].tenth);
  }
  return encoded;
}

void blinkStatusLed(int times)
{
  pinMode(STATUS_LED, OUTPUT);
  for (int i = 0; i < times; ++i)
  {
    digitalWrite(STATUS_LED, HIGH);
    delay(160);
    digitalWrite(STATUS_LED, LOW);
    delay(160);
  }
}

bool syncNetworkTime()
{
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  for (int attempt = 0; attempt < 50; ++attempt)
  {
    const time_t now = time(nullptr);
    if (now > static_cast<time_t>(NTP_MIN_UNIX))
    {
      Serial.printf("NTP unix %ld\n", static_cast<long>(now));
      return true;
    }
    delay(200);
  }
  Serial.println("NTP failed");
  return false;
}

void powerOffEnvironmentSensor()
{
  envI2c.end();
  pinMode(ENV_SENSOR_SDA, INPUT);
  pinMode(ENV_SENSOR_SCL, INPUT);
  digitalWrite(ENV_SENSOR_POWER, LOW);
  pinMode(ENV_SENSOR_POWER, OUTPUT);
}

void powerOnEnvironmentSensor()
{
  const gpio_num_t powerPin = static_cast<gpio_num_t>(ENV_SENSOR_POWER);
  pinMode(ENV_SENSOR_POWER, OUTPUT);
  digitalWrite(ENV_SENSOR_POWER, HIGH);
  gpio_set_drive_capability(powerPin, GPIO_DRIVE_CAP_3);
  if (rtc_gpio_is_valid_gpio(powerPin))
  {
    rtc_gpio_hold_dis(powerPin);
  }
}

void recoverI2cBus()
{
  envI2c.end();
  pinMode(ENV_SENSOR_SDA, INPUT_PULLUP);
  pinMode(ENV_SENSOR_SCL, OUTPUT);
  for (int pulse = 0; pulse < 16; ++pulse)
  {
    digitalWrite(ENV_SENSOR_SCL, HIGH);
    delayMicroseconds(10);
    if (digitalRead(ENV_SENSOR_SDA) != LOW)
    {
      break;
    }
    digitalWrite(ENV_SENSOR_SCL, LOW);
    delayMicroseconds(10);
  }
  pinMode(ENV_SENSOR_SDA, OUTPUT);
  digitalWrite(ENV_SENSOR_SDA, LOW);
  digitalWrite(ENV_SENSOR_SCL, HIGH);
  delayMicroseconds(10);
  digitalWrite(ENV_SENSOR_SDA, HIGH);
  delayMicroseconds(10);
  pinMode(ENV_SENSOR_SDA, INPUT_PULLUP);
  pinMode(ENV_SENSOR_SCL, INPUT_PULLUP);
}

void logSensorPins(const char* when)
{
  Serial.printf("Sensor pins %s: PWR=%d SDA=%d SCL=%d\n", when,
                digitalRead(ENV_SENSOR_POWER), digitalRead(ENV_SENSOR_SDA),
                digitalRead(ENV_SENSOR_SCL));
}

bool startEnvI2c(int sda, int scl)
{
  envI2c.end();
  envI2c.setPins(sda, scl);
  if (!envI2c.begin(sda, scl, ENV_SENSOR_I2C_FREQUENCY))
  {
    return false;
  }
  envI2c.setTimeOut(ENV_SENSOR_I2C_TIMEOUT_MS);
  envI2c.setClock(ENV_SENSOR_I2C_FREQUENCY);
  gpio_pullup_en(static_cast<gpio_num_t>(sda));
  gpio_pullup_en(static_cast<gpio_num_t>(scl));
  return true;
}

uint8_t probeI2cAddress(uint8_t address)
{
  envI2c.beginTransmission(address);
  return envI2c.endTransmission();
}

uint8_t scanI2cBus(uint8_t* found, uint8_t maxFound)
{
  uint8_t count = 0;
  Serial.print("I2C scan:");
  for (uint8_t address = 0x08; address < 0x78; ++address)
  {
    if (probeI2cAddress(address) != 0)
    {
      continue;
    }
    Serial.printf(" 0x%02X", address);
    if (count < maxFound)
    {
      found[count] = address;
    }
    ++count;
  }
  if (count == 0)
  {
    Serial.print(" none");
  }
  Serial.println();
  return count;
}

uint8_t readBmpChipId(uint8_t address)
{
  envI2c.beginTransmission(address);
  envI2c.write(0xD0);
  if (envI2c.endTransmission() != 0)
  {
    return 0;
  }
  if (envI2c.requestFrom(static_cast<int>(address), 1) != 1)
  {
    return 0;
  }
  return static_cast<uint8_t>(envI2c.read());
}

float median3(float a, float b, float c)
{
  if (a > b)
  {
    const float t = a;
    a = b;
    b = t;
  }
  if (b > c)
  {
    const float t = b;
    b = c;
    c = t;
  }
  if (a > b)
  {
    const float t = a;
    a = b;
    b = t;
  }
  return b;
}

float altitudeFromPressureHpa(float pressureHpa)
{
  return 44330.0f * (1.0f - powf(pressureHpa / 1013.25f, 0.1903f));
}

void finishEnvironmentReading(bool hasHumidity, const float* temperaturesC, const float* pressuresHpa,
                              const float* humiditiesPercent, uint8_t sampleCount)
{
  if (sampleCount == 0)
  {
    return;
  }
  latestEnvironment.available = true;
  latestEnvironment.hasHumidity = hasHumidity;
  if (sampleCount == 1)
  {
    latestEnvironment.temperatureC = temperaturesC[0];
    latestEnvironment.pressureHpa = pressuresHpa[0];
    latestEnvironment.humidityPercent = hasHumidity ? humiditiesPercent[0] : 0.0f;
  }
  else if (sampleCount == 2)
  {
    latestEnvironment.temperatureC = (temperaturesC[0] + temperaturesC[1]) * 0.5f;
    latestEnvironment.pressureHpa = (pressuresHpa[0] + pressuresHpa[1]) * 0.5f;
    latestEnvironment.humidityPercent =
        hasHumidity ? (humiditiesPercent[0] + humiditiesPercent[1]) * 0.5f : 0.0f;
  }
  else
  {
    latestEnvironment.temperatureC = median3(temperaturesC[0], temperaturesC[1], temperaturesC[2]);
    latestEnvironment.pressureHpa = median3(pressuresHpa[0], pressuresHpa[1], pressuresHpa[2]);
    latestEnvironment.humidityPercent =
        hasHumidity ? median3(humiditiesPercent[0], humiditiesPercent[1], humiditiesPercent[2]) : 0.0f;
  }
  latestEnvironment.altitudeM = altitudeFromPressureHpa(latestEnvironment.pressureHpa);
  Serial.printf("Sensor samples: %u (%s)\n", sampleCount,
                sampleCount >= 3 ? "median" : "partial");
}

void logSensorReading()
{
  if (!latestEnvironment.available)
  {
    Serial.println("Sensor data: none (card on server will show no device data)");
    return;
  }
  Serial.printf("Sensor data: chip=%s temp_c=%.2f pressure_hpa=%.2f altitude_m=%.1f",
                latestEnvironment.hasHumidity ? "bme280" : "bmp280",
                latestEnvironment.temperatureC, latestEnvironment.pressureHpa,
                latestEnvironment.altitudeM);
  if (latestEnvironment.hasHumidity)
  {
    Serial.printf(" humidity=%.1f", latestEnvironment.humidityPercent);
  }
  Serial.println();
}

int lipoPercentFromVoltage(float volts)
{
  static const float table[][2] = {
      {4.20f, 100.0f}, {4.15f, 95.0f}, {4.11f, 90.0f}, {4.08f, 85.0f},
      {4.02f, 80.0f},  {3.98f, 75.0f}, {3.95f, 70.0f}, {3.91f, 65.0f},
      {3.87f, 60.0f},  {3.85f, 55.0f}, {3.84f, 50.0f}, {3.82f, 45.0f},
      {3.80f, 40.0f},  {3.79f, 35.0f}, {3.77f, 30.0f}, {3.75f, 25.0f},
      {3.73f, 20.0f},  {3.71f, 15.0f}, {3.69f, 10.0f}, {3.61f, 5.0f},
      {3.30f, 0.0f},
  };
  constexpr size_t count = sizeof(table) / sizeof(table[0]);
  if (volts >= table[0][0])
  {
    return 100;
  }
  if (volts <= table[count - 1][0])
  {
    return 0;
  }
  for (size_t i = 0; i + 1 < count; ++i)
  {
    if (volts <= table[i][0] && volts >= table[i + 1][0])
    {
      const float span = table[i][0] - table[i + 1][0];
      const float t = span > 0.0f ? (volts - table[i + 1][0]) / span : 0.0f;
      return static_cast<int>(table[i + 1][1] + t * (table[i][1] - table[i + 1][1]) + 0.5f);
    }
  }
  return 0;
}

void readOptionalBattery()
{
  latestBattery = BatteryReading{};
  analogSetPinAttenuation(BATTERY_ADC_PIN, ADC_11db);
  analogReadMilliVolts(BATTERY_ADC_PIN);
  delay(8);

  uint64_t milliSum = 0;
  for (uint32_t sample = 0; sample < BATTERY_SAMPLE_COUNT; ++sample)
  {
    milliSum += analogReadMilliVolts(BATTERY_ADC_PIN);
    delay(4);
  }
  const float pinVolts = (milliSum / static_cast<float>(BATTERY_SAMPLE_COUNT)) / 1000.0f;
  const float packVolts = pinVolts * static_cast<float>(BATTERY_DIVIDER);
  const int percent = lipoPercentFromVoltage(packVolts);

  if (packVolts < BATTERY_PRESENT_MIN_V || packVolts > BATTERY_PRESENT_MAX_V)
  {
    Serial.printf("Battery: not present (%.2f V on GPIO %d pack sense -> %d%%)\n",
                  packVolts, BATTERY_ADC_PIN, percent);
    return;
  }

  latestBattery.available = true;
  latestBattery.voltage = packVolts;
  latestBattery.percent = percent;
  Serial.printf("Battery: %.2f V -> %d%% (GPIO %d)\n", latestBattery.voltage,
                latestBattery.percent, BATTERY_ADC_PIN);
}

void readOptionalEnvironmentSensor()
{
  latestEnvironment = EnvironmentReading{};

  Serial.printf("Sensor bus: SDA=GPIO %d, SCL=GPIO %d, PWR=GPIO %d\n",
                ENV_SENSOR_SDA, ENV_SENSOR_SCL, ENV_SENSOR_POWER);

  powerOnEnvironmentSensor();
  delay(250);
  pinMode(ENV_SENSOR_SDA, INPUT_PULLUP);
  pinMode(ENV_SENSOR_SCL, INPUT_PULLUP);
  logSensorPins("after power-on");
  if (digitalRead(ENV_SENSOR_SDA) == LOW || digitalRead(ENV_SENSOR_SCL) == LOW)
  {
    recoverI2cBus();
    logSensorPins("after bus recover");
  }
  if (digitalRead(ENV_SENSOR_POWER) == LOW || digitalRead(ENV_SENSOR_SDA) == LOW ||
      digitalRead(ENV_SENSOR_SCL) == LOW)
  {
    Serial.println("I2C lines are held low; BMP VIN is probably not getting 3.3V from GPIO 12");
    Serial.println("Try wiring BMP VIN to FireBeetle 3V3 instead of D13, then reset");
  }

  int sda = ENV_SENSOR_SDA;
  int scl = ENV_SENSOR_SCL;
  if (!startEnvI2c(sda, scl))
  {
    Serial.println("Optional sensor: I2C initialization failed; continuing without sensor");
    logSensorReading();
    powerOffEnvironmentSensor();
    return;
  }

  uint8_t found[4] = {};
  uint8_t foundCount = scanI2cBus(found, 4);
  if (foundCount == 0)
  {
    Serial.println("Retrying I2C with SDA/SCL swapped");
    sda = ENV_SENSOR_SCL;
    scl = ENV_SENSOR_SDA;
    if (!startEnvI2c(sda, scl))
    {
      Serial.println("Optional sensor: I2C initialization failed; continuing without sensor");
      logSensorReading();
      powerOffEnvironmentSensor();
      return;
    }
    foundCount = scanI2cBus(found, 4);
  }

  uint8_t address = 0;
  for (uint8_t i = 0; i < foundCount && i < 4; ++i)
  {
    if (found[i] == ENV_SENSOR_ADDRESS_LOW || found[i] == ENV_SENSOR_ADDRESS_HIGH)
    {
      address = found[i];
      break;
    }
  }
  if (address == 0 && foundCount > 0)
  {
    address = found[0];
  }

  if (address == 0)
  {
    Serial.println("No I2C device on D10/D11. Leave SDA and SCL connected to the sensor and reset");
    logSensorReading();
    powerOffEnvironmentSensor();
    return;
  }

  const uint8_t chipId = readBmpChipId(address);
  Serial.printf("Sensor ACK at 0x%02X, chip id 0x%02X\n", address, chipId);
  if (chipId == BME280_CHIP_ID && bme280.begin(address, &envI2c))
  {
    bme280.setSampling(Adafruit_BME280::MODE_FORCED, Adafruit_BME280::SAMPLING_X1,
                       Adafruit_BME280::SAMPLING_X1, Adafruit_BME280::SAMPLING_X1,
                       Adafruit_BME280::FILTER_OFF);
    float temperaturesC[ENV_SENSOR_SAMPLE_COUNT] = {};
    float pressuresHpa[ENV_SENSOR_SAMPLE_COUNT] = {};
    float humiditiesPercent[ENV_SENSOR_SAMPLE_COUNT] = {};
    uint8_t sampleCount = 0;
    for (uint8_t i = 0; i < ENV_SENSOR_SAMPLE_COUNT; ++i)
    {
      if (!bme280.takeForcedMeasurement())
      {
        continue;
      }
      temperaturesC[sampleCount] = bme280.readTemperature();
      pressuresHpa[sampleCount] = bme280.readPressure() / 100.0f;
      humiditiesPercent[sampleCount] = bme280.readHumidity();
      sampleCount++;
    }
    finishEnvironmentReading(true, temperaturesC, pressuresHpa, humiditiesPercent, sampleCount);
  }
  else if ((chipId == BMP280_CHIP_ID || chipId == 0) && bmp280.begin(address))
  {
    bmp280.setSampling(Adafruit_BMP280::MODE_FORCED, Adafruit_BMP280::SAMPLING_X1,
                       Adafruit_BMP280::SAMPLING_X1, Adafruit_BMP280::FILTER_OFF,
                       Adafruit_BMP280::STANDBY_MS_1);
    float temperaturesC[ENV_SENSOR_SAMPLE_COUNT] = {};
    float pressuresHpa[ENV_SENSOR_SAMPLE_COUNT] = {};
    float humiditiesPercent[ENV_SENSOR_SAMPLE_COUNT] = {};
    uint8_t sampleCount = 0;
    for (uint8_t i = 0; i < ENV_SENSOR_SAMPLE_COUNT; ++i)
    {
      if (!bmp280.takeForcedMeasurement())
      {
        continue;
      }
      temperaturesC[sampleCount] = bmp280.readTemperature();
      pressuresHpa[sampleCount] = bmp280.readPressure() / 100.0f;
      sampleCount++;
    }
    finishEnvironmentReading(false, temperaturesC, pressuresHpa, humiditiesPercent, sampleCount);
  }
  else
  {
    Serial.printf("I2C device at 0x%02X (chip id 0x%02X) is not a supported BME280/BMP280\n",
                  address, chipId);
  }

  logSensorReading();
  powerOffEnvironmentSensor();
}

void showError(const String& message, const String& details = "")
{
  Serial.printf("ERROR: %s", message.c_str());
  if (!details.isEmpty())
  {
    Serial.printf(" - %s", details.c_str());
  }
  Serial.println();

  // E-Ink keeps the last image without power, so leave a useful diagnostic on
  // the device while it sleeps and retries.
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200);
  display.setRotation(0);
  display.setFullWindow();
  display.setTextColor(GxEPD_BLACK);
  display.setTextWrap(true);
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    display.setTextSize(4);
    display.setCursor(32, 70);
    display.println("ERROR");
    display.drawFastHLine(32, 90, IMAGE_WIDTH - 64, GxEPD_BLACK);
    display.setTextSize(3);
    display.setCursor(32, 145);
    display.println(message);
    if (!details.isEmpty())
    {
      display.setTextSize(2);
      display.setCursor(32, 245);
      display.println(details);
    }
    display.setTextSize(2);
    display.setCursor(32, 430);
    display.println("The device will retry automatically.");
  }
  while (display.nextPage());
  display.hibernate();
  SPI.end();
}

void recordWiFiDisconnectReason(arduino_event_id_t event, arduino_event_info_t info)
{
  if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED)
  {
    lastWiFiDisconnectReason = info.wifi_sta_disconnected.reason;
  }
}

String wiFiErrorMessage(wl_status_t status, uint8_t reason)
{
  switch (reason)
  {
    case WIFI_REASON_NO_AP_FOUND:
      return "Wi-Fi network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED:
      return "Wi-Fi authentication failed (wrong password)";
    case WIFI_REASON_BEACON_TIMEOUT:
      return "Wi-Fi signal was lost";
    default:
      break;
  }

  switch (status)
  {
    case WL_NO_SSID_AVAIL:
      return "Wi-Fi network not found";
    case WL_CONNECT_FAILED:
      return "Wi-Fi authentication failed (wrong password)";
    case WL_CONNECTION_LOST:
      return "Wi-Fi connection lost";
    case WL_DISCONNECTED:
      return "Wi-Fi disconnected or password is incorrect";
    default:
      return "Wi-Fi connection timed out";
  }
}

class VectorWriteStream : public Stream
{
public:
  explicit VectorWriteStream(std::vector<uint8_t>& target) : target_(target) {}
  int available() override { return 0; }
  int read() override { return -1; }
  int peek() override { return -1; }
  void flush() override {}
  size_t write(uint8_t value) override { return write(&value, 1); }
  size_t write(const uint8_t* data, size_t length) override
  {
    if (length > MAX_PNG_SIZE - target_.size())
    {
      overflowed_ = true;
      return 0;
    }
    const size_t offset = target_.size();
    target_.resize(offset + length);
    memcpy(target_.data() + offset, data, length);
    return length;
  }
  bool overflowed() const { return overflowed_; }

private:
  std::vector<uint8_t>& target_;
  bool overflowed_ = false;
};

void appendQueryParam(String& query, const char* key, const String& value)
{
  query += query.length() == 0 ? '?' : '&';
  query += key;
  query += '=';
  query += value;
}

String screenshotQuery()
{
  String query;
  if (latestEnvironment.available)
  {
    appendQueryParam(query, "chip", latestEnvironment.hasHumidity ? "bme280" : "bmp280");
    appendQueryParam(query, "temp_c", String(latestEnvironment.temperatureC, 2));
    appendQueryParam(query, "pressure_hpa", String(latestEnvironment.pressureHpa, 2));
    appendQueryParam(query, "altitude_m", String(latestEnvironment.altitudeM, 1));
    if (latestEnvironment.hasHumidity)
    {
      appendQueryParam(query, "humidity", String(latestEnvironment.humidityPercent, 1));
    }
  }
  if (latestBattery.available)
  {
    appendQueryParam(query, "batt_pct", String(latestBattery.percent));
  }
  const String hist = encodeTemperatureHist();
  if (hist.length() > 0)
  {
    appendQueryParam(query, "hist", hist);
  }
  return query;
}

bool connectWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    return true;
  }

  Serial.printf("Connecting to Wi-Fi %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  // Modem sleep drops TLS handshake packets and shows up as start_ssl_client: -1.
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(false);
  lastWiFiDisconnectReason = 0;
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < WIFI_TIMEOUT_MS)
  {
    delay(500);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED)
  {
    const wl_status_t status = WiFi.status();
    const uint8_t reason = lastWiFiDisconnectReason;
    const String details = "Status: " + String(static_cast<int>(status)) +
                           ", disconnect reason: " + String(reason);
    showError(wiFiErrorMessage(status, reason), details);
    return false;
  }

  Serial.print("Wi-Fi connected, IP: ");
  Serial.println(WiFi.localIP());
  Serial.printf("Free heap before HTTPS: %u bytes\n",
                static_cast<unsigned>(ESP.getFreeHeap()));
  delay(250);
  return true;
}

void stopWiFi()
{
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

uint32_t fnv1a(const std::vector<uint8_t>& data)
{
  uint32_t hash = 2166136261UL;
  for (const uint8_t value : data)
  {
    hash ^= value;
    hash *= 16777619UL;
  }
  return hash;
}

void rememberEtag(const String& etag)
{
  if (etag.isEmpty())
  {
    lastScreenEtag[0] = '\0';
    return;
  }
  strncpy(lastScreenEtag, etag.c_str(), sizeof(lastScreenEtag) - 1);
  lastScreenEtag[sizeof(lastScreenEtag) - 1] = '\0';
}

void applyRefreshHeader(const String& value, uint32_t& refreshSeconds)
{
  const long seconds = value.toInt();
  if (seconds >= static_cast<long>(MIN_REFRESH_INTERVAL_SECONDS) &&
      seconds <= static_cast<long>(MAX_REFRESH_INTERVAL_SECONDS))
  {
    refreshSeconds = static_cast<uint32_t>(seconds);
  }
}

DownloadResult downloadScreenshot(const String& screenshotUrl, std::vector<uint8_t>& image,
                                  String& responseEtag, uint32_t& refreshSeconds)
{
  WiFiClientSecure client;
  client.setInsecure(); // Test firmware: accept Vercel's current TLS certificate chain.
  client.setHandshakeTimeout(30);

  HTTPClient http;
  http.setConnectTimeout(25000);
  http.setTimeout(DOWNLOAD_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const char* responseHeaders[] = {"Content-Type", "ETag", "X-Next-Refresh-Seconds"};
  http.collectHeaders(responseHeaders, 3);

  Serial.printf("GET %s\n", screenshotUrl.c_str());
  int status = -1;
  for (int attempt = 1; attempt <= 3; ++attempt)
  {
    if (!http.begin(client, screenshotUrl))
    {
      showError("Invalid or unsupported link", screenshotUrl);
      return DownloadResult::failed;
    }
    if (lastScreenEtag[0] != '\0')
    {
      http.addHeader("If-None-Match", lastScreenEtag);
    }

    status = http.GET();
    if (status > 0)
    {
      break;
    }
    Serial.printf("HTTPS attempt %d failed: %d (%s), heap %u\n", attempt, status,
                  http.errorToString(status).c_str(),
                  static_cast<unsigned>(ESP.getFreeHeap()));
    http.end();
    delay(750 * attempt);
  }
  if (status <= 0)
  {
    String details = "HTTP status: " + String(status) + " (" + http.errorToString(status) + ")";
    showError("Server connection failed", details);
    return DownloadResult::failed;
  }
  applyRefreshHeader(http.header("X-Next-Refresh-Seconds"), refreshSeconds);
  if (status == HTTP_CODE_NOT_MODIFIED)
  {
    Serial.printf("Screen unchanged (ETag), next check in %lu seconds\n",
                  static_cast<unsigned long>(refreshSeconds));
    http.end();
    return DownloadResult::notModified;
  }
  if (status != HTTP_CODE_OK)
  {
    String message;
    if (status == HTTP_CODE_NOT_FOUND)
    {
      message = "Link not found";
    }
    else if (status < 0)
    {
      message = "Server connection failed";
    }
    else
    {
      message = "Server returned an error";
    }
    String details = "HTTP status: " + String(status);
    if (status < 0)
    {
      details += " (" + http.errorToString(status) + ")";
    }
    http.end();
    showError(message, details);
    return DownloadResult::failed;
  }

  responseEtag = http.header("ETag");
  const int contentLength = http.getSize();
  const String contentType = http.header("Content-Type");
  Serial.printf("HTTP 200, Content-Type: %s, Content-Length: %d\n",
                contentType.c_str(), contentLength);
  if (contentLength > 0 && static_cast<size_t>(contentLength) > MAX_PNG_SIZE)
  {
    http.end();
    showError("Image is too large", "Content-Length: " + String(contentLength) + " bytes");
    return DownloadResult::failed;
  }
  if (!contentType.startsWith("image/png"))
  {
    http.end();
    showError("Link did not return a PNG image", "Content-Type: " + contentType);
    return DownloadResult::failed;
  }

  image.clear();
  if (contentLength < 0)
  {
    // Vercel commonly uses chunked transfer encoding, where Content-Length is -1.
    // writeToStream() decodes chunks directly into the preallocated PNG buffer.
    Serial.println("Downloading chunked PNG response");
    VectorWriteStream output(image);
    const int written = http.writeToStream(&output);
    http.end();
    if (written <= 0 || output.overflowed() || image.empty())
    {
      const String details = "Result: " + String(written) + ", received: " +
                             String(static_cast<unsigned>(image.size())) + " bytes";
      image.clear();
      showError(output.overflowed() ? "Downloaded image is too large" : "Image download failed",
                details);
      return DownloadResult::failed;
    }
  }
  else
  {
    image.resize(contentLength);
    WiFiClient* stream = http.getStreamPtr();
    size_t received = 0;
    uint32_t lastDataAt = millis();

    while (received < image.size())
    {
      const size_t available = stream->available();
      if (available > 0)
      {
        const size_t remaining = image.size() - received;
        const size_t chunk = available < remaining ? available : remaining;
        const size_t read = stream->readBytes(image.data() + received, chunk);
        received += read;
        lastDataAt = millis();
        continue;
      }

      if (!stream->connected() || millis() - lastDataAt >= DOWNLOAD_TIMEOUT_MS)
      {
        break;
      }
      delay(1);
    }
    http.end();

    if (received != image.size())
    {
      const String details = "Received " + String(static_cast<unsigned>(received)) + " of " +
                             String(static_cast<unsigned>(image.size())) + " bytes";
      image.clear();
      showError("Image download was interrupted", details);
      return DownloadResult::failed;
    }
  }

  const bool hasPngSignature = image.size() >= 8 && image[0] == 0x89 &&
                               image[1] == 'P' && image[2] == 'N' && image[3] == 'G';
  if (!hasPngSignature)
  {
    image.clear();
    showError("Downloaded file is not a valid PNG image");
    return DownloadResult::failed;
  }

  Serial.printf("PNG downloaded: %u bytes\n", static_cast<unsigned>(image.size()));
  return DownloadResult::downloaded;
}

int drawPngLine(PNGDRAW* draw)
{
  activePng->getLineAsRGB565(draw, rgbLine, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);

  for (int x = 0; x < draw->iWidth; ++x)
  {
    const uint16_t pixel = rgbLine[x];
    const uint16_t red = (pixel >> 11) & 0x1f;
    const uint16_t green = (pixel >> 5) & 0x3f;
    const uint16_t blue = pixel & 0x1f;
    const uint16_t luminance = red * 299U + green * 293U + blue * 114U;
    const uint16_t color = luminance < 15800U ? GxEPD_BLACK : GxEPD_WHITE;
    display.drawPixel(x, draw->y, color);
  }
  return 1;
}

bool showPng(const std::vector<uint8_t>& image)
{
  activePng = &pngDecoder;
  const int result = activePng->openRAM(
      const_cast<uint8_t*>(image.data()), image.size(), drawPngLine);
  if (result != PNG_SUCCESS)
  {
    activePng = nullptr;
    showError("Cannot open PNG image", "PNG error code: " + String(result));
    return false;
  }

  if (activePng->getWidth() != IMAGE_WIDTH || activePng->getHeight() != IMAGE_HEIGHT)
  {
    const String details = "Expected 800x480, received " + String(activePng->getWidth()) + "x" +
                           String(activePng->getHeight());
    activePng->close();
    activePng = nullptr;
    showError("Invalid PNG dimensions", details);
    return false;
  }

  Serial.println("Rendering PNG to E-Ink framebuffer");
  SPI.begin(EPD_SCK, -1, EPD_MOSI, EPD_CS);
  display.init(115200);
  display.setRotation(0);
  display.setFullWindow();
  display.firstPage();
  do
  {
    display.fillScreen(GxEPD_WHITE);
    // Decode the full PNG for every display page. GxEPD2 clips pixels to the
    // active 80-row page, keeping RAM use low while preserving all 800x480 pixels.
    const int decodeResult = activePng->decode(nullptr, 0);
    if (decodeResult != PNG_SUCCESS)
    {
      activePng->close();
      activePng = nullptr;
      display.hibernate();
      SPI.end();
      showError("Cannot decode PNG image", "PNG error code: " + String(decodeResult));
      return false;
    }
  }
  while (display.nextPage());

  activePng->close();
  activePng = nullptr;
  display.hibernate();
  SPI.end();
  Serial.println("E-Ink refresh complete");
  return true;
}

bool refreshScreen(uint32_t& refreshSeconds)
{
  if (DEVICE_BASE_URL[0] == '\0')
  {
    showError("Device link is missing", "Set DEVICE_BASE_URL in secrets.h");
    return false;
  }

  if (!connectWiFi())
  {
    return false;
  }

  loadTemperatureLog();
  if (syncNetworkTime() && latestEnvironment.available)
  {
    appendTemperatureSample(static_cast<uint32_t>(time(nullptr)), latestEnvironment.temperatureC);
  }

  const String screenshotUrl = String(DEVICE_BASE_URL) + "/screen.png" + screenshotQuery();
  logSensorReading();
  String responseEtag;
  const DownloadResult result = downloadScreenshot(
      screenshotUrl, downloadBuffer, responseEtag, refreshSeconds);
  // The E-Ink refresh takes about 25 seconds and does not need the radio.
  stopWiFi();
  if (result == DownloadResult::failed)
  {
    return false;
  }
  if (result == DownloadResult::notModified)
  {
    return true;
  }

  const uint32_t imageHash = fnv1a(downloadBuffer);
  if (hasLastImageHash && imageHash == lastImageHash)
  {
    Serial.println("Downloaded PNG is identical; skipping E-Ink refresh");
    rememberEtag(responseEtag);
    return true;
  }
  if (!showPng(downloadBuffer))
  {
    return false;
  }

  lastImageHash = imageHash;
  hasLastImageHash = true;
  rememberEtag(responseEtag);
  return true;
}

[[noreturn]] void sleepFor(uint32_t seconds)
{
  stopWiFi();
  digitalWrite(STATUS_LED, LOW);
  Serial.printf("Deep sleep for %lu seconds\n", static_cast<unsigned long>(seconds));
  Serial.flush();
  esp_sleep_enable_timer_wakeup(static_cast<uint64_t>(seconds) * 1000000ULL);
  esp_deep_sleep_start();
  while (true) {}
}

void setup()
{
  Serial.begin(115200);
  delay(200);
  blinkStatusLed(3);
  Serial.println("FPC-8612 Wi-Fi weather display: start");
  WiFi.onEvent(recordWiFiDisconnectReason, ARDUINO_EVENT_WIFI_STA_DISCONNECTED);

  // Sensor, reset button and battery are optional. Reset is hardware-only;
  // charging is onboard. Firmware only reads the sensor and battery ADC.
  readOptionalEnvironmentSensor();
  readOptionalBattery();

  // Allocate the download buffer before Wi-Fi/TLS can fragment the heap.
  downloadBuffer.reserve(MAX_PNG_SIZE);
  Serial.printf("PNG buffer reserved: %u bytes\n",
                static_cast<unsigned>(downloadBuffer.capacity()));

  uint32_t refreshSeconds = DEFAULT_REFRESH_INTERVAL_SECONDS;
  const bool refreshed = refreshScreen(refreshSeconds);
  sleepFor(refreshed ? refreshSeconds : RETRY_INTERVAL_SECONDS);
}

void loop()
{
}
