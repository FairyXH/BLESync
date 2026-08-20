# BLESync 设计说明

## 目标和边界

BLESync 不重定向整个 Windows Registry Hive，也不替换 `SYSTEM` 文件。它以 Windows 服务方式运行，只处理经过明确选择的 Bluetooth Port 注册表分支。

目标是持久化蓝牙配对身份和相关认证材料，而不是复制所有 Windows PnP 生成数据。`Enum\BTH`、`Enum\BTHENUM`、`Control\DeviceClasses` 仍由 Windows 当前安装生成，不进行机械复制。

## 当前实现与设计修正

本版本相对早期基线增加并修正：

1. `build.bat` 每次都重新编译，避免误用旧 EXE；编译输出写入 `build\compile.log`。
2. 使用 `MachineId`、`SnapshotVersion`、双快照 SHA-256 和原子 metadata 更新支持双系统审计基础。
3. Storage ACL 递归覆盖现有快照和日志，避免旧文件保留宽松权限。
4. 服务启动时只有在本地与 Storage 确实不同且尚未建立本地运行基线时才恢复；本地稳定变化优先发布。
5. 恢复有有限验证窗口，避免简单的 Restore -> Scan -> Restore 循环。
6. 服务控制器每次安装都会刷新绝对二进制路径、账户、自动启动配置和恢复策略。
7. 无参数启动和 `--install`/`--uninstall` 支持 UAC `runas` 提权。


程序分为两种模式：

1. 控制器模式：定位 EXE 所在目录，读取同目录 INI，创建或更新服务，配置 LocalSystem、延迟启动和失败恢复，然后启动服务并退出。
2. 服务模式：通过 SCM 注册服务入口，运行工作线程，响应停止和关机控制码。

核心职责包括：

- 配置解析
- 文件日志
- Storage ACL
- Registry 递归读取
- Registry 递归写回
- 快照序列化和校验
- Bluetooth PnP 状态检测
- Bluetooth 服务状态检测
- 周期同步

## Wi-Fi Architecture

The service now starts a separate `WifiSyncManager` after the shared Storage mutex is acquired. It uses Native WLAN API and a one-second shared worker loop; Bluetooth registry decisions remain in their existing branch.

- `WlanRegisterNotification` marks Wi-Fi state dirty.
- The scheduler calls `WifiSyncManager::tick` when dirty or when the Wi-Fi interval expires.
- Wi-Fi API failures do not stop the Bluetooth branch.
- Wi-Fi never calls `WlanDeleteProfile`, `netsh`, `StartService`, or `ControlService` for `WlanSvc`.
- Global profile XML is stored under `StoragePath\wifi\profiles` using hash-based names and atomic replacement.
- Failed interface propagation creates a pending marker; later scans retry when an interface is available.
- Offline interfaces are not written while absent; their Global profiles remain stored.

See `docs\WifiProfileAnalysis.md` and `docs\WifiSyncDesign.md` for API evidence, key protection, offline-interface limits, conflict handling, and unverified cross-system DPAPI/EAP boundaries.

## Registry 快照

快照树由以下对象组成：

```text
Key
├─ name
├─ values[name] = {type, binary data}
└─ children[name] = Key
```

支持 `REG_SZ`、`REG_EXPAND_SZ`、`REG_BINARY`、`REG_DWORD`、`REG_QWORD`、`REG_MULTI_SZ` 等 Registry value 类型，因为数据按类型和原始字节保存。

快照格式包含：

```text
BLESNAP2
FormatVersion=2
RecursiveKeyTree
```

保存时先生成 `.tmp` 文件，写入并调用 `FlushFileBuffers`，然后保留 `.backup` 并使用 `MoveFileExW` 原子替换目标文件。

## 完整性校验

快照载荷通过 Windows CNG `BCrypt*` API 计算 SHA-256。`state\metadata.ini` 记录：

```ini
[Meta]
Hash=...
Bytes=...
UpdatedTick=...
```

读取快照时会重新计算当前载荷摘要；格式损坏或摘要无法计算时拒绝使用。当前实现已经记录摘要，但后续还应把元数据摘要比对作为单独的拒绝条件，避免只验证二进制格式而不验证外部元数据。

## 同步算法

服务维护当前已确认的本地快照：

```text
local_devices
local_keys
have_local
```

每轮扫描：

