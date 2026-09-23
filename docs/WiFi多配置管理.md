# Wi-Fi 多配置管理

## 1. 设计目标

- 默认保存最多 16 组 Wi-Fi SSID/密码，可通过 Kconfig 调整到 32 组。
- 官方 ESP BLE Provisioning App 继续用于新增网络。
- 自定义 App 后续可以通过 BLE 管理已保存网络。
- 正常桥接启动只读取当前配置，不扫描全部网络。
- 不把 NVS 操作、扫描或切换逻辑放进 Ethernet/WiFi 数据转发路径。

## 2. NVS 数据结构

配置使用独立 namespace `wifi_profiles`：

| Key | 类型 | 内容 |
|-----|------|------|
| `count` | `u16` | 当前配置数量 |
| `active` | `u16` | 当前配置 ID，0 表示未选择 |
| `p0001` ... | blob | 单条 `wifi_profile_t` 配置 |

每条配置包含：

- SSID 和密码
- 认证类型
- 上次连接信道提示
- 优先级
- 最近 RSSI、失败次数
- 可选 BSSID
- 版本和结构长度

存储实现位于 `main/wifi_profile_store.c`。正常启动只调用一次
`wifi_profile_store_get_active()`，读取当前 profile 后写入 ESP Wi-Fi 驱动。

当前工程未自动启用 NVS encryption。生产版本如果要求防范 Flash 物理读取，
应启用 NVS encryption 或 Flash encryption，并把密钥按量产流程管理。

## 3. 新增网络

官方 App 的配网流程保持不变：

1. 手机连接 `PROV_` BLE 设备。
2. App 发送 SSID 和密码。
3. ESP32 连接目标 AP 并获得 IP。
4. 收到 `NETWORK_PROV_WIFI_CRED_SUCCESS` 后，配置自动写入 `wifi_profiles`。
5. 新配置被设为 `active`。

如果已有网络断开或用户长按 IO2，设备重新进入配网模式，重复上述流程即可增加配置。
配置已满时，新网络会替换优先级最低的一条记录，避免官方 App 配网成功后凭据无法保存。

## 4. BLE 管理端点

端点名称：`wifi-profiles`

该端点通过 `network_provisioning` 的自定义端点机制注册，和官方配网共享相同的
Security 1/2 加密会话。命令和响应均为 UTF-8 文本，响应体为 JSON。

### LIST

请求：

```text
LIST
```

响应：

```json
{
  "ok": true,
  "count": 2,
  "active_id": 1,
  "profiles": [
    {
      "id": 1,
      "ssid": "Office-2.4G",
      "priority": 10,
      "enabled": true,
      "authmode": 3,
      "channel": 6,
      "last_rssi": -127,
      "fail_count": 0
    }
  ]
}
```

密码不会通过该端点返回。

### GET_ACTIVE

```text
GET_ACTIVE
```

返回当前选中的 profile。没有选中项时 `profile` 为 `null`。

### SELECT

```text
SELECT 2
```

将 ID 2 设为当前网络，保存成功后设备约 800 ms 后重启进入正常桥接模式。

### DELETE

```text
DELETE 2
```

删除 ID 2。如果删除的是当前网络，固件会选择剩余记录中优先级最高的一条。

### PRIORITY

```text
PRIORITY 2 20
```

设置 ID 2 的优先级为 20。数值越大优先级越高，范围 0-255。

## 5. App 推荐交互

管理页面可以按以下流程实现：

1. 进入 BLE 配网/管理模式。
2. 打开加密会话并调用 `LIST` 展示已保存网络和当前选中项。
3. 点击网络调用 `SELECT`，设备重启后连接该网络。
4. 点击删除调用 `DELETE`。
5. 调整顺序时调用 `PRIORITY`。
6. “添加新网络”继续使用标准配网数据端点，而不是自定义命令。

注意：自定义端点必须在标准 Wi-Fi 配置成功前访问，因为标准配网成功后
`network_provisioning` 会自动停止服务。

## 6. 性能约束

- 正常桥接期间不启用 BLE 管理端点。
- 正常启动不扫描 Wi-Fi，只读取一条当前配置。
- 连接成功后保存信道提示，下一次可从该信道快速连接。
- 数据转发路径不调用任何 NVS API。
- 状态和统计先缓存在 RAM，避免逐包写 Flash。

因此，多配置容量不会增加 Ethernet/Wi-Fi 数据路径延迟。真正的耗时来源仍是
Wi-Fi 认证、关联和 DHCP，而不是 NVS 中保存的配置数量。
