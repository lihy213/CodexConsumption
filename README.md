# CodexConsumption

CodexConsumption 是一个 Windows 原生 C++ 小工具，用于显示 Codex 的 5h 和 1w 剩余额度。

## 功能

- 自动读取本机 Codex 登录信息：`%USERPROFILE%\.codex\auth.json`
- 自动请求 Codex 用量接口并显示剩余额度
- 支持桌面悬浮小窗
- 支持固定到任务栏区域显示
- 固定到任务栏时自动避让托盘、任务栏图标和 TrafficMonitor 的 CPU/内存显示窗口
- 鼠标悬浮时显示 5h 和 1w 的下一次刷新时间
- 右键菜单：立即刷新、固定/取消固定到任务栏、退出
- Explorer/任务栏重绘后自动维护窗口位置

## 使用

直接运行：

```text
dist-native\CodexAssumptionNative.exe
```

启动后右键小工具，可选择固定到任务栏。

## 构建

需要 Visual Studio 2022 C++ 工具链。

```bat
native\build-native.bat
```

输出文件：

```text
dist-native\CodexAssumptionNative.exe
```

## 配置

配置文件位于：

```text
dist-native\config.json
```

常用字段：

- `refresh_seconds`：自动刷新间隔
- `taskbar_status_pinned`：是否启动后固定到任务栏
- `taskbar_status_native_embed`：是否启用实验性的原生嵌入逻辑，默认关闭
- `taskbar_status_width`：悬浮模式宽度
- `taskbar_status_pinned_width`：任务栏模式宽度

## 说明

本项目的任务栏交互逻辑参考了 TrafficMonitor 的设计思路：隐藏主窗口负责定时器和消息处理，小型自绘状态窗口负责显示，并在固定模式中持续维护位置和层级。
