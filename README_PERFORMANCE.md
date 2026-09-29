# High-performance / stability profile

This build keeps the packet path deliberately small:

**Wi-Fi driver -> lwIP IPv4 forwarding -> NAPT -> Wi-Fi driver**

No VPN, packet capture, ACL engine, cloud service, or background telemetry is added to the forwarding path.

## Performance tuning

- Dual-core ESP32 at 240 MHz.
- Wi-Fi power save disabled (`WIFI_PS_NONE`) for lower latency and fewer sleep/wake stalls.
- Wi-Fi static RX buffers: 16.
- Wi-Fi dynamic RX buffers: 64.
- Wi-Fi dynamic TX buffers: 64.
- AMPDU TX/RX enabled.
- TX/RX BA windows: 32.
- Wi-Fi TX/RX IRAM optimizations enabled.
- lwIP IRAM optimization enabled.
- TCP/IP task pinned to CPU1 so the Wi-Fi/NAT application work is not unnecessarily mixed with the main CPU0 path.
- TCP send/receive windows raised to 65534 bytes to better match the large Wi-Fi buffer profile.
- TCP/IP, TCP and UDP receive mailboxes set to 64.
- NAPT and IPv4 forwarding remain enabled directly in lwIP.

Espressif documents that increasing Wi-Fi buffer counts can improve packet throughput, while consuming more RAM, and that TCP send/receive buffers should be sized consistently with the Wi-Fi dynamic buffers for high stable performance. The selected values therefore favor a large but bounded memory profile rather than blindly maximizing every buffer.

## Stability improvements

- Gateway health check remains deliberately slow (20-second interval) so it does not compete with normal traffic.
- Ping sessions are always cleaned up, including the timeout/inconclusive path.
- The ESP-IDF 5.5-compatible `esp_ping_start()` API is used.
- Uplink reconnect uses exponential backoff.
- Long-term uplink failure can reboot the device instead of leaving it permanently wedged.
- Task watchdog and panic-to-reboot remain enabled.
- Logs stay at warning level to reduce unnecessary serial/log overhead.

## Live signal monitor

The local web page now displays the actual STA/uplink RSSI in **dBm**, plus the current uplink channel. The value is refreshed every 5 seconds through `/api/status`.

RSSI is intentionally reported instead of converting it into an invented distance in metres. Distance cannot be calculated accurately from RSSI alone because walls, antenna orientation, interference and the remote AP's transmit power all change the reading.

## Firmware artifacts

The GitHub Actions workflow now fails immediately if the application binary is missing. After a successful build it creates and verifies:

- `esp32_nat_router_performance.bin` — application firmware
- `merged-flash.bin` — complete flash image
- `bootloader.bin`
- `partition-table.bin`

The workflow uses `idf.py merge-bin` instead of silently swallowing a failed `esptool merge_bin` command.

## Important limitation

A classic ESP32 has one 2.4 GHz radio shared by STA and AP. NAT routing therefore has a fundamental throughput ceiling: every packet traverses the same radio system. Software tuning can reduce overhead and improve consistency, but it cannot turn the classic ESP32 into a dual-radio router.

For meaningful comparison, test the old and new firmware at the same location, same upstream router/channel, same client, same RSSI range and same TCP/UDP test method. Record throughput, RSSI, free heap and disconnect/reset events.
