# WT32-ETH01 通用无线网桥 · 多类型有线设备无感 WiFi 接入

> BLE 蓝牙配网 · WiFi↔以太网 L2 透明桥接 · 静态 IP / DHCP · 多 Wi-Fi 配置管理
>
> 让只有**有线网口**的设备（IP 摄像头、电脑、NVR、工控板等）——**插上网线即接入 WiFi 网络**，无需安装驱动。当前固件同一时间支持一台有线客户端。

---

## 一、应用场景

```
                 ┌──────────────────┐  网线   ┌─────────────────┐   WiFi    ┌────────┐
  局域网 / 云端  │ IPC / PC / NVR / │────────│  WT32-ETH01 网桥  │──────────│  路由器  │
                │ 工控板等有线设备 │        │ ESP32+LAN8720    │  2.4GHz  └────────┘
                 └──────────────────┘        └─────────────────┘
```

- 设备只拥有百兆以太网口、**没有 WiFi**，靠网桥接入无线网络；
- 网桥对设备透明：设备**保持自己的 IP**（静态或 DHCP 均可），同一局域网内仍可直接访问该 IP，无需端口映射；
- 支持不同设备类型依次接入，通过网线插拔触发客户端 MAC 重学；
- 网桥最多保存 16 组 Wi-Fi，正常启动只读取当前选中的一组；
- 换 WiFi 环境：**长按按键 2 秒** → 蓝牙配网（手机 App），全程不需要接电脑。

## 二、特性

| 特性 | 说明 |
|------|------|
| L2 透明桥接 | 以太网混杂模式 + MAC 改写，同一广播域、端到端会话，IP 层无 NAT |
| 多类型设备兼容 | IPC、电脑、NVR、工控板等标准以太网设备共用同一固件；一次接入一台 |
| **通用 MAC 学习** | 从有线侧第一个合法单播帧学习，不依赖 DHCP 或 ARP；网线断开后自动重学 |
| BLE 蓝牙配网 | 长按 IO2（2 秒）进入 ESP 统一配网，手机 App（ESP BLE Provisioning）下发 WiFi 凭据 |
| 多 Wi-Fi 配置库 | 默认保存 16 组 SSID/密码，NVS 持久化；启动时只加载当前配置 |
| BLE 配置管理端点 | 自定义 App 可列出、选择、删除 Wi-Fi，并设置选择优先级 |
| 开机自动回连 | 当前凭据存 NVS，断电重启自动重连；连不上自动回退配网模式 |
| 拥塞可观测 | WiFi 发送队列丢帧计数告警（`WiFi TX drop #N`），量化空口拥塞 |
| 省电关闭 | `WIFI_PS_NONE`，避免下行流量在休眠窗口丢失 |

## 三、硬件

| 器件 | 型号/要求 | 备注 |
|------|-----------|------|
| 网桥 | WT32-ETH01（ESP32-D0WD-V3 + LAN8720AI，百兆） | 5V 供电 |
| 有线设备 | 任意带标准 10/100M 以太网口和 TCP/IP 栈的设备 | 网桥不向外设供电，IPC 等设备需独立供电 |
| 实测参考设备 | 海思 HI3516CV610 方案 IPC 模组 | **DC 12V/2A，不能依赖 POE 供电** |
| 路由器 | 任意 2.4GHz WiFi 路由器 | 建议关闭"无线隔离/AP 隔离"；有双频时监控端连 5G 可显著改善延迟 |

> 硬件细节见 [`docs/WT32-ETH01硬件说明.md`](docs/WT32-ETH01硬件说明.md)，通用接入流程和摄像头实测示例见 [`docs/设备接入示例.md`](docs/设备接入示例.md)。

## 四、快速开始

### 1. 环境

- ESP-IDF **v6.0.2**：本工程由官方示例 `examples/network/sta2eth` 改造而来，**v6.0.2 为验证过的编译版本**；
  其他 IDF 版本的示例目录位置与内部 API 均有差异（如 `esp_wifi_internal_reg_rxcb` 等私有接口），
  直接换版本编译不保证通过；
