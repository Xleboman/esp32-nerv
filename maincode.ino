#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <U8g2_for_Adafruit_GFX.h>

#include <NimBLEDevice.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Настройки Wi-Fi и погоды
const char* HOME_WIFI_SSID = "ENTER_YOUR_WIFI_NAME";
const char* HOME_WIFI_PASSWORD = "ENTER_YOUR_WIFI_PASSWORD";

const char* PHONE_WIFI_SSID = "ENTER_YOUR_WIFI_NAME";
const char* PHONE_WIFI_PASSWORD = "ENTER_YOUR_WIFI_PASSWORD";

const float WEATHER_LATITUDE = 55.008785;
const float WEATHER_LONGITUDE = 73.3267;
const char* WEATHER_TIMEZONE = "Asia/Omsk";

const uint32_t WIFI_CONNECT_TIMEOUT_MS = 15000;
const uint32_t WIFI_RETRY_DELAY_MS = 1500;
const uint32_t WEATHER_UPDATE_INTERVAL_MS = 10UL * 60UL * 1000UL;
const uint32_t WEATHER_RETRY_INTERVAL_MS = 60UL * 1000UL;
const uint32_t MUSIC_PAUSE_HOLD_MS = 90000;
const uint32_t PROGRESS_REFRESH_MS = 250;

// Дисплей ST7789V
#define TFT_CS    27
#define TFT_DC    26
#define TFT_RST   25

#define TFT_SCK   18
#define TFT_MISO  19
#define TFT_MOSI  23

Adafruit_ST7789 tft(TFT_CS, TFT_DC, TFT_RST);
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

#define NERV_RED     0xF800
#define NERV_GRAY    0x4208
#define NERV_WHITE   0xFFFF
#define NERV_BLACK   0x0000
#define NERV_CYAN    0x07FF

// Apple Media Service
static NimBLEUUID AMS_SERVICE_UUID(
  "89D3502B-0F36-433A-8EF4-C502AD55F8DC"
);

static NimBLEUUID AMS_ENTITY_UPDATE_UUID(
  "2F7CABCE-808D-411F-9A0C-BB92BA96C102"
);

static NimBLEUUID AMS_ENTITY_ATTRIBUTE_UUID(
  "C6B2F38C-23AB-46D8-A6AB-A3A870BBD5D7"
);

NimBLEServer* bleServer = nullptr;
NimBLEClient* phoneClient = nullptr;

NimBLERemoteCharacteristic* entityUpdateCharacteristic = nullptr;
NimBLERemoteCharacteristic* entityAttributeCharacteristic = nullptr;

volatile bool phoneConnected = false;

bool bleSecurityDone = false;
bool amsReady = false;

uint32_t nextAmsDiscoveryAttempt = 0;

struct AMSMessage {
  uint8_t entity;
  uint8_t attribute;
  uint8_t flags;
  char value[192];
};

QueueHandle_t amsQueue = nullptr;

const uint8_t AMS_ENTITY_PLAYER = 0;
const uint8_t AMS_ENTITY_TRACK = 2;

const uint8_t AMS_PLAYER_NAME = 0;
const uint8_t AMS_PLAYER_PLAYBACK_INFO = 1;

const uint8_t AMS_TRACK_ARTIST = 0;
const uint8_t AMS_TRACK_ALBUM = 1;
const uint8_t AMS_TRACK_TITLE = 2;
const uint8_t AMS_TRACK_DURATION = 3;

String playerName = "";
String trackArtist = "";
String trackAlbum = "";
String trackTitle = "";

float trackDuration = 0.0f;
float elapsedAtLastUpdate = 0.0f;
float playbackRate = 1.0f;

int playbackState = -1;

uint32_t lastPlaybackInfoMs = 0;
uint32_t lastMediaUpdateMs = 0;
uint32_t lastTrackResetMs = 0;

// Состояние Wi-Fi и погоды
enum WiFiProfile {
  WIFI_HOME = 0,
  WIFI_PHONE = 1
};

WiFiProfile currentWiFiProfile = WIFI_HOME;

bool wifiConnecting = false;
bool wifiWasConnected = false;

uint32_t wifiAttemptStartedMs = 0;
uint32_t nextWiFiAttemptMs = 0;

uint32_t lastWeatherAttemptMs = 0;

bool weatherValid = false;
bool weatherDirty = true;

float weatherTemperature = 0.0f;
float weatherFeelsLike = 0.0f;
float weatherWindSpeed = 0.0f;
int weatherHumidity = 0;

String weatherDescription = "Ожидание данных";

enum ScreenMode {
  SCREEN_WEATHER,
  SCREEN_MUSIC
};

ScreenMode currentScreen = SCREEN_WEATHER;
bool screenInitialized = false;
bool musicDirty = true;

uint32_t lastProgressDrawMs = 0;

