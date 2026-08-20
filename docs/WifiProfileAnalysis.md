# Windows WLAN Profile 分析

日期：2026-08-20

## 结论摘要

BLESync 的 Wi-Fi 扩展应使用 Native WLAN API，不把 `netsh wlan` 作为核心实现，也不直接复制 `C:\ProgramData\Microsoft\Wlansvc` 文件作为写入接口。

当前主机的 `WlanSvc` 为 `Running / Automatic`。实际 Wlansvc 存储可观察到两类内容：

- `Profiles\Interfaces\{InterfaceGuid}\{ProfileGuid}.xml`：按 WLAN Interface GUID 分目录的底层 profile 文件。
- `WLAN-*.xml`、`WLAN 2-*.xml`、`WLAN 3-*.xml`：由系统/工具生成的导出或派生文件，不能仅凭存在就当作 Native WLAN Profile Store 的权威接口。

Windows 官方 API 的权威边界是 `WlanOpenHandle`、`WlanEnumInterfaces`、`WlanGetProfileList`、`WlanGetProfile`、`WlanSetProfile` 和 `WlanRegisterNotification`。

## API 事实

| API | 事实 | BLESync 用法 |
|---|---|---|
| `WlanOpenHandle` | 建立 WLAN API client handle | WifiSyncManager 初始化；失败只影响 Wi-Fi，不影响 Bluetooth |
| `WlanEnumInterfaces` | 枚举当前 WLAN API 可见的接口 | 用于当前可传播目标接口和运行状态；不能据此宣称覆盖所有离线接口 |
| `WlanGetProfileList` | 获取指定 Interface GUID 的 profile 名称和属性，按 preference order 返回 | 读取每个当前可见接口的 profile 列表 |
| `WlanGetProfile` | 获取指定接口和 profile name 的完整 XML；profile name 区分大小写 | 读取 XML、flag、granted access；不请求 plaintext key |
| `WlanSetProfile` | 针对指定接口新增或覆盖 profile；`dwFlags=0` 为 all-user，`WLAN_PROFILE_USER` 为 per-user | 只对当前可见接口调用；使用 all-user profile、`bOverwrite=TRUE`，失败进入 Pending |
| `WlanRegisterNotification` | 注册 ACM 等 WLAN 通知；回调可能异步发生 | 注册 ACM 通知，回调只置 dirty/event，不在回调中调用同步或注销 API |
| `WlanCloseHandle` | 关闭 WLAN handle，同时取消通知注册 | 服务停止时调用 |

## 离线适配器限制

官方文档明确指出 WLAN profile 操作都需要 Interface GUID；当无线接口被移除时，WLAN Service 会清除该接口状态，不能继续针对已移除 GUID 执行 profile 操作，`WlanSetProfile` 可能返回 `ERROR_INVALID_PARAMETER`。

因此 BLESync 不能仅依靠 `WlanEnumInterfaces` 恢复一个已拔出的适配器，也不能直接把历史 Interface GUID 当作当前写入目标。正确策略是：

1. 当前可见接口上的 profiles 合并进全局集合。
2. Global profiles 永不因为某个接口缺失而删除。
3. 离线接口只作为历史观测元数据保存，不执行写入。
4. 接口未来重新出现时，新的 Interface GUID/接口枚举项触发传播。
5. 不直接修改 `Wlansvc` 文件和 Registry 私有存储。

这满足“离线适配器不影响 Global Profile 保留”，但不能声称在接口物理不存在期间已经写入该接口。

## Profile 身份与合并

仅使用 profile name/SSID 不安全。同一 profile name 可能具有不同 XML、安全配置或企业认证配置。实现使用：

```text
ProfileId = SHA-256(CanonicalProfileXml)
```

并同时保存：

- ProfileName
- 原始 Profile XML
- XML hash
- 来源 Interface GUID（仅作为来源，不作为全局唯一边界）
- user/all-user flag
- 当前连接状态
- 最近观察时间

本阶段 canonicalization 使用安全的 XML 文本规范化：去除 UTF-8 BOM、统一 CRLF/LF 和 XML 声明周围空白；不重排 XML 节点、不修改密钥内容。这样不会误改 Windows WLAN schema。真正的 XML C14N 不是实现前提。

## 密钥和权限

`WlanGetProfile` 默认返回加密的 `keyMaterial`。官方文档说明：

- 不应设置 `WLAN_PROFILE_GET_PLAINTEXT_KEY`。
- 请求 plaintext key 需要 `wlan_secure_get_plaintext_key` 权限，默认仅本机 Administrators 可用。
- LocalSystem 在同一台计算机上可以使用 DPAPI 解密某些 key material，但这不证明该密钥可跨 Windows 安装复用。
- `WlanSetProfile` 接受 profile XML；Windows 会在保存前加密 plaintext key，并在随后读取时返回加密形式。
- 企业 802.1X/EAP 的用户数据可能不在 profile XML 中，可能需要 `WlanSetProfileEapUserData` 或用户上下文，不能假设 XML 单独足够。

BLESync 因此：

- 不请求 plaintext key。
- 不在日志输出 XML、SSID 原文或 keyMaterial。
- Storage 继承 BLESync 的 SYSTEM + Administrators ACL。
- 把 Profile XML 作为敏感持久化数据处理。
- 文档明确提示跨系统 DPAPI/EAP 兼容性未经验证。

## Master 选择

Global 合并规则：

```text
Global = Global ∪ Local
```

本地 Profile 不存在时不得从 Global 删除。

同名不同 XML hash 的冲突按以下顺序记录候选：

1. 当前连接的 profile 优先。
2. 当前扫描中连接状态更强的 profile 优先。
3. 最近观察到的本地 profile 优先。
4. 无法判断时保留两份版本，不静默删除候选。

当前实现传播所有 Global XML 版本；若同名 XML 版本无法在目标接口共存，记录冲突并以候选排序第一者尝试覆盖，不删除 Global 候选。

## 通知与轮询

注册 `WLAN_NOTIFICATION_SOURCE_ACM`，重点关注：

- interface arrival/removal
- connection complete
- disconnected
- radio state change
- profile change（如果系统版本提供）

回调不执行阻塞同步、不调用 `WlanRegisterNotification`，只设置 dirty 标志。周期扫描是最终一致性 fallback，因为官方文档明确建议通知未及时到达时查询当前 interface state。

## 实验与未验证边界

已验证：

- `WlanSvc` 当前运行。
- 本机 Wlansvc 存在按 Interface GUID 存放的 XML profile 文件。
- 官方 Native WLAN API 的 profile、set、notification 和权限语义。

未在本轮完成：

- 在当前机器上调用 `WlanGetProfileList`/`WlanGetProfile` 的实际 API 输出。
- 新建/修改/删除 Wi-Fi profile 前后通知差分。
- 同一 profile 跨两个 Windows 安装的真实连接验证。
- WPA/WPA3、802.1X/EAP、DPAPI key material 跨系统可用性。
- 拔出后重新插入同一个 USB WLAN 适配器的 Interface GUID 是否保持。

这些边界不能用 `Wlansvc` 文件名或单机 XML 存在性替代。

## 参考

- Microsoft Learn: `WlanGetProfile`
- Microsoft Learn: `WlanGetProfileList`
- Microsoft Learn: `WlanSetProfile`
- Microsoft Learn: `WlanRegisterNotification`
- Microsoft Learn: Native Wifi functions