1. 查询 `bthserv` 状态。
2. 查询 Bluetooth 类 PnP 设备是否存在以及是否 `DN_STARTED`。
3. 如果服务处于 Pending，等待。
4. 如果 Radio 未知或未启用，不执行自动恢复。
5. 读取 Devices 和 Keys。
6. 如果没有有效 Storage 快照：发布当前本地快照作为基线。
7. 如果有 Storage 快照但本进程尚未建立本地基线：尝试恢复两个目标分支，再重新读取验证。
8. 如果本地快照相对上轮发生变化：视为用户在当前系统进行了增加或删除，发布本地状态。
9. 如果 Storage 与本地都稳定但不一致：记录冲突并保留本地状态。

这种策略优先保护用户刚刚执行的配对、删除和开关操作，不使用“Storage 总是覆盖本地”的暴力策略。

## 删除同步

Registry 镜像写回使用递归比较：

- Snapshot 中存在的 value 使用 `RegSetValueExW` 写入。
- Snapshot 中不存在的 value 使用 `RegDeleteValueW` 删除。
- Snapshot 中存在的子键递归处理。
- Snapshot 中不存在的子键使用 `RegDeleteTreeW` 删除。

该逻辑只允许用于明确批准的 BTHPORT 分支，不能用于 `Enum` 或 `DeviceClasses`。

## 详细日志和写入审计

日志记录以下摘要，不记录 Link Key 或其他敏感二进制原文：

- 当前已配对设备数量和脱敏后的设备标识。
- Devices/Keys 子键的增加和删除。
- Registry value 的增加、删除和修改。
- value 类型和数据长度。
- 恢复时实际向系统写入的设置值、删除值、访问键和删除键数量。
- 恢复完成后的重新读取验证结果。
- 冲突时的本地设备列表和持久化设备列表摘要。

设备标识只保留首尾少量字符；REG_BINARY 内容只报告类型和长度。



服务状态和适配器状态分开处理：

```text
BluetoothServiceState：SCM 查询结果
BluetoothAdapterState：SetupAPI/CM_Get_DevNode_Status
BluetoothDeviceState：Devices/Keys 快照
```

`bthserv == RUNNING` 不等于蓝牙适配器已启用。当前实现使用 Bluetooth 设备类的 SetupAPI 枚举和 `DN_STARTED`、Problem Code 作为适配器门控。

未知状态的策略是：

```text
不恢复
不启动蓝牙
不重启 bthserv
等待下一轮
```

## 用户操作优先

BLESync 不拦截 Windows 蓝牙服务启动，也不 Hook Service Control Manager。用户在设置中打开或关闭蓝牙时，服务只观察状态变化并延迟同步。

用户主动配对新设备后，本地 Registry 变化会被识别为本地稳定变化并发布。用户主动删除设备后，本地缺失同样会发布，从而允许删除传播到共享 Storage。

## 并发和超时

两个 Windows 可能同时写入同一个目录。服务使用：

```text
Global\BLESync.StorageLock
```

保护 Storage。获取锁最多等待 2 秒；超时则记录错误并退出本次服务工作线程，避免死锁。文件写入也使用共享读写策略和原子替换。

当前版本采用保守冲突策略：发现 Storage 与本地稳定状态不一致时不静默覆盖本地，而是记录摘要并继续观察。生产版本还应增加实例 ID、单调版本号、基线版本和显式冲突文件，以支持两个系统的可审计合并。

## 服务恢复

服务配置：

```text
账户：LocalSystem
启动：SERVICE_AUTO_START
延迟启动：禁用
失败恢复：1 分钟、2 分钟、5 分钟后重启
```

服务自身不会在扫描周期中调用 `StartService` 或 `ControlService` 控制 `bthserv`。

## 验证边界

当前系统可验证：

- MinGW 编译
- CLI 参数
- 服务注册和启动
- LocalSystem 账户
- 延迟自动启动
- Storage ACL
- Devices/Keys 读取
- 快照文件和备份
- 日志
- 无周期性蓝牙服务重启

当前系统无法单机证明：

- 新设备真实配对后跨系统恢复
- 删除设备后的第二系统同步
- 重启 Windows 后 Windows Bluetooth Stack 是否接受全部 Keys/Devices 数据
- 两个不同系统同时写 Storage 时的业务级合并

这些项目必须在双系统、同适配器、可控配对设备的实验环境中验证。