// Вспомогательные функции и отрисовка
String formatTimeSeconds(float seconds) {
  if (!isfinite(seconds) || seconds < 0) {
    seconds = 0;
  }

  int total = (int)seconds;
  int minutes = total / 60;
  int secs = total % 60;

  char buffer[12];
  snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, secs);
  return String(buffer);
}

float currentTrackElapsed() {
  float elapsed = elapsedAtLastUpdate;

  if (playbackState == 1 || playbackState == 2 || playbackState == 3) {
    uint32_t deltaMs = millis() - lastPlaybackInfoMs;
    elapsed += (deltaMs / 1000.0f) * playbackRate;
  }

  if (!isfinite(elapsed) || elapsed < 0) {
    elapsed = 0;
  }

  if (trackDuration > 0 && elapsed > trackDuration) {
    elapsed = trackDuration;
  }

  return elapsed;
}

bool musicShouldBeVisible() {
  if (!phoneConnected) {
    return false;
  }

  if (playbackState == 1 || playbackState == 2 || playbackState == 3) {
    return true;
  }

  if (trackTitle.length() > 0 &&
      (millis() - lastPlaybackInfoMs < MUSIC_PAUSE_HOLD_MS ||
       millis() - lastMediaUpdateMs < MUSIC_PAUSE_HOLD_MS)) {
    return true;
  }

  return false;
}

void resetProgressForNewTrack() {
  uint32_t now = millis();

  if (lastTrackResetMs != 0 && now - lastTrackResetMs < 1800) {
    return;
  }

  lastTrackResetMs = now ? now : 1;

  trackDuration = 0.0f;
  elapsedAtLastUpdate = 0.0f;
  lastPlaybackInfoMs = now;
}

void drawWrappedUTF8(
  const String& text,
  int x,
  int firstBaseline,
  int maxWidth,
  int lineHeight,
  int maxLines,
  const uint8_t* font,
  uint16_t color
) {
  if (text.length() == 0 || maxLines <= 0) {
    return;
  }

  u8g2Fonts.setFont(font);
  u8g2Fonts.setFontMode(1);
  u8g2Fonts.setFontDirection(0);
  u8g2Fonts.setForegroundColor(color);

  String line = "";
  String word = "";
  int linesDrawn = 0;

  auto emitLine = [&]() {
    if (line.length() == 0 || linesDrawn >= maxLines) {
      return;
    }

    u8g2Fonts.drawUTF8(
      x,
      firstBaseline + linesDrawn * lineHeight,
      line.c_str()
    );

    linesDrawn++;
    line = "";
  };

  for (int i = 0; i <= (int)text.length(); i++) {
    char c = (i < (int)text.length()) ? text.charAt(i) : ' ';

    if (c == ' ' || c == '\n' || c == '\r') {
      if (word.length() > 0) {
        String candidate = line;

        if (candidate.length() > 0) {
          candidate += " ";
        }

        candidate += word;

        if (line.length() > 0 &&
            u8g2Fonts.getUTF8Width(candidate.c_str()) > maxWidth) {
          emitLine();
          line = word;
        } else {
          line = candidate;
        }

        word = "";
      }

      if (c == '\n') {
        emitLine();
      }
    } else {

      word += c;
    }

    if (linesDrawn >= maxLines) {
      break;
    }
  }

  if (linesDrawn < maxLines && line.length() > 0) {
    emitLine();
  }
}

void drawHeader(const char* rightText) {
  tft.drawRoundRect(4, 4, 232, 312, 5, NERV_WHITE);
  tft.fillRect(12, 13, 216, 3, NERV_RED);

  tft.setTextWrap(false);
  tft.setTextSize(3);
  tft.setTextColor(NERV_RED);
  tft.setCursor(16, 24);
  tft.print("NERV");

  tft.setTextSize(1);
  tft.setTextColor(NERV_WHITE);
  tft.setCursor(142, 27);
  tft.print(rightText);

  tft.drawFastHLine(14, 57, 212, NERV_WHITE);
}

void drawProgressBar() {
  const int x = 16;
  const int y = 250;
  const int w = 208;
  const int h = 11;

  tft.fillRect(13, 240, 214, 48, NERV_BLACK);

  tft.drawRoundRect(x, y, w, h, 4, NERV_WHITE);

  float fraction = 0.0f;

  if (trackDuration > 0.1f) {
    fraction = currentTrackElapsed() / trackDuration;
    if (!isfinite(fraction)) fraction = 0.0f;
    fraction = constrain(fraction, 0.0f, 1.0f);
  }

  int fillWidth = (int)((w - 4) * fraction);

  if (fillWidth > 0) {
    tft.fillRoundRect(x + 2, y + 2, fillWidth, h - 4, 2, NERV_RED);
  }

  String elapsedText = formatTimeSeconds(currentTrackElapsed());
  String durationText = (trackDuration > 0.1f)
                        ? formatTimeSeconds(trackDuration)
                        : String("--:--");

  tft.setTextSize(1);
  tft.setTextColor(NERV_WHITE);
  tft.setCursor(17, 273);
  tft.print(elapsedText);
  tft.setCursor(174, 273);
  tft.print(durationText);
}

