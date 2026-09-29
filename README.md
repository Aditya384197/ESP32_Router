# ESP32 NAT Router (performance + stability)

Classic ESP32, ESP-IDF v5.5.1, lwIP IPv4 NAPT.

## Use
1. Flash `merged-flash.bin` at 0x0 (or bootloader 0x1000, partition-table 0x8000, app 0x10000).
2. Join Wi-Fi **ESP32_Router**, password **12345678**.
3. Open **http://192.168.4.1**, enter uplink Wi-Fi name/password (and change the AP name/password), Save.
4. Connect devices to the access point.

Reset to defaults: hold the BOOT button 5 seconds.
