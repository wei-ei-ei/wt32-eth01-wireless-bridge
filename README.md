<div align="center">

**English** | [简体中文](README_zh-CN.md)

</div>

# WT32-ETH01 Universal Wireless Bridge · Seamless Wi-Fi Access for Any Wired Device

> BLE provisioning · Wi-Fi↔Ethernet L2 transparent bridging · Static IP / DHCP · Multi-profile Wi-Fi management
>
> Give devices that only have a **wired Ethernet port** (IP cameras, PCs, NVRs, industrial controllers, etc.) **plug-and-play access to a Wi-Fi network** — just connect a cable, no drivers required. The current firmware supports one wired client at a time.

---

## 1. Use Case

```
                           ethernet                        Wi-Fi (2.4 GHz)
   ┌──────────────────────┐         ┌──────────────────────┐         ┌────────┐
   │ IPC / PC / NVR / PLC │─────────│ WT32-ETH01 bridge    │─────────│ Router │
   │ (any wired device)   │         │ ESP32 + LAN8720      │         └────────┘
   └──────────────────────┘         └──────────────────────┘
              LAN / Cloud
```

- Devices with only a 100 Mbps Ethernet port and **no Wi-Fi** join the wireless network through the bridge;
- The bridge is fully transparent to the device: it **keeps its own IP** (static or DHCP), stays directly reachable at that IP within the same LAN, and needs no port mapping;
- Different device types can connect in turn — unplugging and re-plugging the cable triggers client MAC re-learning;
- The bridge stores up to 16 Wi-Fi profiles; a normal boot reads only the currently selected one;
- Changing Wi-Fi environments: **long-press the button for 2 s** → BLE provisioning (phone app), no computer needed at any point.

## 2. Features

| Feature | Description |
|---------|-------------|
| L2 transparent bridging | Ethernet promiscuous mode + MAC rewriting; single broadcast domain, end-to-end sessions, no NAT at the IP layer |
| Multi-device-type compatible | IP cameras, PCs, NVRs, industrial controllers and other standard Ethernet devices share one firmware; one client at a time |
| **Generic MAC learning** | Learns from the first valid unicast frame on the wired side — no dependency on DHCP or ARP; re-learns automatically after the cable is disconnected |
| BLE provisioning | Long-press IO2 (2 s) enters ESP unified provisioning; the phone app (ESP BLE Provisioning) delivers Wi-Fi credentials |
| Multi-profile Wi-Fi store | Saves 16 SSID/password pairs by default, persisted in NVS; boot loads only the active profile |
| BLE management endpoint | A custom app can list, select and delete saved Wi-Fi networks, and set selection priority |
| Auto-reconnect on boot | Active credentials are stored in NVS; power-cycle reconnects automatically, with automatic fallback to provisioning mode on failure |
| Observable congestion | Drop-counter warning for the Wi-Fi TX queue (`WiFi TX drop #N`) to quantify radio congestion |
| Power save disabled | `WIFI_PS_NONE` prevents downlink traffic from being lost in modem-sleep windows |

## 3. Hardware

| Component | Model / requirement | Notes |
|-----------|---------------------|-------|
| Bridge | WT32-ETH01 (ESP32-D0WD-V3 + LAN8720AI, 100 Mbps) | 5 V power supply |
| Wired device | Any device with a standard 10/100M Ethernet port and a TCP/IP stack | The bridge does not power the peripheral; cameras etc. need their own supply |
| Field-tested reference | IP camera module based on HiSilicon HI3516CV610 | **DC 12 V / 2 A — POE power alone is not sufficient** |
| Router | Any 2.4 GHz Wi-Fi router | Disable "wireless/AP isolation"; on dual-band routers, put the viewing client on 5 GHz for a significant latency improvement |

> For hardware details see [`docs/WT32-ETH01硬件说明.md`](docs/WT32-ETH01硬件说明.md) *(Chinese)*; for the generic onboarding walkthrough and camera field-test notes see [`docs/设备接入示例.md`](docs/设备接入示例.md) *(Chinese)*. The documents under [`docs/`](docs/) are currently available in Chinese only.

## 4. Quick Start

### 1. Environment

- ESP-IDF **v6.0.2**: this project is adapted from the official example `examples/network/sta2eth`, and **v6.0.2 is the verified build version**;
  other IDF versions differ in the example's directory layout and internal APIs (e.g. private symbols such as `esp_wifi_internal_reg_rxcb`),
  so building against another version is not guaranteed to work;