// Экран погоды
String weatherCodeToRussian(int code) {
  switch (code) {
    case 0: return "Ясное небо";
    case 1: return "Преимущественно ясно";
    case 2: return "Переменная облачность";
    case 3: return "Пасмурно";
    case 45:
    case 48: return "Туман";
    case 51:
    case 53:
    case 55: return "Морось";
    case 56:
    case 57: return "Ледяная морось";
    case 61: return "Небольшой дождь";
    case 63: return "Умеренный дождь";
    case 65: return "Сильный дождь";
    case 66:
    case 67: return "Ледяной дождь";
    case 71: return "Небольшой снег";
    case 73: return "Умеренный снег";
    case 75: return "Сильный снег";
    case 77: return "Снежные зёрна";
    case 80: return "Небольшой ливень";
    case 81: return "Ливень";
    case 82: return "Сильный ливень";
    case 85:
    case 86: return "Снегопад";
    case 95: return "Гроза";
    case 96:
    case 99: return "Гроза с градом";
    default: return "Нет описания";
  }
}

void drawWeatherScreen() {
  tft.fillScreen(NERV_BLACK);
  drawHeader("SYSTEM // WEATHER");

  drawWrappedUTF8(
    "ПОГОДА // ОМСК",
    17, 79, 205, 17, 1,
    u8g2_font_7x13_t_cyrillic,
    NERV_RED
  );

  tft.drawRoundRect(14, 91, 212, 126, 5, NERV_RED);

  if (weatherValid) {
    tft.setTextColor(NERV_WHITE);
    tft.setTextSize(4);
    tft.setCursor(25, 111);
    tft.print((int)roundf(weatherTemperature));

    u8g2Fonts.setFont(u8g2_font_7x13_t_cyrillic);
    u8g2Fonts.setForegroundColor(NERV_RED);
    u8g2Fonts.drawUTF8(104, 139, "°C");

    drawWrappedUTF8(
      weatherDescription,
      22, 166, 192, 16, 1,
      u8g2_font_7x13_t_cyrillic,
      NERV_WHITE
    );

    String feels = "Ощущается: " + String((int)roundf(weatherFeelsLike)) + "°C";
    drawWrappedUTF8(
      feels,
      22, 194, 192, 15, 1,
      u8g2_font_6x13_t_cyrillic,
      NERV_WHITE
    );
  } else {
    drawWrappedUTF8(
      "ОЖИДАНИЕ ПОГОДНЫХ ДАННЫХ",
      22, 145, 192, 17, 2,
      u8g2_font_7x13_t_cyrillic,
      NERV_WHITE
    );

    drawWrappedUTF8(
      "Проверьте подключение Wi-Fi",
      22, 190, 192, 16, 2,
      u8g2_font_6x13_t_cyrillic,
      NERV_RED
    );
  }

  tft.drawFastHLine(15, 231, 210, NERV_RED);

  String humidity = "ВЛАЖНОСТЬ: " +
                    (weatherValid ? String(weatherHumidity) + "%" : "--");

  String wind = "ВЕТЕР: " +
                (weatherValid ? String(weatherWindSpeed, 1) + " км/ч" : "--");

  drawWrappedUTF8(
    humidity,
    18, 254, 204, 16, 1,
    u8g2_font_6x13_t_cyrillic,
    NERV_WHITE
  );

  drawWrappedUTF8(
    wind,
    18, 276, 204, 16, 1,
    u8g2_font_6x13_t_cyrillic,
    NERV_WHITE
  );

  tft.setTextSize(1);
  tft.setTextColor(WiFi.status() == WL_CONNECTED ? NERV_RED : NERV_GRAY);
  tft.setCursor(17, 300);
  tft.print(WiFi.status() == WL_CONNECTED ? "WI-FI // ONLINE" : "WI-FI // OFFLINE");

  tft.setTextColor(NERV_WHITE);
  tft.setCursor(154, 300);
  tft.print("NERV SYSTEM");

  weatherDirty = false;
}

