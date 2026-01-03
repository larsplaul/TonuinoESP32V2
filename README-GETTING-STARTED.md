# ESP32-S3 TonUINO – Fungerende Audio-Setup (MP3 + SD + I2S)

Denne dokumentation beskriver **det præcise setup, der reelt virker**, efter omfattende fejlsøgning.

Formålet er:
- at kunne gentage opsætningen hurtigt på en ny PC
- at undgå kendte faldgruber (IDF-versioner, 5V-pin, audio-libs)
- at have et stabilt fundament for et TonUINO-lignende system

---

## Overblik

**Status:**  
✅ MP3 afspilning virker  
✅ SD-kort læses korrekt  
✅ I2S DAC (UDA1334) virker  
✅ ESP32-S3 med PSRAM virker  
❌ WAV fravalgt (for komplekst / unødvendigt)

---

## Hardware (verificeret)

- **MCU:** ESP32-S3 N16R8 (8 MB PSRAM)
- **DAC:** UDA1334 (I2S)
- **Lagring:** SD-kort (SPI)
- **Forstærker:** ekstern (kræver 5V)
- **Audioformat:** MP3

---

## Kritisk erkendelse (vigtigt!)

### ⚠️ ESP32-S3 “5V” pin er IKKE en rigtig 5V
På dette board måles “5V”-pinnen til ca. **1.2 V**, selv uden belastning.

Konsekvens:
- DAC og forstærker fik ingen reel strøm
- Ingen lyd, selv med korrekt kode

### ✅ Løsning
- **UDA1334 drives på 3V3**
- **Forstærker får ekstern 5V** (USB-lader / powerbank)
- **Fælles GND** mellem ESP32 og forstærker

Dette alene var forskellen på *ingen lyd* og *alt virker*.

---

## Software-miljø (meget vigtigt)

### Platform / Framework
- **IDE:** VS Code + PlatformIO
- **Platform:** pioarduino (ikke standard espressif32)
- **Arduino-ESP32:** 3.1.1
- **ESP-IDF:** 5.3

> Arduino-ESP32 2.x / IDF 4.x er **for gammel** til moderne I2S/audio og gav:
> - manglende `i2s_std.h`
> - `<span>`-fejl
> - inkompatible audio-biblioteker

---

## platformio.ini (kendt-virkende)

```ini
[platformio]
default_envs = esp32-s3

[env:esp32-s3]
platform = https://github.com/pioarduino/platform-espressif32/releases/download/53.03.11/platform-espressif32.zip
board = esp32-s3-devkitc-1
framework = arduino
monitor_speed = 115200

board_build.arduino.memory_type = qio_opi
build_flags =
  -DBOARD_HAS_PSRAM
