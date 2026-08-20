# BLESync Wi-Fi 同步设计

## 目标

Wi-Fi 与 Bluetooth 共用 BLESync.exe、Windows Service、INI、Logger、Storage ACL、原子文件写入和调度循环，但业务策略完全独立。

```text
Bluetooth = Local <-> Persistent Mirror
             add / modify / delete

Wi-Fi      = Local + Persistent -> Global Union -> Available Interfaces
             add / modify / never delete from Global
```

## 运行边界

`WifiSyncManager` 不控制 `WlanSvc`，不调用 `netsh`，不启动或停止 WLAN AutoConfig，不自动打开 Wi-Fi。Bluetooth 的 `bthserv` 状态机和 Registry 逻辑不依赖 Wi-Fi 结果。

WLAN API 只允许针对当前存在的 Interface GUID 读写。适配器拔出后，Windows 会清理该接口状态，不能对历史 GUID 调用 `WlanSetProfile`。因此：

- Global profile 文件保留离线接口产生的 profile。
- 重新出现的接口通过 `WlanEnumInterfaces` 重新发现并传播。
- 不直接改写 `C:\ProgramData\Microsoft\Wlansvc`。
- `Wlansvc\Profiles\Interfaces` 只读扫描作为离线历史发现 fallback；若权限或格式不允许则记录 Pending，不影响服务。

## 数据目录

```text
StoragePath\wifi\
├─ profiles\
│  └─ <sha256>.xml
├─ pending\
│  └─ <sha256>.pending
└─ metadata.ini
```

Wi-Fi XML 和 Bluetooth Keys 都受到 Storage 根目录递归 DACL 保护，仅 SYSTEM 和 Administrators 可访问。日志只记录 hash、计数、接口 GUID 的脱敏摘要和错误码，不记录 XML、SSID、keyMaterial 或 plaintext key。

## Profile 身份

全局 profile 以规范化 XML 的 SHA-256 作为版本身份。规范化只处理 BOM、换行和 XML 声明外围空白，不重排 WLAN schema 节点，避免改变 Windows 认证语义。Profile name 作为传播时的 Windows 名称，不作为唯一身份。

同名不同 hash 版本均保存在 Global 集合。传播到一个接口时只能选择一个同名版本，母版排序为：

1. 当前连接的版本；
2. 本轮当前接口观察到的版本；
3. 最近观察到的版本；
4. 其余 Global 版本。

未被选中的版本不从 Global 删除，并记录冲突数量。

## 启动和周期流程

```text
Service Start
  -> WifiSyncManager 初始化 WlanOpenHandle
  -> 注册 WLAN ACM notification
  -> 读取 Global XML
  -> 读取当前可见接口 profile
  -> 只读发现 Wlansvc 历史 XML
  -> Global = Global union Local
  -> 原子写入新增/更新 Global XML
  -> 对每个当前接口调用 WlanSetProfile
  -> 离线接口保留 Pending

每次 Wi-Fi due
  -> 若 Wlan API 暂时不可用，记录 Pending，等待下次
  -> notification dirty 或 fallback interval 到期时枚举
  -> 只在 hash 不同或 profile 缺失时 WlanSetProfile
  -> 失败不删除 Global，保留 pending
```

删除本地 profile 不会触发 Global 删除，也不会删除 `WlanDeleteProfile`。这是明确的 Wi-Fi 只增不删语义；用户需要接受后续同步可能重新传播该 profile。

## 通知

注册 `WLAN_NOTIFICATION_SOURCE_ACM`。回调只设置 dirty 标志，不在回调中调用 WLAN API，不获取同步锁，不注销通知。主服务循环在安全线程中处理 dirty。通知丢失时由周期 fallback 扫描补偿。

## 敏感数据

`WlanGetProfile` 不请求 `WLAN_PROFILE_GET_PLAINTEXT_KEY`。Windows 返回的 `keyMaterial` 仍可能是本机保护的加密数据；LocalSystem 能否解密不等于它能跨 Windows 安装复用。802.1X/EAP 用户数据也可能独立于 profile XML。跨系统 WPA/WPA3/802.1X/DPAPI 可用性必须通过双系统实测，不能由 XML 文件存在性推断。

## 故障策略

- Wi-Fi API 失败：只暂停 Wi-Fi，Bluetooth 继续运行。
- Interface 不存在：保留 Global，生成 Pending 语义，等待未来接口出现。
- `WlanSetProfile` 返回 capability mismatch、access denied 或 invalid parameter：记录错误码和 hash，不删除任何 Global 数据。
- WlanSvc stopped/pending：不重启服务，等待下一次扫描。
- 状态未知：不改变 Wi-Fi 开关、不连接网络、不删除配置。

## 未验证边界

当前实现可以验证 API 枚举、Global union、原子 XML 存储、通知注册和当前接口传播。以下仍需真实实验：

- 不同 Windows 安装之间 DPAPI 加密 keyMaterial 是否可用。
- WPA3、802.1X/EAP 用户凭据跨系统迁移。
- 拔出后重新插入相同 USB 适配器的 Interface GUID 是否变化。
- 用户 profile 与 LocalSystem 服务可见性差异。
- 同名不同 XML 版本在每种 Windows 版本上的覆盖行为。