// Экран музыки
void drawMusicScreen() {
  tft.fillScreen(NERV_BLACK);
  drawHeader("MEDIA // AMS");

  drawWrappedUTF8(
    "YOU CAN (NOT) HEAR",
    17, 78, 205, 16, 1,
    u8g2_font_7x13_t_cyrillic,
    NERV_RED
  );

  tft.drawRoundRect(14, 90, 212, 139, 5, NERV_RED);

  String app = playerName.length() ? playerName : "МУЗЫКАЛЬНЫЙ ПЛЕЕР";

  drawWrappedUTF8(
    app,
    22, 111, 192, 15, 1,
    u8g2_font_6x13_t_cyrillic,
    NERV_CYAN
  );

  drawWrappedUTF8(
    trackTitle.length() ? trackTitle : "Ожидание композиции...",
    22, 137, 192, 17, 3,
    u8g2_font_7x13_t_cyrillic,
    NERV_WHITE
  );

  String artistLine = trackArtist.length()
                      ? trackArtist
                      : "Исполнитель не указан";

  drawWrappedUTF8(
    artistLine,
    22, 190, 192, 15, 1,
    u8g2_font_6x13_t_cyrillic,
    NERV_RED
  );

  String albumLine = trackAlbum.length()
                     ? trackAlbum
                     : "Альбом не указан";

  drawWrappedUTF8(
    albumLine,
    22, 211, 192, 14, 1,
    u8g2_font_6x12_t_cyrillic,
    NERV_WHITE
  );

  String stateText;

  if (playbackState == 1) {
    stateText = "ВОСПРОИЗВЕДЕНИЕ";
  } else if (playbackState == 0) {
    stateText = "ПАУЗА";
  } else if (playbackState == 2) {
    stateText = "ПЕРЕМОТКА НАЗАД";
  } else if (playbackState == 3) {
    stateText = "ПЕРЕМОТКА ВПЕРЁД";
  } else {
    stateText = "BLUETOOTH // CONNECTED";
  }

  drawWrappedUTF8(
    stateText,
    18, 235, 204, 14, 1,
    u8g2_font_6x12_t_cyrillic,
    NERV_CYAN
  );

  drawProgressBar();

  tft.setTextSize(1);
  tft.setTextColor(NERV_RED);
  tft.setCursor(17, 300);
  tft.print("APPLE MEDIA SERVICE");

  tft.setTextColor(NERV_WHITE);
  tft.setCursor(174, 300);
  tft.print("ONLINE");

  musicDirty = false;
  lastProgressDrawMs = millis();
}

// Подключение: домашняя сеть, затем точка доступа iPhone
bool profileHasCredentials(WiFiProfile profile) {
  const char* ssid = (profile == WIFI_HOME)
                     ? HOME_WIFI_SSID
                     : PHONE_WIFI_SSID;

  if (ssid == nullptr || strlen(ssid) == 0) {
    return false;
  }

  if (String(ssid) == "HOME_WIFI_SSID" ||
      String(ssid) == "PHONE_WIFI_SSID") {
    return false;
  }

  return true;
}

void startWiFiAttempt() {
  WiFiProfile selected = currentWiFiProfile;
  bool foundProfile = false;

  for (int i = 0; i < 2; i++) {
    WiFiProfile candidate = (WiFiProfile)((currentWiFiProfile + i) % 2);

    if (profileHasCredentials(candidate)) {
      selected = candidate;
      foundProfile = true;
      break;
    }
  }

  if (!foundProfile) {
    Serial.println("[WiFi] Заполните SSID и пароль в начале скетча.");
    nextWiFiAttemptMs = millis() + 10000;
    return;
  }

  currentWiFiProfile = selected;

  const char* ssid = (selected == WIFI_HOME)
                     ? HOME_WIFI_SSID
                     : PHONE_WIFI_SSID;

  const char* password = (selected == WIFI_HOME)
                         ? HOME_WIFI_PASSWORD
                         : PHONE_WIFI_PASSWORD;

  Serial.print("[WiFi] Подключение к: ");
  Serial.println(ssid);

  WiFi.disconnect(false, false);
  WiFi.begin(ssid, password);

  wifiConnecting = true;
  wifiAttemptStartedMs = millis();
}

void serviceWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnecting = false;

    if (!wifiWasConnected) {
      wifiWasConnected = true;

      Serial.print("[WiFi] Подключено. IP: ");
      Serial.println(WiFi.localIP());

      configTzTime(
        WEATHER_TIMEZONE,
        "pool.ntp.org",
        "time.google.com"
      );

      weatherDirty = true;
      lastWeatherAttemptMs = 0;
    }

    return;
  }

  if (wifiWasConnected) {
    wifiWasConnected = false;
    wifiConnecting = false;

    Serial.println("[WiFi] Соединение потеряно.");

    currentWiFiProfile =
      (currentWiFiProfile == WIFI_HOME) ? WIFI_PHONE : WIFI_HOME;

    nextWiFiAttemptMs = millis() + WIFI_RETRY_DELAY_MS;
    weatherDirty = true;
  }

  if (wifiConnecting &&
      millis() - wifiAttemptStartedMs >= WIFI_CONNECT_TIMEOUT_MS) {

    Serial.println("[WiFi] Тайм-аут подключения.");

    wifiConnecting = false;
    WiFi.disconnect(false, false);

    currentWiFiProfile =
      (currentWiFiProfile == WIFI_HOME) ? WIFI_PHONE : WIFI_HOME;

    nextWiFiAttemptMs = millis() + WIFI_RETRY_DELAY_MS;
  }

  if (!wifiConnecting && millis() >= nextWiFiAttemptMs) {
    startWiFiAttempt();
  }
}

