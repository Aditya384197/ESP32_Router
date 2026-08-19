# High-performance / stability baseline

This build has two deliberate layers:

1. **Architecture cleanup:** Martin Ger's proven APSTA + lwIP IPv4 forwarding + NAPT mechanism is retained; packet-path ACL/PCAP/statistics/VPN and unrelated services are removed.
2. **Evidence-based throughput tuning:** the classic ESP32 Wi-Fi buffer and TCP window values are aligned with Espressif's ESP32 Wi-Fi iperf profile.

The build does **not** add `WIFI_PS_NONE`, custom packet hooks, a new NAPT implementation, or speculative task-affinity changes.

## What changed from the previous clean baseline

- Static RX buffers: 16
- Dynamic RX buffers: 64
- Dynamic TX buffers: 64
- RX/TX BA windows: 32
- TCP send/receive buffers: 65534
- TCP/UDP/TCPIP mailboxes: 64
- lwIP extra IRAM optimization enabled
- Main task stack restored to the proven 7168-byte value
- Explicit DIO/40 MHz flash settings
- Martin-tested ESP32 partition geometry restored
- Boot/reset diagnostics added
- OTA confirmation failure no longer causes an immediate panic

## Important limitation

No source-only change can remove the fundamental cost of using one classic ESP32 Wi-Fi radio for both the AP and STA sides. The purpose of this profile is to push the software stack harder without replacing the NAT mechanism.

Measure against the original firmware under identical RF conditions. Record TCP and UDP throughput, RSSI, free heap during traffic, reset reason, and disconnect reason.

If stability worsens after the buffer/window changes, revert those tuning values as one controlled experiment rather than changing several unrelated variables at once.