- 依赖组件随 `main/idf_component.yml` 由 Component Manager 自动拉取：`espressif/ethernet_init`、`espressif/network_provisioning`；
- Windows 下若工具链不在默认位置，需设置环境变量（参考文末附录）

### 2. 编译烧录

WT32-ETH01 **没有自动下载电路**，烧录需手动进 bootloader：

1. **IO0 接 GND** → 模块断电再上电（进入下载模式）
2. 烧录：
   ```powershell
   idf.py -p COM15 flash monitor
   ```
3. **拔掉 IO0 接地线** → 断电重新上电，模块自动运行

> 刷机后首次开机（NVS 无凭据）自动进入蓝牙配网；已有配置时串口会打印
> `Loaded N profiles, active id M`，用于确认当前配置库状态。

### 3. 蓝牙配网（换 WiFi 也用这一步）

1. 手机安装 **ESP BLE Provisioning** App（Espressif 官方）；
2. **长按网桥 IO2 按键 2 秒**（触发重配网，设备重启进入配网模式）；
3. App 扫描设备 → 发送 WiFi 名称/密码 → 配网成功后设备自动重启进入桥接；
4. 串口看到 `Wi-Fi STA connected` 即成功。

官方 App 只负责新增网络。后续自定义 App 可通过 BLE 端点 `wifi-profiles` 管理已保存网络：

```text
LIST
GET_ACTIVE
SELECT <id>
DELETE <id>
PRIORITY <id> <0-255>
```

完整协议见 [`docs/WiFi多配置管理.md`](docs/WiFi多配置管理.md)。

### 4. 接入有线设备

1. DHCP 设备直接接入即可；静态 IP 设备配置为**与路由器同网段、掩码一致、网关指向路由器**；
2. 网线接入网桥；直连插拔会触发链路重连和自动 MAC 重学；
3. 从同一 WiFi 的电脑 `ping` 设备 IP，或在串口确认 `Wi-Fi STA connected`；
4. 串口出现 `Wired client MAC learned: xx:xx:...` = 设备已被网桥识别；
5. 更换设备时断开并重新插入网线；通过交换机更换且链路不中断时，需要重启网桥。

> 摄像头供电、静态 IP 和 RTMP 推流验证步骤见 [`docs/设备接入示例.md`](docs/设备接入示例.md)。

## 五、性能实测（供参考）

| 项目 | 数据 | 条件 |
|------|------|------|
| iperf3 TCP（单跳） | 13~14 Mbps | 对端接路由器有线侧 |
| iperf3 TCP（两跳同频） | ~9.6 Mbps | 对端与网桥同为路由器 2.4G 无线终端 |
| ping 延迟（两跳） | 5~11 ms | 信号 RSSI -44 时 |
| 摄像头 RTMP 推流 | 可用 | 两跳场景建议码率 ≤2~3 Mbps，并观察串口 `WiFi TX drop` 计数 |

> 同频两跳的吞吐约为单跳的 5~7 折（空口时间共享所致），是物理规律而非故障。改善：路由器开双频，监控端走 5G。

## 六、与官方示例的差异

本工程基于 Espressif 官方 `sta2eth` 示例（ESP-IDF）改造。**逐项改动说明（含原因与验证方法）见 [`docs/官方示例改动说明.md`](docs/官方示例改动说明.md)**，概要：

1. **硬件适配三项**：WPA2 握手需要 SHA1（默认预设被禁）；LAN8720 复位脚 GPIO16 需拉高；RMII 50MHz 时钟为 GPIO0 **输入**（LAN8720 晶振倍频输出）；
2. **配网方案**：网页配网 → **BLE 蓝牙配网**（`network_provisioning` + NimBLE）；
3. **开启以太网混杂模式**：官方默认关闭，关闭时 EMAC 硬件过滤会丢弃"目的 MAC 非本机"的单播帧——这是"能上网但无法与 WiFi 侧设备互通"的直接根因；
4. **通用 MAC 学习**：官方 MAC 槽位只认 DHCP DISCOVER，静态 IP 设备无法接入；现从首个合法上行单播帧学习，并在链路断开后清空重学；
5. **多 Wi-Fi 配置库**：NVS 保存多组凭据，正常启动只读取当前一组，不引入逐包开销；
6. **BLE 管理端点**：官方配网继续负责新增网络，自定义端点负责查看、选择、删除和优先级管理；
7. **TX 丢包计数**：官方对 WiFi 发送失败只打 LOGD（不可见），现改为 WARN 级计数告警。