// Прямое HTTPS-подключение к Open-Meteo без повторного DNS-запроса
bool readWeatherBytes(
  WiFiClientSecure& client,
  String& output,
  size_t count,
  uint32_t timeoutMs
) {
  uint32_t lastDataMs = millis();

  while (count > 0) {
    if (client.available() > 0) {
      int value = client.read();

      if (value >= 0) {
        output += (char)value;
        count--;
        lastDataMs = millis();
      }
    } else {
      if (!client.connected()) {
        return false;
      }

      if (millis() - lastDataMs >= timeoutMs) {
        return false;
      }

      delay(1);
    }
  }

  return true;
}

bool readWeatherBody(
  WiFiClientSecure& client,
  String& payload,
  int contentLength,
  bool isChunked
) {
  payload = "";
  payload.reserve(contentLength > 0 && contentLength < 8192
                    ? contentLength
                    : 2048);

  if (isChunked) {
    while (true) {
      String chunkLine = client.readStringUntil('\n');
      chunkLine.trim();

      int extensionPos = chunkLine.indexOf(';');
      if (extensionPos >= 0) {
        chunkLine = chunkLine.substring(0, extensionPos);
      }

      unsigned long chunkSize =
        strtoul(chunkLine.c_str(), nullptr, 16);

      if (chunkLine.length() == 0) {
        Serial.println("[WEATHER] Empty chunk header.");
        return false;
      }

      if (chunkSize == 0) {

        while (client.connected() || client.available()) {
          String trailer = client.readStringUntil('\n');
          trailer.trim();

          if (trailer.length() == 0) {
            break;
          }
        }

        return true;
      }

      if (chunkSize > 8192 ||
          payload.length() + chunkSize > 8192) {
        Serial.println("[WEATHER] Chunked response is too large.");
        return false;
      }

      if (!readWeatherBytes(client, payload, chunkSize, 15000)) {
        Serial.println("[WEATHER] Timeout while reading chunk.");
        return false;
      }

      client.readStringUntil('\n');
    }
  }

  if (contentLength >= 0) {
    if (contentLength > 8192) {
      Serial.println("[WEATHER] HTTP response is too large.");
      return false;
    }

    if (!readWeatherBytes(
          client,
          payload,
          (size_t)contentLength,
          15000
        )) {
      Serial.println("[WEATHER] Timeout while reading response body.");
      return false;
    }

    return true;
  }

  uint32_t lastDataMs = millis();

  while (client.connected() || client.available()) {
    if (client.available() > 0) {
      int value = client.read();

      if (value >= 0) {
        payload += (char)value;
        lastDataMs = millis();

        if (payload.length() > 8192) {
          Serial.println("[WEATHER] HTTP response is too large.");
          return false;
        }
      }
    } else {
      if (millis() - lastDataMs > 15000) {
        break;
      }

      delay(1);
    }
  }

  return payload.length() > 0;
}

