# BLESync

BLESync 是一个以 Windows `LocalSystem` 权限运行的蓝牙状态持久化服务。它将经过筛选的 Bluetooth Port 注册表状态保存到非系统分区，目标是让使用同一物理蓝牙适配器的两个 Windows 安装之间共享配对状态。

## 当前实现范围

当前实现同步以下两个注册表区域：

```text
HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices
HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Keys
```

`Keys` 包含配对认证材料，服务必须以 `LocalSystem` 运行。程序不会把密钥写入日志，也不会修改原注册表权限。

以下区域不会被机械复制：

```text
HKLM\SYSTEM\CurrentControlSet\Enum\BTH
HKLM\SYSTEM\CurrentControlSet\Enum\BTHENUM
HKLM\SYSTEM\CurrentControlSet\Control\DeviceClasses
```

这些区域包含当前 Windows 安装生成的 PnP 实例、设备接口和驱动状态。盲目复制可能导致设备实例冲突。

跨 Windows 恢复是否能让某个具体设备直接工作，取决于 Windows 版本、驱动、适配器地址和设备协议。必须先执行 `docs\BluetoothRegistryAnalysis.md` 中的差分实验。

### 重要实现边界

- `BLESync.exe` 无参数运行时会请求管理员权限，注册/更新 `BLESync` 服务并启动；服务实例通过 SCM 进入 `ServiceMain`。
- `--capture` 是当前权限下的只读/写入测试入口。管理员上下文读取 `Keys` 可能被拒绝；正式服务以 `LocalSystem` 运行后才是有效的敏感分支验证路径。
- Storage ACL 会递归应用到已有文件和目录，只允许 `SYSTEM` 与 `Administrators`。
- 服务只在 Bluetooth 服务为 `RUNNING` 且 Bluetooth PnP 节点已启动时处理快照；`STOPPED`、`START_PENDING`、`STOP_PENDING`、适配器禁用或未知状态都不会强行启用或重启 Bluetooth。
- 首次观察到本地状态时建立基线；稳定本地增加/删除优先发布到共享 Storage。共享 Storage 与当前本地状态冲突时保留本地状态并记录冲突，不静默覆盖用户操作。
- 恢复使用 `InstanceId`、`SnapshotVersion`、SHA-256、原子替换以及有限恢复窗口；跨系统冲突的最终业务决策仍需在双系统实验中验证。


需要 64 位 MinGW-w64，并确保 `g++` 在 `PATH` 中：

```bat
build.bat
```

输出文件：

```text
build\BLESync.exe
build\BLESync.ini
```

构建脚本使用 `--console=plain` 等 Gradle 选项不适用；本项目完全使用 MinGW `g++`，编译输出写入 `build\compile.log`。

## Wi-Fi 同步扩展

BLESync 现已在同一个 `BLESync.exe` / `BLESync` Service 中增加 Wi-Fi Profile 同步。Bluetooth 和 Wi-Fi 共享 Service、配置、Storage、ACL、日志和调度，但业务状态独立：

```text
Bluetooth = Mirror（增加、修改、删除）
Wi-Fi     = Global Union + Propagation（增加、修改、永不从 Global 删除）
```

新增配置保持向后兼容：

```ini
[BLESync]
LogSensitiveNames=false

[WiFi]
Enabled=true
ScanInterval=5
SyncOnServiceStart=true
SyncOnWiFiEnable=true
```

Wi-Fi 数据保存于：

```text
StoragePath\wifi\profiles\<sha256>.xml
StoragePath\wifi\pending\
```

Wi-Fi 使用 `WlanOpenHandle`、`WlanEnumInterfaces`、`WlanGetProfileList`、`WlanGetProfile`、`WlanSetProfile` 和 `WlanRegisterNotification`。不使用 `netsh` 作为核心同步机制，不调用 `WlanDeleteProfile`，不重启 `WlanSvc`。

当前可见 WLAN Interface 的 Profile 会合并到 Global Union，并传播到所有当前可见 WLAN Interface。适配器拔出后，Windows WLAN API 不允许继续对已移除 Interface GUID 操作；BLESync 保留 Global XML，待接口未来重新出现后再次传播。详细边界见：

- `docs\WifiProfileAnalysis.md`
- `docs\WifiSyncDesign.md`

