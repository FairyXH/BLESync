# Windows 蓝牙注册表分析

日期：2026-08-20

## 范围和证据

本文档记录当前 Windows 安装上的第一次现场检查结果。它是实现基线，不代表所有 Windows 版本和驱动都使用完全相同的注册表结构。因此任何恢复策略都必须保守，并且需要版本和状态门控。

当前环境：

- Windows 10 Pro，构建号 22631，x64
- `bthserv`：运行中，手动启动，共享进程 `svchost.exe -k LocalService -p`
- Bluetooth PnP 适配器：`Generic Bluetooth Adapter`，状态 OK
- 当前存在已配对设备以及多个 `BTH`/`BTHENUM` 枚举项
- 当前提升用户可以读取 `BTHPORT\Parameters\Devices`
- 当前非 SYSTEM 检查上下文无法读取 `BTHPORT\Parameters\Keys`

## 注册表区域判断

| 路径 | 作用判断 | 双系统策略 | 风险 |
|---|---|---|---|
| `HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Devices` | 设备记录、名称、元数据和配对相关状态 | 第一优先级快照对象，只同步递归设备子树 | 设备结构可能随驱动变化 |
| `HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters\Keys` | 适配器和设备认证材料，包括 Link Key | 第一优先级敏感对象，只允许 LocalSystem 访问，Storage 使用 ACL | 等价于凭据，绝不能记录或暴露 |
| `HKLM\SYSTEM\CurrentControlSet\Enum\BTH` | Bluetooth 总线和设备节点 | 不直接复制，由当前 PnP 重新生成 | 复制可能造成设备实例冲突 |
| `HKLM\SYSTEM\CurrentControlSet\Enum\BTHENUM` | Bluetooth Profile/服务枚举节点 | 不直接复制，由当前 PnP 重新生成 | 依赖设备实例和驱动 |
| `HKLM\SYSTEM\CurrentControlSet\Control\DeviceClasses` | 设备接口注册信息 | 不直接复制 | 影响范围大，可能破坏系统设备接口 |
| `HKLM\SYSTEM\CurrentControlSet\Services\BTHPORT\Parameters` | Bluetooth Stack 配置和生成状态 | 只研究明确子项，不替换整棵树 | 可能包含适配器或运行时状态 |

## 必须进行的差分实验

在生产环境启用跨系统恢复前，应依次执行：

1. 配对前保存只读快照。
2. 配对一个新设备，等待 Bluetooth Stack 稳定，再保存快照。
3. 只删除这个设备，等待稳定，再保存快照。
4. 不配对设备，只切换蓝牙关闭/打开，记录服务、适配器和 Registry 变化。
5. 连接和断开一个已经配对的设备，记录变化。
6. 每一步同时记录 `bthserv` 状态、SetupAPI/PnP 状态、设备数量和规范化快照摘要。
7. 如果条件允许，使用 Process Monitor 对 `BTHPORT`、`BTH`、`BTHENUM` 设置路径过滤，关联用户操作和 Registry 写入。

生产代码必须以差分实验结果为证据，不能因为某个值位于列出的路径下就默认它可以跨 Windows 复制。

## 蓝牙状态层级

服务将以下维度分开处理：

- 服务状态：SCM 状态，例如 `STOPPED`、`START_PENDING`、`RUNNING`。
- 适配器状态：SetupAPI/PnP 是否存在、是否启动、Problem Code。
- Radio 状态：是否能枚举到 Bluetooth 设备类的已启动节点。
- 设备状态：规范化后的 BTHPORT Devices/Keys 快照。

未知是合法状态。未知状态下不执行自动服务控制，也不执行破坏性恢复。

## 当前实验限制

当前机器存在已配对设备，但没有完成受控的“配对前、配对后、删除后”全流程实验，也没有 Process Monitor 追踪文件。因此当前实现使用保守默认值：不复制 Enum/DeviceClasses，不周期性重启 `bthserv`，并在无法确认状态时放弃恢复。

当前机器已经完成的动态验证包括：

- 服务可注册并运行。
- 服务账户为 LocalSystem。
- Storage ACL 仅包含 SYSTEM 和 Administrators。
- Devices 和 Keys 快照可以在服务上下文生成。
- 快照具有备份文件和 SHA-256 元数据。
