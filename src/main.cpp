#include <Arduino.h>
#include <HTTPClient.h>
#include <PNGdec.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <cstring>
#include <vector>

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

constexpr uint32_t WIFI_TIMEOUT_MS = 30000;
constexpr uint32_t DOWNLOAD_TIMEOUT_MS = 30000;
constexpr uint32_t DEFAULT_REFRESH_INTERVAL_SECONDS = 10UL * 60UL;
constexpr uint32_t RETRY_INTERVAL_SECONDS = 30UL;
constexpr uint32_t MIN_REFRESH_INTERVAL_SECONDS = 5UL * 60UL;
constexpr uint32_t MAX_REFRESH_INTERVAL_SECONDS = 24UL * 60UL * 60UL;
constexpr size_t MAX_PNG_SIZE = 64UL * 1024UL;
constexpr int IMAGE_WIDTH = 800;
constexpr int IMAGE_HEIGHT = 480;

// 80 display rows per page keep the PNG decoder in static memory and avoid
// heap fragmentation. The complete 800x480 image is rendered in 6 passes.
GxEPD2_3C<GxEPD2_750c_86BF, 80> display(
    GxEPD2_750c_86BF(EPD_CS, EPD_DC, EPD_RST, EPD_BUSY));
PNG* activePng = nullptr;
PNG pngDecoder;
uint16_t rgbLine[IMAGE_WIDTH];
std::vector<uint8_t> downloadBuffer;

RTC_DATA_ATTR char lastScreenEtag[72] = "";
RTC_DATA_ATTR uint32_t lastImageHash = 0;
RTC_DATA_ATTR bool hasLastImageHash = false;

enum class DownloadResult
{
  downloaded,
  notModified,
  failed,
};

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

bool connectWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    return true;
  }

  Serial.printf("Connecting to Wi-Fi %s", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);
  WiFi.setAutoReconnect(false);
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
    Serial.printf("Wi-Fi connection failed, status=%d\n", WiFi.status());
    return false;
  }

  Serial.print("Wi-Fi connected, IP: ");
  Serial.println(WiFi.localIP());
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

  HTTPClient http;
  http.setConnectTimeout(15000);
  http.setTimeout(DOWNLOAD_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  const char* responseHeaders[] = {"Content-Type", "ETag", "X-Next-Refresh-Seconds"};
  http.collectHeaders(responseHeaders, 3);

  Serial.printf("GET %s\n", screenshotUrl.c_str());
  if (!http.begin(client, screenshotUrl))
  {
    Serial.println("HTTPS initialization failed");
    return DownloadResult::failed;
  }
  if (lastScreenEtag[0] != '\0')
  {
    http.addHeader("If-None-Match", lastScreenEtag);
  }

  const int status = http.GET();
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
    Serial.printf("HTTP error: %d (%s)\n", status, http.errorToString(status).c_str());
    http.end();
    return DownloadResult::failed;
  }

  responseEtag = http.header("ETag");
  const int contentLength = http.getSize();
  const String contentType = http.header("Content-Type");
  Serial.printf("HTTP 200, Content-Type: %s, Content-Length: %d\n",
                contentType.c_str(), contentLength);
  if (contentLength > 0 && static_cast<size_t>(contentLength) > MAX_PNG_SIZE)
  {
    Serial.printf("Invalid Content-Length: %d\n", contentLength);
    http.end();
    return DownloadResult::failed;
  }
  if (!contentType.startsWith("image/png"))
  {
    Serial.printf("Unexpected Content-Type: %s\n", contentType.c_str());
    http.end();
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
      Serial.printf("Chunked download failed: result=%d, size=%u\n", written,
                    static_cast<unsigned>(image.size()));
      image.clear();
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
      Serial.printf("Incomplete download: %u/%u bytes\n",
                    static_cast<unsigned>(received), static_cast<unsigned>(image.size()));
      image.clear();
      return DownloadResult::failed;
    }
  }

  const bool hasPngSignature = image.size() >= 8 && image[0] == 0x89 &&
                               image[1] == 'P' && image[2] == 'N' && image[3] == 'G';
  if (!hasPngSignature)
  {
    Serial.println("Downloaded data is not a PNG file");
    image.clear();
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
    Serial.printf("PNG open error: %d\n", result);
    activePng = nullptr;
    return false;
  }

  if (activePng->getWidth() != IMAGE_WIDTH || activePng->getHeight() != IMAGE_HEIGHT)
  {
    Serial.printf("Unexpected PNG dimensions: %dx%d\n",
                  activePng->getWidth(), activePng->getHeight());
    activePng->close();
    activePng = nullptr;
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
      Serial.printf("PNG decode error: %d\n", decodeResult);
      activePng->close();
      activePng = nullptr;
      display.hibernate();
      SPI.end();
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
  if (!connectWiFi())
  {
    return false;
  }

  if (DEVICE_BASE_URL[0] == '\0')
  {
    Serial.println("DEVICE_BASE_URL is empty; copy it from E-Ink Control Desk");
    return false;
  }

  const String screenshotUrl = String(DEVICE_BASE_URL) + "/screen.png";
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
  Serial.println("FPC-8612 Wi-Fi weather display: start");

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