Wi-Fi Profile XML 可能包含本机保护的 `keyMaterial` 或企业认证信息。BLESync 不请求 plaintext key、不把 XML/密钥写入日志。DPAPI、WPA3、802.1X/EAP 跨 Windows 可用性必须通过双系统实测，不能由 XML 文件存在性推断。

## 命令行帮助

```text
BLESync.exe --help
BLESync.exe -h
BLESync.exe /?
BLESync.exe /h
```

帮助输出、状态输出、错误提示和服务日志使用简体中文；参数名保持英文以兼容脚本。


使用管理员命令提示符运行：

```bat
install.bat
uninstall.bat
```

也可以直接调用：

```text
BLESync.exe --install
BLESync.exe --uninstall
BLESync.exe --capture
BLESync.exe --status
BLESync.exe --console
```

服务信息：

```text
服务名：BLESync
显示名：BLESync Bluetooth Persistence Service
账户：LocalSystem
启动方式：自动启动
```

安装会配置服务失败后的自动重启，并使用逐次增加的重启延迟，避免逻辑错误造成快速重启循环。

## 配置文件

`BLESync.ini` 必须位于 EXE 同目录，程序不依赖当前工作目录：

```ini
[BLESync]
StoragePath=D:\BLESyncData
ScanInterval=5
LogLevel=INFO
```

`ScanInterval` 最小值为 1 秒。Storage 目录不存在时由服务创建。配置或目录初始化失败时，服务停止同步，不会强制启动或重启蓝牙服务。

| `StoragePath` | `D:\BLESyncData` | 蓝牙快照和日志目录 |
| `ScanInterval` | `5` | 扫描间隔，单位为秒，最小 1 秒 |
| `LogSensitiveNames` | `false` | 默认不记录 Wi-Fi SSID/Profile 名称 |
| `[Bluetooth] Enabled` | `true` | 保留并显式控制原 Bluetooth 模块 |
| `[WiFi] Enabled` | `true` | 启用 Wi-Fi Global Union + Propagation |
| `[WiFi] ScanInterval` | `5` | Wi-Fi fallback 扫描周期，秒 |
| `[WiFi] SyncOnServiceStart` | `true` | 服务启动时执行 Wi-Fi 扫描/传播 |
| `[WiFi] SyncOnWiFiEnable` | `true` | Wi-Fi 状态通知后执行同步 |


默认结构：

```text
D:\BLESyncData\
├─ registry\
│  ├─ devices.regdata
│  ├─ devices.regdata.backup
│  ├─ keys.regdata
│  └─ keys.regdata.backup
├─ state\
│  └─ metadata.ini
├─ logs\
│  └─ BLESync.log
│     BLESync.log.previous
```

快照使用自定义二进制格式，带版本标记 `BLESNAP2`。写入过程为：临时文件、`FlushFileBuffers`、备份旧文件、原子替换。快照内容使用 Windows CNG `SHA-256` 计算摘要，元数据记录摘要和大小。

## 同步策略

- 蓝牙服务处于 `START_PENDING` 或 `STOP_PENDING` 时等待。
- 无法确认蓝牙适配器或 Radio 状态时不恢复、不强制打开蓝牙、不重启 `bthserv`。
- 服务首次运行且没有有效持久化快照时，把当前本地状态作为基线。
- `BTHPORT\Parameters\Devices/Keys` 快照校验成功且本地已一致时不会重复写入。
- `Enum\BTH` / `Enum\BTHENUM` 没有对应设备实例时，即使 BTHPORT 数据存在，也只能报告“配对数据已持久化”，不能报告“设备已在设置界面恢复”。
- 稳定运行期间检测到本地增加或删除时，把当前本地状态发布到 Storage，因此支持增加和删除镜像。
- 当 Storage 与本地状态冲突时，当前策略保留稳定本地状态并记录冲突，不静默覆盖用户刚刚执行的配对或删除操作。
- 使用全局命名互斥体保护共享 Storage，并设置 2 秒超时；超时后安全退出当前工作线程，不无限等待。
- 正常运行期间绝不周期性 stop/start `bthserv`。

## 详细日志

- `BLESync.log` 是当前服务进程日志；新进程启动时旧文件移动为 `BLESync.log.previous`。
- `BLESync.log.previous` 最多保留一份，下一次新进程启动会覆盖它。
- 日志写入调用 `FlushFileBuffers`，避免进程异常退出时丢失最后记录。
- 蓝牙状态边沿和注册表通知用于缩短用户 OFF→ON 后的同步等待，不调用 `bthserv` 重启。