bool fetchWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WEATHER] Wi-Fi is not connected.");
    return false;
  }

  const IPAddress openMeteoIPs[] = {
    IPAddress(188, 40, 99, 226),
    IPAddress(167, 114, 211, 86)
  };

  const char* host = "api.open-meteo.com";

  String timezone = WEATHER_TIMEZONE;
  timezone.replace("/", "%2F");

  String path =
    String("/v1/forecast?latitude=") + String(WEATHER_LATITUDE, 6) +
    "&longitude=" + String(WEATHER_LONGITUDE, 6) +
    "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m" +
    "&timezone=" + timezone;

  Serial.println("[WEATHER] Connecting directly to Open-Meteo...");

  WiFiClientSecure secureClient;
  secureClient.setInsecure();
  secureClient.setTimeout(15000);
  secureClient.setConnectionTimeout(15000);
  secureClient.setHandshakeTimeout(20);

  bool tlsConnected = false;

  for (size_t i = 0; i < sizeof(openMeteoIPs) / sizeof(openMeteoIPs[0]); i++) {
    Serial.print("[WEATHER] TLS to ");
    Serial.println(openMeteoIPs[i]);

    if (secureClient.connect(
          openMeteoIPs[i],
          443,
          host,
          nullptr,
          nullptr,
          nullptr
        )) {
      tlsConnected = true;
      Serial.println("[WEATHER] TLS CONNECTED.");
      break;
    }

    Serial.println("[WEATHER] TLS connection failed; trying next IP.");
    secureClient.stop();
  }

  if (!tlsConnected) {
    Serial.println("[WEATHER] Could not establish TLS connection.");
    return false;
  }

  secureClient.print("GET ");
  secureClient.print(path);
  secureClient.print(" HTTP/1.1\r\n");
  secureClient.print("Host: ");
  secureClient.print(host);
  secureClient.print("\r\n");
  secureClient.print("User-Agent: ESPDeck/1.0\r\n");
  secureClient.print("Accept: application/json\r\n");
  secureClient.print("Accept-Encoding: identity\r\n");
  secureClient.print("Connection: close\r\n");
  secureClient.print("\r\n");
  secureClient.flush();

  Serial.println("[WEATHER] HTTP request sent through pinned TLS socket.");

  String statusLine = secureClient.readStringUntil('\n');
  statusLine.trim();

  Serial.print("[WEATHER] Status line: ");
  Serial.println(statusLine);

  int firstSpace = statusLine.indexOf(' ');
  int httpStatus = (firstSpace >= 0)
                   ? statusLine.substring(firstSpace + 1).toInt()
                   : 0;

  if (httpStatus != 200) {
    Serial.printf("[WEATHER] Server returned HTTP %d.\n", httpStatus);
    secureClient.stop();
    return false;
  }

  int contentLength = -1;
  bool chunked = false;
  bool headersFinished = false;

  for (int i = 0; i < 40; i++) {
    String headerLine = secureClient.readStringUntil('\n');

    if (headerLine.length() == 0) {
      break;
    }

    headerLine.trim();

    if (headerLine.length() == 0) {
      headersFinished = true;
      break;
    }

    String lowerHeader = headerLine;
    lowerHeader.toLowerCase();

    if (lowerHeader.startsWith("content-length:")) {
      contentLength =
        lowerHeader.substring(strlen("content-length:")).toInt();
    }

    if (lowerHeader.startsWith("transfer-encoding:") &&
        lowerHeader.indexOf("chunked") >= 0) {
      chunked = true;
    }
  }

  if (!headersFinished) {
    Serial.println("[WEATHER] HTTP headers were not completed.");
    secureClient.stop();
    return false;
  }

  String payload;

  if (!readWeatherBody(
        secureClient,
        payload,
        contentLength,
        chunked
      )) {
    Serial.println("[WEATHER] Failed to read HTTP response body.");
    secureClient.stop();
    return false;
  }

  secureClient.stop();

  Serial.print("[WEATHER] JSON bytes received: ");
  Serial.println(payload.length());

  DynamicJsonDocument doc(3072);

  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.print("[WEATHER] JSON error: ");
    Serial.println(error.c_str());
    Serial.println("[WEATHER] Response body:");
    Serial.println(payload);
    return false;
  }

  JsonObject current = doc["current"];

  if (current.isNull()) {
    Serial.println("[WEATHER] Response has no current object.");
    return false;
  }

  float temp = current["temperature_2m"] | NAN;
  float feels = current["apparent_temperature"] | NAN;
  float wind = current["wind_speed_10m"] | NAN;
  int humidity = current["relative_humidity_2m"] | -1;
  int code = current["weather_code"] | -1;

  if (!isfinite(temp)) {
    Serial.println("[WEATHER] Invalid temperature in JSON.");
    return false;
  }

  weatherTemperature = temp;
  weatherFeelsLike = isfinite(feels) ? feels : temp;
  weatherWindSpeed = isfinite(wind) ? wind : 0.0f;
  weatherHumidity = humidity;
  weatherDescription = weatherCodeToRussian(code);

  weatherValid = true;
  weatherDirty = true;

  Serial.printf(
    "[WEATHER] Омск: %.1f C, %s, влажность %d%%, ветер %.1f км/ч\n",
    weatherTemperature,
    weatherDescription.c_str(),
    weatherHumidity,
    weatherWindSpeed
  );

  return true;
}
void serviceWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  uint32_t interval = weatherValid
                      ? WEATHER_UPDATE_INTERVAL_MS
                      : WEATHER_RETRY_INTERVAL_MS;

  if (lastWeatherAttemptMs == 0 ||
      millis() - lastWeatherAttemptMs >= interval) {

    lastWeatherAttemptMs = millis();

    if (!fetchWeather()) {
      Serial.println("[WEATHER] Обновление не удалось; повтор позже.");
    }
  }
}

// Получение и обработка уведомлений AMS
void onAMSNotification(
  NimBLERemoteCharacteristic* characteristic,
  uint8_t* data,
  size_t length,
  bool isNotify
) {
  if (data == nullptr || length < 3 || amsQueue == nullptr) {
    return;
  }

  AMSMessage message{};

  message.entity = data[0];
  message.attribute = data[1];
  message.flags = data[2];

  size_t valueLength = length - 3;

  if (valueLength >= sizeof(message.value)) {
    valueLength = sizeof(message.value) - 1;
  }

  memcpy(message.value, data + 3, valueLength);
  message.value[valueLength] = '\0';

  xQueueSend(amsQueue, &message, 0);
}

class ESPDeckServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* server, NimBLEConnInfo& connInfo) override {
    Serial.print("[BLE] iPhone connected: ");
    Serial.println(connInfo.getAddress().toString().c_str());

    phoneClient = server->getClient(connInfo);

    phoneConnected = (phoneClient != nullptr);

    bleSecurityDone = false;
    amsReady = false;

    entityUpdateCharacteristic = nullptr;
    entityAttributeCharacteristic = nullptr;

    nextAmsDiscoveryAttempt = 0;
  }

  void onDisconnect(
    NimBLEServer* server,
    NimBLEConnInfo& connInfo,
    int reason
  ) override {
    Serial.printf("[BLE] iPhone disconnected, reason: %d\n", reason);

    phoneConnected = false;

    phoneClient = nullptr;

    bleSecurityDone = false;
    amsReady = false;

    entityUpdateCharacteristic = nullptr;
    entityAttributeCharacteristic = nullptr;
  }
};

ESPDeckServerCallbacks serverCallbacks;

void startBLE() {
  Serial.println("[BLE] Starting NimBLE...");

  NimBLEDevice::init("ESPDeck");
  NimBLEDevice::setPower(9);

  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  bleServer = NimBLEDevice::createServer();
  bleServer->setCallbacks(&serverCallbacks);
  bleServer->advertiseOnDisconnect(true);

  NimBLEAdvertising* advertising = bleServer->getAdvertising();

  NimBLEAdvertisementData advertisementData;
  advertisementData.setFlags(0x06);
  advertisementData.setName("ESPDeck");

  const uint8_t amsSolicitation[] = {
    0x11, 0x15,
    0xDC, 0xF8, 0x55, 0xAD,
    0x02, 0xC5, 0xF4, 0x8E,
    0x3A, 0x43, 0x36, 0x0F,
    0x2B, 0x50, 0xD3, 0x89
  };

  advertisementData.addData(
    amsSolicitation,
    sizeof(amsSolicitation)
  );

  advertising->setAdvertisementData(advertisementData);
  advertising->start();

  Serial.println("[BLE] Advertising as ESPDeck.");
  Serial.println("[BLE] iPhone: Settings -> Bluetooth -> ESPDeck.");
}

void serviceAMSConnection() {
  if (!phoneConnected || phoneClient == nullptr) {
    return;
  }

  if (!phoneClient->isConnected()) {
    return;
  }

  if (!bleSecurityDone) {
    Serial.println("[BLE] Securing Bluetooth connection...");

    if (!phoneClient->secureConnection()) {
      Serial.println("[BLE] Secure connection not ready; retrying.");
      nextAmsDiscoveryAttempt = millis() + 3000;
      return;
    }

    bleSecurityDone = true;
    Serial.println("[BLE] Secure connection established.");
  }

  if (amsReady) {
    return;
  }

  if (millis() < nextAmsDiscoveryAttempt) {
    return;
  }

  nextAmsDiscoveryAttempt = millis() + 4000;

  NimBLERemoteService* amsService =
    phoneClient->getService(AMS_SERVICE_UUID);

  if (amsService == nullptr) {
    Serial.println("[AMS] Service not available yet; retrying...");
    return;
  }

  entityUpdateCharacteristic =
    amsService->getCharacteristic(AMS_ENTITY_UPDATE_UUID);

  entityAttributeCharacteristic =
    amsService->getCharacteristic(AMS_ENTITY_ATTRIBUTE_UUID);

  if (entityUpdateCharacteristic == nullptr) {
    Serial.println("[AMS] Entity Update characteristic not found.");
    return;
  }

  if (!entityUpdateCharacteristic->canNotify() ||
      !entityUpdateCharacteristic->canWrite()) {
    Serial.println("[AMS] Entity Update has no notify/write support.");
    return;
  }

  if (!entityUpdateCharacteristic->subscribe(
        true,
        onAMSNotification,
        true
      )) {
    Serial.println("[AMS] Subscribe failed; retrying.");
    return;
  }

  const uint8_t playerRequest[] = {
    AMS_ENTITY_PLAYER,
    AMS_PLAYER_NAME,
    AMS_PLAYER_PLAYBACK_INFO
  };

  const uint8_t trackRequest[] = {
    AMS_ENTITY_TRACK,
    AMS_TRACK_ARTIST,
    AMS_TRACK_ALBUM,
    AMS_TRACK_TITLE,
    AMS_TRACK_DURATION
  };

  bool playerOk = entityUpdateCharacteristic->writeValue(
    playerRequest,
    sizeof(playerRequest),
    true
  );

  bool trackOk = entityUpdateCharacteristic->writeValue(
    trackRequest,
    sizeof(trackRequest),
    true
  );

  if (!playerOk || !trackOk) {
    Serial.println("[AMS] Attribute registration failed; retrying.");
    return;
  }

  amsReady = true;

  Serial.println("[AMS] Subscribed to player and track metadata.");
}

