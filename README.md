# ESP32 Bluetooth Keyboard

ESP32-S3 开发板固件：320×240 屏幕当终端，USB 串口和蓝牙键盘都能敲命令，板载按键负责配色、开机画面、回连和发现。

板子是幻尔科技 **ESP32S3 Large Model Board V1.1**，模组 **ESP32-S3-WROOM-1-N16R8**（16 MB Flash，8 MB PSRAM）。原理图 V1.0（2025.10.30）。

目录名仍是 `hello_world`。CMake 工程名是 `esp32_bluetooth_keyboard`。

## 能做什么

- 屏幕终端：8×16 ASCII，40 列 × 15 行，满屏向上滚。超长输出自动换行，一次最多 5 行，截断处显示 `[...]`。
- USB 串口行编辑：本地回显、`>` 提示符、上翻历史。
- 蓝牙 HID 键盘：逐键回显，Enter 才提交。和串口共用同一套命令和 8 条历史。
- 命令由各模块自己注册。`help` 列出关键字和一行说明。
- `trace screen` / `trace serial` 两个开关互不影响，分别控制屏幕和串口输出。
- 板载 KEY1–KEY4：改字色、开机画面、短按回连、长按发现、串口输出开关。
- 一条 I2C 总线同时服务背光、按键和 5 路扩展座。`i2c` 命令可以扫描、读寄存器、写寄存器。

## 按键

按键在 XL9555 上，按下为低电平。组合键按掩码精确匹配，短按 KEY1 不会顺带触发 KEY1|KEY4。

| 按键 | 动作 |
| --- | --- |
| KEY1 | 文字改成红色 |
| KEY2 | 文字改成绿色 |
| KEY3 短按 | 回连上次的键盘。先直连已保存地址，失败再扫描 |
| KEY3 长按约 1 秒 | 发现新的 HID 设备，弹出选择 |
| KEY4 | 重放开机画面 |
| KEY1 + KEY4 | 翻转串口 trace 开关 |

发现或翻页时：KEY1、KEY2、KEY3 选择当前页的设备，KEY4 翻页。

## 命令

在串口或蓝牙键盘里输入，Enter 执行。说明必须是 ASCII，屏幕字库只有 32–126。

| 命令 | 作用 |
| --- | --- |
| `help` | 列出已注册命令 |
| `color <name\|hex>` | 改文字颜色。名字：black white red green blue yellow cyan magenta。也接受 RGB565（4 位十六进制）或 RGB888（6 位） |
| `splash` | 重放开机画面，每行间隔 0.5 秒 |
| `trace` | 打印 screen、serial 两个开关的当前值 |
| `trace screen` / `trace serial` | 只看这一路是 0 还是 1 |
| `trace screen 1` / `trace screen 0` | 打开或关闭屏幕输出 |
| `trace serial 1` / `trace serial 0` | 打开或关闭串口输出 |
| `connect <n>` | 连接上一次扫描列表里的第 n 台设备，从 1 计 |
| `i2c` 或 `i2c scan` | 扫描 0x08–0x77，列出有应答的地址 |
| `i2c read <addr> <reg> [n]` | 读寄存器，n 默认 1，最多 16。地址和字节可用十进制或 `0x` |
| `i2c write <addr> <reg> <bytes...>` | 写寄存器 |

默认屏幕输出开、串口输出关。两个都关掉之前，状态行会先打出来。

## 硬件接线（固件实际使用的部分）

| 功能 | 连接 |
| --- | --- |
| 屏 ST7789，横屏 320×240 | SPI：MOSI GPIO47，SCLK GPIO21，CS GPIO2，DC GPIO3。复位走板级 RST，软件复位 |
| 背光 | XL9555 IO1_6 拉高，经三极管点亮 |
| 按键 KEY1–KEY4 | XL9555 IO0_4–IO0_7，按下为低 |
| I2C | 先试 GPIO4/5，再试 GPIO38/48。XL9555 常见地址 0x20。5 个扩展座和背光、按键是同一条总线 |
| 烧录和调试口 | 原生 USB（USB Serial/JTAG）。本机端口是 **COM8** |
| 另一路 USB | CH340。只能当普通串口，不能用来下载 |

屏的方向：`swap_xy`，左右镜像，小端 RGB565，颜色反相打开。字是黑底，前景色运行时默认白色。

蓝牙是 BLE 4.2（Bluedroid），走 HID over GATT。配对是 Just Works，绑定信息在 NVS（`bt_config.conf`，最多 15 个）。上次连上的键盘名字和地址另外记在 NVS 命名空间 `bt_hid`。

更细的网表见 [board_schematic.md](board_schematic.md) 和 [board_database.md](board_database.md)。

## 依赖

固定使用已经装好的 **ESP-IDF 5.4.4**。不要升到 6.x。

| 项 | 本机路径或版本 |
| --- | --- |
| ESP-IDF | `D:\Espressif\frameworks\esp-idf-v5.4.4` |
| IDF 工具根 | `D:\Espressif`（`IDF_TOOLS_PATH` 必须指到这里） |
| Python | 3.11.2，`D:\Espressif\python_env\idf5.4_py3.11_env` |
| 编译器 | IDF 自带的 Xtensa ESP32-S3 GCC，随 `idf.py` 激活 |
| 主机 | Windows，PowerShell |
| 下载线 | 板上原生 USB，端口 COM8，波特率 460800 |

`sdkconfig.defaults` 里打开的选项：USB Serial/JTAG 控制台、Bluedroid BLE 4.2、`CONFIG_BT_GATTC_NOTIF_REG_MAX=16`、中断看门狗 3000 ms。

仓库里的 `package.json` 是早期 Agent SDK 脚手架，**编固件用不到**。固件只依赖 ESP-IDF。

## 编译和烧录

每个新开的 PowerShell 都要先激活 IDF。激活只对当前窗口有效。

```powershell
$env:IDF_TOOLS_PATH = "D:\Espressif"
$env:IDF_PATH = "D:\Espressif\frameworks\esp-idf-v5.4.4"
$py = "D:\Espressif\tools\idf-python\3.11.2\python.exe"
$idf_exports = & $py "$env:IDF_PATH\tools\activate.py" --export
. $idf_exports

cd D:\AI_Proj\hello_world
idf.py -p COM8 build flash
```

`idf.py monitor` 会占住 COM8。看完日志后退出监视器，再烧录。COM8 被其他串口软件占用时，烧录会报 `PermissionError 13`（拒绝访问）。拔掉原生 USB 时，端口会消失，CH340 那路不能代替它下载。

`menuconfig` 必须在本工程目录里跑。在 IDF 根目录执行会报 “Current directory is not buildable”。

## 目录

```
main/hello_world_main.c    上电：射频域复位、I2C、背光、屏、命令注册、按键
main/lcd_text.c            字符终端、开机画面、color / splash
main/debug_port.c          USB 串口行编辑
main/command_interpreter.* 命令注册、help、历史提交
main/command_history.*     8 条命令历史
main/trace.c               屏幕/串口两个输出开关
main/button.*              按键掩码、短按、长按、选择会话
main/bluetooth.c           HID 扫描、配对、回连、键盘行
main/i2c.*                 共享 I2C 和 i2c 命令
main/font8x16.inc          ASCII 8×16 字模
sdkconfig.defaults         需要强制打开的 Kconfig
```

交互是怎么一步步长成现在这样的，写在 [docs/generation.md](docs/generation.md)。
