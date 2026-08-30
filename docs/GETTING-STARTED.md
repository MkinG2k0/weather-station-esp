<!-- GSD:docs-update -->

# Начало работы

Полная инструкция по железу, Wi-Fi и Control Desk — в [README.md](../README.md). Здесь короткий путь.

## 1. Сайт

Откройте [weather-e-ink.vercel.app](https://weather-e-ink.vercel.app/), войдите, настройте **01 Экран** и **02 Плата** (800×480, чёрный/белый), сохраните, скопируйте URL из **03**.

![Вход в Control Desk](images/control-desk-login.png)

## 2. Секреты

```powershell
Copy-Item src/secrets.example.h src/secrets.h
```

Заполните SSID 2,4 ГГц, пароль Wi-Fi и `DEVICE_BASE_URL` без `/screen.png`.

## 3. Прошивка

VS Code + PlatformIO: Build, Upload, Serial Monitor 115200. Порт по умолчанию в `platformio.ini`: `COM5` — поменяйте при необходимости.

Подключение DESPI-C02 — таблица в README. Только 3,3 В, `RESE` = 0.47Ω.

## 4. Ожидаемый лог

Wi-Fi connected → HTTP 200 или 304 → полное обновление E-Ink (~20–30 с) → `Deep sleep for … seconds`.
