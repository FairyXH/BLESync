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

## 编译

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
启动方式：自动启动、延迟自动启动
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
| `LogLevel` | `INFO` | 日志级别预留配置 |


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
└─ logs\
   └─ BLESync.log
```

快照使用自定义二进制格式，带版本标记 `BLESNAP2`。写入过程为：临时文件、`FlushFileBuffers`、备份旧文件、原子替换。快照内容使用 Windows CNG `SHA-256` 计算摘要，元数据记录摘要和大小。

## 同步策略

- 蓝牙服务处于 `START_PENDING` 或 `STOP_PENDING` 时等待。
- 无法确认蓝牙适配器或 Radio 状态时不恢复、不强制打开蓝牙、不重启 `bthserv`。
- 服务首次运行且没有有效持久化快照时，把当前本地状态作为基线。
- 存在有效持久化快照时，启动阶段尝试按注册表树恢复，并在恢复后重新读取验证。
- 稳定运行期间检测到本地增加或删除时，把当前本地状态发布到 Storage，因此支持增加和删除镜像。
- 当 Storage 与本地状态冲突时，当前策略保留稳定本地状态并记录冲突，不静默覆盖用户刚刚执行的配对或删除操作。
- 使用全局命名互斥体保护共享 Storage，并设置 2 秒超时；超时后安全退出当前工作线程，不无限等待。
- 正常运行期间绝不周期性 stop/start `bthserv`。

## 详细日志

服务会记录但不会记录 Link Key 原文：

- 当前已配对设备数量和脱敏后的设备标识。
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

## 测试

静态测试：

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_static_tests.ps1
```

动态验证需要管理员权限。当前系统已验证过服务注册、LocalSystem 运行、延迟自动启动、Storage ACL、Devices/Keys 快照生成和日志写入。真实的“配对新设备、删除设备、重启 Windows、第二系统恢复”仍需要可控的双系统蓝牙实验环境。

## 故障排查

- 查看 `StoragePath\logs\BLESync.log`。
- 使用 `sc.exe queryex BLESync` 检查服务状态。
- 如果 `Keys` 读取失败，确认服务实际账户是 `LocalSystem`，不要放宽注册表权限。
- 如果快照损坏，服务应保留当前 Registry，不要手工导入未知 Registry 文件。
- 如果服务快速退出，检查 Windows 事件查看器中的 `Application Error` 和 `Service Control Manager` 事件。
