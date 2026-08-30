<!-- GSD:docs-update -->

# Конфигурация прошивки

Локальный файл `src/secrets.h` (в `.gitignore`):

| Константа | Значение |
|---|---|
| `WIFI_SSID` | Сеть 2,4 ГГц |
| `WIFI_PASSWORD` | Пароль Wi-Fi |
| `DEVICE_BASE_URL` | `https://weather-e-ink.vercel.app/d/<slug>` без слэша в конце |

Шаблон: `src/secrets.example.h`.

## `platformio.ini`

- `upload_port` / `monitor_port` = `COM5` — смените под свою машину или удалите строки.
- `monitor_speed` = 115200.
- Библиотеки: GxEPD2@1.6.4, PNGdec@1.1.5, Adafruit BME280@2.3.0, BMP280@3.0.0.

## Что задаётся на сайте, не в прошивке

Город, язык, единицы, макет, интервал, тихие часы, кеш PNG, тема. Плата читает интервал из `X-Next-Refresh-Seconds`.

Ночное окно по умолчанию на сервере: 23:00–06:00, не короче часа — если владелец не выключил «Ночной интервал».