void processAMSMessage(const AMSMessage& message) {
  String value = String(message.value);
  value.trim();

  if ((message.flags & 0x01) &&
      entityAttributeCharacteristic != nullptr &&
      phoneConnected &&
      amsReady &&
      entityAttributeCharacteristic->canWrite() &&
      entityAttributeCharacteristic->canRead()) {

    uint8_t request[2] = { message.entity, message.attribute };

    if (entityAttributeCharacteristic->writeValue(request, sizeof(request), true)) {
      std::string fullValue = entityAttributeCharacteristic->readValue();
      if (!fullValue.empty()) value = String(fullValue.c_str());
    }
  }

  lastMediaUpdateMs = millis();

  if (message.entity == AMS_ENTITY_PLAYER) {
    if (message.attribute == AMS_PLAYER_NAME) {
      playerName = value;
    } else if (message.attribute == AMS_PLAYER_PLAYBACK_INFO) {
      int state;
      float rate, elapsed;

      if (sscanf(value.c_str(), "%d,%f,%f", &state, &rate, &elapsed) == 3) {
        playbackState = state;
        playbackRate = rate;
        elapsedAtLastUpdate = elapsed;
        lastPlaybackInfoMs = millis();
      }
    }
  } else if (message.entity == AMS_ENTITY_TRACK) {
    if (message.attribute == AMS_TRACK_ARTIST) {
      if (trackArtist.length() && trackArtist != value) {
        resetProgressForNewTrack();
      }
      trackArtist = value;

    } else if (message.attribute == AMS_TRACK_ALBUM) {
      trackAlbum = value;

    } else if (message.attribute == AMS_TRACK_TITLE) {
      if (trackTitle != value) {
        resetProgressForNewTrack();
        trackTitle = value;
        Serial.printf("[MUSIC] %s - %s\n",
                      trackArtist.c_str(), trackTitle.c_str());
      }

    } else if (message.attribute == AMS_TRACK_DURATION) {
      float duration = strtof(value.c_str(), nullptr);
      trackDuration = (isfinite(duration) && duration > 0.1f)
                      ? duration : 0.0f;
    }
  }

  musicDirty = true;
}

void serviceAMSQueue() {
  if (amsQueue == nullptr) {
    return;
  }

  AMSMessage message;

  while (xQueueReceive(amsQueue, &message, 0) == pdTRUE) {
    processAMSMessage(message);
  }
}

// Автоматическое переключение двух экранов
void serviceDisplay() {
  bool wantMusic = musicShouldBeVisible();

  if (wantMusic && currentScreen != SCREEN_MUSIC) {
    currentScreen = SCREEN_MUSIC;
    drawMusicScreen();
    screenInitialized = true;
    return;
  }

  if (!wantMusic && currentScreen != SCREEN_WEATHER) {
    currentScreen = SCREEN_WEATHER;
    drawWeatherScreen();
    screenInitialized = true;
    return;
  }

  if (!screenInitialized) {
    currentScreen = wantMusic ? SCREEN_MUSIC : SCREEN_WEATHER;

    if (currentScreen == SCREEN_MUSIC) {
      drawMusicScreen();
    } else {
      drawWeatherScreen();
    }

    screenInitialized = true;
    return;
  }

  if (currentScreen == SCREEN_MUSIC) {
    if (musicDirty) {
      drawMusicScreen();
    }

    if (millis() - lastProgressDrawMs >= PROGRESS_REFRESH_MS) {
      drawProgressBar();
      lastProgressDrawMs = millis();
    }
  }

  else if (currentScreen == SCREEN_WEATHER) {
    if (weatherDirty) {
      drawWeatherScreen();
    }
  }
}

// Инициализация и главный цикл
void setup() {
  Serial.begin(115200);
  delay(300);

  amsQueue = xQueueCreate(24, sizeof(AMSMessage));

  SPI.begin(TFT_SCK, TFT_MISO, TFT_MOSI, TFT_CS);

  tft.init(240, 320);
  tft.setRotation(0);

  tft.invertDisplay(false);

  tft.setTextWrap(false);

  u8g2Fonts.begin(tft);
  u8g2Fonts.setFontMode(1);
  u8g2Fonts.setFontDirection(0);

  tft.fillScreen(NERV_BLACK);

  drawWrappedUTF8(
    "NERV SYSTEM INITIALIZING",
    18, 120, 205, 18, 2,
    u8g2_font_7x13_t_cyrillic,
    NERV_RED
  );

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  nextWiFiAttemptMs = 0;

  startBLE();

  drawWeatherScreen();
  screenInitialized = true;

  Serial.println("[SYSTEM] ESPDeck ONLINE.");
}

void loop() {

  serviceAMSConnection();
  serviceAMSQueue();

  serviceWiFi();
  serviceWeather();

  serviceDisplay();

  delay(2);
}