## 七、已知限制与排查口诀

| 限制/现象 | 原因 | 处理 |
|-----------|------|------|
| 电脑拿到 192.168.4.x | WiFi 没连上，掉进了配网模式（内置 DHCP） | 查 WiFi 环境后**断电**重启（软重启会停留在配网模式） |
| ping 通但 TTL=128 / 0ms | 应答的是电脑自己（IP 撞车），不是目标设备 | 检查本机 IP 配置，`arp -d *` 后用正确 IP 重测 |
| 能上网但无法与 WiFi 侧设备互通 | 混杂模式未开启（本仓库已默认开启） | 确认 `CONFIG_EXAMPLE_ETHERNET_USE_PROMISCUOUS=y` |
| 通过交换机更换有线设备后新设备不通 | 链路没有断开，网桥仍保持第一个客户端 MAC | 断开网桥与交换机链路后重试，或重启网桥 |
| 第二台有线设备同时接入 | 当前设计只支持单台有线客户端 | 使用交换机后仍需保证同一时间只有一台业务设备 |
| 推流延迟持续升高不回落 | 空口容量不足 → 看串口 `WiFi TX drop` 计数 | 降码率（≤3 Mbps）、双频分流、换干净信道 |

## 八、目录结构

```
├── main/
│   ├── bridge_main.c         # 主逻辑：桥接/配网分支、WiFi 事件、收发回调
│   ├── ethernet_iface.c      # 以太网侧：MAC 改写、通用客户端学习、桥接收发
│   ├── wifi_profile_store.c  # 多 Wi-Fi NVS 配置库
│   ├── wifi_profile_prov.c   # BLE 配置管理端点
│   ├── provisioning.c        # BLE 配网（network_provisioning + NimBLE）
│   ├── usb_ncm_iface.c       # (备用) USB NCM 有线接口，以太网方案下不参与编译
│   └── manual_config.c       # (备用) 手动配置方案
├── docs/
│   ├── 官方示例改动说明.md     # 逐项改动、原因、验证方法
│   ├── WT32-ETH01硬件说明.md  # 模块构成、引脚、时钟设计、官方资料链接
│   ├── 设备接入示例.md          # 通用接入流程与 IPC 实测经验
│   └── WiFi多配置管理.md      # NVS 数据结构和 BLE 管理协议
├── mbedtls_preset_bridge.conf   # mbedtls 裁剪预设（已修正 SHA1）
└── sdkconfig.defaults        # 本仓库全部配置差异（混杂模式、BLE、PHY、时钟…）
```

## 九、许可与致谢

- 代码基于 **Espressif 官方示例 `examples/network/sta2eth`**（ESP-IDF v6.0.2）改造，原文件保留 Espressif 的 SPDX 标识（`Unlicense OR CC0-1.0`）；本仓库的修改同样以该许可发布；
- 依赖组件（`espressif/ethernet_init`、`espressif/network_provisioning` 等）许可随 IDF Component Manager 声明；
- WT32-ETH01 规格资料版权归原厂（Wireless-Tag / 启明云端）所有，本仓库**不转载原厂 PDF**，只提供原创整理笔记与官方链接；
- 各商标归其所有者所有。

---

## 附录：特殊环境配置（工具链非默认位置）

```powershell
cd E:\esp-idf\esp-idf-v6.0.2
set IDF_TOOLS_PATH=E:\.espressif
set TEMP=E:\esp_temp
set TMP=E:\esp_temp
.\export.ps1
cd project\wt32-eth01-bridge
idf.py build
```
