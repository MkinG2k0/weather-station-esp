<!-- GSD:docs-update -->

# Разработка

Платформа: PlatformIO, env `firebeetle2_esp32e` (`platformio.ini`). Плата в PIO указана как `esp32dev` (у DFR0654 4 МБ flash; профиль `firebeetle32` в PIO рассчитан на 16 МБ).

## Сборка

```bash
pio run
pio run --target upload
pio device monitor --baud 115200
```

`PNG_MAX_BUFFERED_PIXELS=6402` — две RGBA-строки 800 px для PNGdec.

## Основные константы (`src/main.cpp`)

- Кадр 800×480, PNG ≤ 64 КиБ, 6 проходов по 80 строк.
- Интервал по умолчанию 10 минут, повтор ошибки 30 с.
- GPIO пинов дисплея, I²C датчика и ADC батареи — в начале `main.cpp`.

Драйвер панели не заменяйте на стандартный `GxEPD2_750c_Z08`: у FPC-8612 другая инициализация.

## Совместно с сайтом

Новые query к PNG, заголовки сна, размер кадра — меняйте в weather (`screen.png/route.ts`) и здесь одновременно.