- Dependency components are fetched automatically by the Component Manager via `main/idf_component.yml`: `espressif/ethernet_init`, `espressif/network_provisioning`;
- On Windows, use `idf.ps1` at the repository root: it sets `IDF_TOOLS_PATH`, `TEMP` and `TMP` automatically and loads ESP-IDF for you.

### 2. Build & Flash

The WT32-ETH01 **has no auto-download circuit**, so entering the bootloader manually is required for flashing:

1. **Connect IO0 to GND** → power the module off and back on (enters download mode)
2. Flash:
   ```powershell
   .\idf.ps1 -p COM15 flash monitor
   ```
3. **Remove the IO0-to-GND wire** → power-cycle again; the module starts running automatically

> On the first boot after flashing (empty NVS) the device enters BLE provisioning automatically; if profiles already exist, the console prints
> `Loaded N profiles, active id M` so you can confirm the profile store state.

### 3. BLE Provisioning (also used when changing Wi-Fi)

1. Install the **ESP BLE Provisioning** app on your phone (official Espressif app);
2. **Long-press the bridge's IO2 button for 2 s** (triggers re-provisioning; the device reboots into provisioning mode);
3. Scan for the device in the app → send the Wi-Fi SSID/password → on success the device reboots into bridge mode automatically;
4. Seeing `Wi-Fi STA connected` on the console means success.

The official app only adds networks. A custom app can manage saved networks through the BLE endpoint `wifi-profiles`:

```text
LIST
GET_ACTIVE
SELECT <id>
DELETE <id>
PRIORITY <id> <0-255>
```

For the full protocol see [`docs/WiFi多配置管理.md`](docs/WiFi多配置管理.md) *(Chinese)*.

### 4. Connecting a Wired Device

1. DHCP devices work out of the box; for static-IP devices, configure **the same subnet as the router, the same netmask, and the router as the gateway**;
2. Plug the cable into the bridge; hot-plugging a direct connection triggers link re-establishment and automatic MAC re-learning;
3. From a computer on the same Wi-Fi, `ping` the device's IP, or confirm `Wi-Fi STA connected` on the console;
4. `Wired client MAC learned: xx:xx:...` on the console = the device has been recognized by the bridge;
5. When swapping devices, unplug and re-plug the cable; when swapping through a switch without breaking the link, reboot the bridge.

> For camera power supply, static IP setup and RTMP streaming verification, see [`docs/设备接入示例.md`](docs/设备接入示例.md) *(Chinese)*.

## 5. Measured Performance (for reference)

| Test | Result | Conditions |
|------|--------|------------|
| iperf3 TCP (one hop) | 13–14 Mbps | Peer on the router's wired side |
| iperf3 TCP (two hops, same band) | ~9.6 Mbps | Peer is also a 2.4 GHz wireless client of the same router |
| Ping latency (two hops) | 5–11 ms | At RSSI −44 |
| Camera RTMP streaming | Usable | For the two-hop case keep the bitrate ≤2–3 Mbps and watch the console's `WiFi TX drop` counter |

> Two-hop throughput on the same band is roughly 50–70% of single-hop (shared radio airtime) — physics, not a fault. Improvement: enable dual band on the router and put the viewing client on 5 GHz.

## 6. Differences from the Official Example

This project is adapted from Espressif's official `sta2eth` example (ESP-IDF). **The itemized change list (with rationale and verification steps) is in [`docs/官方示例改动说明.md`](docs/官方示例改动说明.md) *(Chinese)***. Summary:

