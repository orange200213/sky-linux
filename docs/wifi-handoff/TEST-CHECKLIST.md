# 上机测试检查表

## 设备与版本

- [ ] 设备为 sky / SM4450 + WCN6750/Adrastea
- [ ] 运行内核版本与模块构建内核一致
- [ ] 设备树与固件版本已记录
- [ ] 原有模块、固件和日志已备份

## 模块

- [ ] sha256 校验通过
- [ ] `modinfo wlan.ko` 通过
- [ ] `modinfo icnss2.ko` 通过
- [ ] 先加载 icnss2，再加载 wlan
- [ ] 无 vermagic、Unknown symbol、依赖解析错误

## 基础功能

- [ ] wlan 网卡出现
- [ ] `iw phy` 可见
- [ ] 扫描成功
- [ ] WPA2/WPA3 连接成功
- [ ] 断开后可再次连接

## cfg80211 重点

- [ ] ROC cookie 请求与 ready/expired 一致
- [ ] mgmt TX cookie 与 TX status 一致
- [ ] mgmt TX 失败能返回错误
- [ ] 非零 link/radio 请求被拒绝
- [ ] change_beacon 成功更新
- [ ] change_beacon 失败后旧 beacon 保持
- [ ] control-port cookie/status 结果已记录
- [ ] key install/reconnect/delete 结果已记录

## SSR

- [ ] SSR 前保存完整 dmesg
- [ ] 受控触发 WPSS/firmware SSR
- [ ] QMI client 重建
- [ ] ICNSS FW_READY 状态恢复
- [ ] wlan/wiphy 恢复
- [ ] SSR 后扫描/连接恢复
- [ ] 无 oops、UAF、hung task、firmware crash

## 回传

- [ ] 完整 dmesg
- [ ] modinfo
- [ ] iw/ip 输出
- [ ] 测试命令和时间线
- [ ] 成功/失败及复现次数
- [ ] 保留失败现场，不清理日志