服务会记录但不会记录 Link Key 原文：
- Registry 子键增加、删除。
- Registry value 增加、删除、修改。
- value 类型和字节长度；不会写入敏感二进制内容。
- 从 Storage 恢复时向系统写入的 Devices/Keys 设置值、删除值、访问键和删除键数量。
- 恢复完成后的重新读取验证结果。
- 本地和持久化状态冲突时的两份设备列表摘要。



Storage ACL 限制为：

```text
NT AUTHORITY\SYSTEM：完全控制
BUILTIN\Administrators：完全控制
```

普通用户不能读取 Storage。Link Key 和其他敏感二进制数据不会进入日志。不要把 StoragePath 配置到普通用户共享目录。

## 蓝牙设备可见性边界

当前同步对象是：

```text
BTHPORT\Parameters\Devices
BTHPORT\Parameters\Keys
```

这些数据代表配对记录和认证材料，但 Windows 设置界面还依赖当前安装生成的：

```text
Enum\BTH
Enum\BTHENUM
Control\DeviceClasses
Device Container / PnP instance state
```

本机实测：BTHPORT 中存在两个设备目录，而 `Enum\BTHENUM` 没有对应设备实例，Windows Bluetooth API 只能枚举到一个设备。因此复制 `Devices/Keys` 不能保证设备在设置界面出现，也不能保证可连接。

BLESync 不直接复制这些 PnP/设备接口树，也不伪造设备实例，不调用设备重启或 `bthserv` 重启。服务只会请求一次安全的 Bluetooth PnP 重新枚举并记录实际可见数量。完整跨系统恢复必须在目标 Windows 上重新配对，或另行实现经过验证的 PnP/设备容器重建方案。



Wi-Fi 正常运行期间“本地删除不删除 Global，后续可能重新传播”是本项目的明确设计，不是 Windows API 的默认行为。当前 Windows WLAN Native API 只能对当前存在的 Interface GUID 写入 Profile；离线适配器只保留历史 Profile 贡献，重新插入后重新枚举并传播。

当前主机已实际观察到 `Wlansvc\Profiles\Interfaces` 下多个历史 Interface 目录。BLESync 会在只读模式下发现这些 XML 并并入 Global，但不会直接修改 `Wlansvc` 私有文件。

## 测试

静态测试：

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_static_tests.ps1
```

动态验证需要管理员权限。当前系统已验证过服务注册、LocalSystem 运行、自动启动（非延迟）、Storage ACL、Devices/Keys 快照生成、日志轮换和 Bluetooth PnP 重新枚举请求。真实的“配对新设备、删除设备、重启 Windows、第二系统恢复”仍需要可控的双系统蓝牙实验环境。

## 完成度与未验证边界

已在当前 Windows 主机实际验证：

- MinGW-w64 `g++` 构建成功。
- CLI 帮助、状态、未知参数和有限超时运行。
- 服务启动类型为 `SERVICE_AUTO_START`，不使用延迟自动启动。
- 服务 `RUNNING`、`LocalSystem`、绝对 EXE 路径、自动启动（非延迟）。
- Service Recovery 配置入口已实现。
- Storage 目录、递归 ACL、快照、备份、SHA-256 元数据和 InstanceId。
- 当前机器的 `Devices`/`Keys` 在 LocalSystem 服务上下文中的读取与恢复验证。
- 日志未包含 Link Key 明文或连续长十六进制密文。

尚未在本机完成、不能伪称完成：

- 受控的新设备配对前后差分实验。
- 受控的设备删除前后差分实验。
- 蓝牙设置 OFF/ON 的完整状态转换实验。
- Windows 重启后的恢复验收。
- 第二个 Windows 安装共享同一 Storage 的端到端验收。
- Process Monitor 对真实 Registry 写入时序的采集。
## 故障排查
- 查看 `StoragePath\logs\BLESync.log`。
- 使用 `sc.exe queryex BLESync` 检查服务状态。
- 如果 `Keys` 读取失败，确认服务实际账户是 `LocalSystem`，不要放宽注册表权限。
- 如果快照损坏，服务应保留当前 Registry，不要手工导入未知 Registry 文件。
- 如果服务快速退出，检查 Windows 事件查看器中的 `Application Error` 和 `Service Control Manager` 事件。
