<!-- GSD:docs-update -->

# Архитектура прошивки elink

Плата: FireBeetle 2 ESP32-E (DFR0654). Экран: 7,5″ 800×480 чёрно-белый FPC-8612 через DESPI-C02. Кадр рисует Control Desk; прошивка только скачивает PNG и показывает его.

```text
Wake → BMP/BME (опц.) → ADC батареи → Wi-Fi
     → GET {DEVICE_BASE_URL}/screen.png?…
     → PNG 800×480 ≤ 64 КиБ → GxEPD2_750c_86BF (6 полос по 80 строк)
     → Deep Sleep (X-Next-Refresh-Seconds или 30 с при ошибке)
```

## Файлы

| Файл | Роль |
|---|---|
| `platformio.ini` | env `firebeetle2_esp32e`, Arduino, GxEPD2 1.6.4, PNGdec, Adafruit BME/BMP280 |
| `src/main.cpp` | Цикл, Wi-Fi, HTTPS, декод PNG, сон |
| `src/GxEPD2_750c_86BF.*` | Init sequence FPC-8612 / DEPG0750RWF86BF30 |
| `src/secrets.h` | SSID, пароль, URL (не в Git) |
| `src/secrets.example.h` | Шаблон |
| `src/PhotoBitmap.h` | Служебные битмапы |

Пины (как `lmarzen/esp32-weather-epd`): BUSY=14, CS=13, RST=21, DC=22, SCK=18, MOSI=23.

Опционально: BME/BMP на I²C GPIO 16/17, питание GPIO 12; батарея A2/GPIO34.

## Сеть

`WiFiClientSecure::setInsecure()` — TLS без проверки сертификата. Таймаут Wi-Fi и загрузки 30 с. При ошибке сон 30 с (`RETRY_INTERVAL_SECONDS`). Интервал сна ограничен 60 с … 24 ч.

ETag и хеш кадра в RTC (`lastScreenEtag`, `lastImageHash`). После полного отключения питания снова полная загрузка.

## Backend

Сайт: репозиторий weather (`d:\Project\Main\1OLD\weather`), в нём `docs/API.md` описывает `/d/{slug}/screen.png`.