1. **Three hardware adaptations**: the WPA2 handshake needs SHA1 (disabled in the default preset); the LAN8720 reset pin GPIO16 must be held high; the RMII 50 MHz clock is an **input** on GPIO0 (from the LAN8720's oscillator-doubled output);
2. **Provisioning scheme**: web-based provisioning → **BLE provisioning** (`network_provisioning` + NimBLE);
3. **Ethernet promiscuous mode enabled**: off by default in the official example — with it off, the EMAC hardware filter drops unicast frames not addressed to the bridge itself, the direct root cause of "internet works but Wi-Fi-side devices are unreachable";
4. **Generic MAC learning**: the official MAC slot only recognizes DHCP DISCOVER, so static-IP devices could not connect; it now learns from the first valid upstream unicast frame and clears/re-learns after a link drop;
5. **Multi-profile Wi-Fi store**: NVS holds multiple credentials; a normal boot reads only the active one, adding no per-packet overhead;
6. **BLE management endpoint**: the official provisioning flow keeps handling new networks; the custom endpoint handles listing, selection, deletion and priority management;
7. **TX drop counting**: the official code only LOGD-logs Wi-Fi send failures (invisible); it is now a WARN-level counted warning.

## 7. Known Limitations & Troubleshooting

| Limitation / symptom | Cause | Fix |
|----------------------|-------|-----|
| PC gets 192.168.4.x | Wi-Fi didn't connect; the device fell into provisioning mode (built-in DHCP) | Check the Wi-Fi environment, then **power-cycle** (a soft reboot stays in provisioning mode) |
| Ping succeeds but TTL=128 / 0 ms | The PC itself answered (IP conflict), not the target device | Check the local IP configuration; `arp -d *`, then retest with the correct IP |
| Internet works but Wi-Fi-side devices are unreachable | Promiscuous mode disabled (enabled by default in this repo) | Verify `CONFIG_EXAMPLE_ETHERNET_USE_PROMISCUOUS=y` |
| New device behind a switch doesn't connect after a swap | The link never dropped, so the bridge still holds the first client's MAC | Break the bridge↔switch link and retry, or reboot the bridge |
| A second wired device connected at the same time | The current design supports a single wired client | Even behind a switch, ensure only one active device at a time |
| Streaming latency keeps climbing and never recovers | Insufficient radio capacity → check the console's `WiFi TX drop` counter | Lower the bitrate (≤3 Mbps), offload to the other band, switch to a cleaner channel |

## 8. Repository Layout

```
├── main/
│   ├── bridge_main.c         # Main logic: bridge/provisioning branching, Wi-Fi events, TX/RX callbacks
│   ├── ethernet_iface.c      # Ethernet side: MAC rewriting, generic client learning, bridge TX/RX
│   ├── wifi_profile_store.c  # Multi-profile Wi-Fi store (NVS)
│   ├── wifi_profile_prov.c   # BLE profile-management endpoint
│   ├── provisioning.c        # BLE provisioning (network_provisioning + NimBLE)
│   ├── usb_ncm_iface.c       # (standby) USB NCM wired interface; not compiled in the Ethernet scheme
│   └── manual_config.c       # (standby) manual configuration scheme
├── docs/
│   ├── 官方示例改动说明.md     # Itemized changes vs the official example, with rationale & verification (Chinese)
│   ├── WT32-ETH01硬件说明.md  # Module composition, pins, clock design, official links (Chinese)
│   ├── 设备接入示例.md          # Generic onboarding walkthrough & IPC field-test experience (Chinese)
│   └── WiFi多配置管理.md      # NVS data layout and BLE management protocol (Chinese)
├── mbedtls_preset_bridge.conf   # mbedtls trim preset (SHA1 fixed)
└── sdkconfig.defaults        # All config deltas of this repo (promiscuous mode, BLE, PHY, clock…)
```

## 9. License & Acknowledgements

- The code is adapted from **Espressif's official example `examples/network/sta2eth`** (ESP-IDF v6.0.2); the original files keep Espressif's SPDX tags (`Unlicense OR CC0-1.0`), and the modifications in this repository are released under the same license;
- Licenses of the dependency components (`espressif/ethernet_init`, `espressif/network_provisioning`, etc.) are as declared by the IDF Component Manager;
- WT32-ETH01 specification materials are copyrighted by the original vendor (Wireless-Tag / 启明云端); this repository **does not redistribute vendor PDFs** — only original notes and official links;
- All trademarks belong to their respective owners.

---

## Appendix: Special Environment Setup (non-default toolchain location)

Use the PowerShell entry script shipped with the repository; no manual path setup needed each time:

```powershell
cd E:\esp-idf\esp-idf-v6.0.2\project\wt32-eth01-bridge
.\idf.ps1 build
.\idf.ps1 -p COM15 flash monitor
.\idf.ps1 menuconfig
```

Defaults:

- ESP-IDF: `E:\esp-idf\esp-idf-v6.0.2`
- IDF toolchain: `E:\.espressif`
- Temp directory: `tmp\esp_temp` inside the repository

On a new machine or when the toolchain moves, just set environment variables — no script edits required:

```powershell
$env:WT32_IDF_PATH = "D:\esp-idf\esp-idf-v6.0.2"
$env:WT32_IDF_TOOLS_PATH = "D:\.espressif"
$env:WT32_TEMP = "D:\esp_temp"
.\idf.ps1 build
```

If PowerShell blocks local scripts, allow locally signed scripts for the current user:

```powershell
Set-ExecutionPolicy -Scope CurrentUser RemoteSigned
```
