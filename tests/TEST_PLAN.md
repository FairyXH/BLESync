# BLESync 命令行与日志本地化

命令行输出、帮助信息、错误提示和服务日志统一使用简体中文。参数名称仍保留稳定的英文形式，便于脚本调用。

支持帮助参数：

```text
BLESync.exe --help
BLESync.exe -h
BLESync.exe /?
BLESync.exe /h
```

未知参数返回退出码 `2`，并提示使用 `--help` 或 `/?` 查看用法。


## 详细日志验证

自动测试和动态测试应检查：

- 首次启动会记录当前已配对设备数量和脱敏列表。
- Registry 增加、删除、修改会记录对象名称、类型和长度。
- 恢复时会记录向系统写入的设置值、删除值、访问键和删除键数量。
- 日志不得包含 Link Key、REG_BINARY 原文或连续长十六进制密文。


自动测试只执行不会改变当前机器蓝牙配对状态的检查：

- MinGW 编译和 EXE 存在性。
- 从 EXE 同目录读取 INI。
- 检查 `StoragePath` 和 `ScanInterval`。
- 未知 CLI 参数必须返回非零退出码。
- `--status` 必须在有限时间内返回。
- `--capture` 在有权限时生成 Devices/Keys 快照；权限不足时必须失败而不是崩溃。
- Storage 元数据和快照文件必须存在。

运行：

```powershell
powershell -ExecutionPolicy Bypass -File tests\run_static_tests.ps1
```

## 动态测试

### 服务注册

1. 服务不存在时执行 `BLESync.exe --install`。
2. 确认服务名为 `BLESync`。
3. 确认账户为 `LocalSystem`。
4. 确认启动方式为自动且启用延迟启动。
5. 确认状态进入 `RUNNING`。
6. 重复执行安装，确认不会产生第二个服务。

### 蓝牙关闭或状态未知

1. 在 Windows 设置中关闭蓝牙，或者在无法改变设备状态时模拟服务不可用。
2. 启动 BLESync。
3. 确认 BLESync 不调用 `StartService` 或 `ControlService` 控制 `bthserv`。
4. 确认日志只记录等待或跳过，不记录恢复成功。

### 快照和完整性

1. 执行 `--capture`。
2. 确认生成 `devices.regdata` 和 `keys.regdata`。
3. 确认存在 `state\metadata.ini`。
4. 再次捕获，确认生成 `.backup`。
5. 截断快照后重启服务，确认服务拒绝使用损坏文件并保留当前 Registry。

### 配对增加和删除

1. 记录配对前快照。
2. 配对一个新设备并等待稳定。
3. 确认本地变化发布到 Storage。
4. 删除该设备并等待稳定。
5. 确认 Storage 中对应设备和 Key 被删除。

### 双系统

1. Windows A 和 Windows B 使用同一个非系统 StoragePath。
2. A 配对设备，等待快照发布。
3. 进入 B，确认服务启动时尝试恢复并重新读取验证。
4. 在 B 删除设备，确认删除传播到 Storage。
5. A 再次启动，确认不把旧设备强行恢复。

## 当前机器已完成的动态验证

- `BLESync.exe --status` 在有限时间内返回。
- 服务安装成功。
- 服务显示为 `RUNNING`。
- 服务账户为 `LocalSystem`。
- 服务配置为自动、延迟启动。
- Storage 目录生成。
- Devices/Keys 快照生成。
- `.backup` 文件生成。
- Storage ACL 只包含 SYSTEM 和 Administrators。
- 服务日志生成，未发现 Link Key 明文。

真实配对/删除和第二 Windows 的验证仍需要双系统实验环境，不能由单机测试替代。
