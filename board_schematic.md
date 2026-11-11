# ESP32S3 Large Model Board V1.0 原理图解析

来源：`1.4. ESP32S3开发板原理图_V1.0(1).pdf`  
厂家：幻尔科技有限公司  
模组：ESP32-S3-WROOM-1-N16R8（16MB Flash + 8MB OPI PSRAM）  
日期：2025.10.30

---

## 1. 显示屏 ST7789（SPI）

原理图标题里写了 `LCD屏(D0-D8)`，实际网表是 **4 线 SPI**，不是 8080 并口。

| 信号 | ESP32-S3 GPIO | 说明 |
| --- | --- | --- |
| `LCD_MOSI` | **GPIO47** | SPI MOSI，与 TF 卡共用 |
| `LCD_SCLK` | **GPIO21** | SPI SCLK，与 TF 卡共用 |
| `LCD_CS` | **GPIO2** | LCD 片选 |
| `LCD_DC` | **GPIO3** | 数据/命令 |
| `RST` | 系统 `RST` 网络 | 与板级复位相连，**不是独立 GPIO** |
| `BACKLIGHT` | **XL9555 IO1_6** | 经 Q3（S8050）灌电流点亮背光 |

### 供电

- 逻辑电源：`VDD2.8`（U15 `RT9166A-28PXL`，由 `VDD33` 经 FB1 降压得到）
- 背光阳极：`LEDA` / `A` 接电源；阴极 `LEDK` 经三极管到地
- `BACKLIGHT` 高电平 → 背光亮

### FPC 侧主要引脚

| FPC | 网络 |
| --- | --- |
| 1 | GND |
| 2 | LEDK |
| 3 | A（背光阳极） |
| 4 | VDD2.8V |
| 7 | RS（= `LCD_DC`） |
| 8 | CS |
| 9 | RS/SCK（= `LCD_SCLK`） |
| 10 | SDA（= `LCD_MOSI`） |
| 11 | RST |

分辨率原理图未标注；驱动芯片为 ST7789，常见 240×240 / 240×320，需上电实测确认。

### 点屏最小结论

1. SPI 主机用 **GPIO47 / GPIO21 / GPIO2 / GPIO3**
2. panel reset 用软件复位（`reset_gpio_num = -1`）
3. 背光必须先让 **XL9555** 把 `IO1_6` 拉高，否则屏会一直黑

---

## 2. 触摸 FT6206 / FT6336（I2C）

| 信号 | 连接 | 说明 |
| --- | --- | --- |
| SDA | `SDA1` = **GPIO4** | 与 ES8311、EEPROM 同总线 |
| SCL | `SCL1` = **GPIO5** | |
| INT | `TP_INT` → **XL9555 IO1_1** | 触摸中断进 IO 扩展器 |
| RST | `TP_RST` → 系统 `RST` | 跟板级复位相连 |

实测地址：**0x38**（寄存器特征符合 FT6x36 系列；文案写 FT6206，和 FT6336 同系）。

---

## 3. I2C 总线

板子有 3 组 I2C 网络，MCU 侧实际只有两路硬件脚；第三路通过排针跳线并入。

| 网络 | MCU 引脚 | 已确认器件 |
| --- | --- | --- |
| `SDA1` / `SCL1` | **GPIO4 / GPIO5** | ES8311(0x18)、触摸(0x38)、24C02 EEPROM(0x50) |
| `SDA2` / `SCL2` | **GPIO38 / GPIO48** | 外部 I2C 座 / 跳线 |
| `SDA3` / `SCL3` | 经 **J18 跳帽** 并到 SDA1 或 SDA2 | XL9555、QMI8658、AHT30、AP3216C |

### J18 跳线（`跳路帽二选一`）

```
SDA1 --1  6-- SCL1
SDA3 --2  5-- SCL3
SDA2 --3  4-- SCL2
```

中间脚 `SDA3/SCL3` 是板载传感器/扩展器总线。  
跳 1-2 / 6-5 → 并到 `SDA1/SCL1`（GPIO4/5）  
跳 2-3 / 5-4 → 并到 `SDA2/SCL2`（GPIO38/48）

**背光依赖 XL9555，因此点屏前要确认 J18 已把 SDA3 接到某一路 MCU I2C。**

### XL9555（U7）关键 IO

| 扩展脚 | 网络 | 方向（典型） |
| --- | --- | --- |
| IO1_7 | `MUTE` | 功放静音 |
| IO1_6 | `BACKLIGHT` | LCD 背光 |
| IO1_1 | `TP_INT` | 触摸中断输入 |
| IO1_0 | `AP_INT` | 光感中断 |
| IO0_7..4 | `KEY4`..`KEY1` | 按键 |
| IO0_1 | `IMU_INT1` | IMU 中断 |
| IO0_0 | `CAMERA_LED` | 摄像头灯 |
| INT | `IIC_INT` = **GPIO46** | 扩展器总中断 |
| I2C | `SDA3/SCL3` | |

地址脚 A0/A1/A2 在原理图上引出；默认焊接状态需对照实物（常见地址 `0x20`）。

---

## 4. 音频 ES8311 + NS4150

| 信号 | GPIO |
| --- | --- |
| Codec I2C | `SDA1/SCL1` = GPIO4/5，地址 **0x18** |
| `I2S_MCLK` | GPIO45 |
| `I2S_SCLK`（BCLK） | GPIO39 |
| `I2S_ASDOUT`（ADC→MCU） | GPIO40 |
| `I2S_LRCK`（WS） | GPIO41 |
| `I2S_DSDIN`（MCU→DAC） | GPIO42 |
| `MUTE` | XL9555 IO1_7 |

---

## 5. TF 卡（SPI，与 LCD 共用时钟/数据）

| 信号 | GPIO |
| --- | --- |
| `TF_CS` | **GPIO0（BOOT）** |
| `SPI_MOSI` | GPIO47（= `LCD_MOSI`） |
| `SPI_CLK` | GPIO21（= `LCD_SCLK`） |
| `SPI_MISO` | GPIO14 |

---

## 6. 摄像头 DVP

| 信号 | GPIO |
| --- | --- |
| DVP_Y2 | GPIO11 |
| DVP_Y3 | GPIO9 |
| DVP_Y4 | GPIO8 |
| DVP_Y5 | GPIO10 |
| DVP_Y6 | GPIO12 |
| DVP_Y7 | GPIO18 |
| DVP_Y8 | GPIO17 |
| DVP_Y9 | GPIO16 |
| DVP_PCLK | GPIO13 |
| DVP_XMCLK | GPIO15 |
| DVP_HREF | GPIO7 |
| DVP_VSYNC | GPIO6 |
| SCCB | `SDA1/SCL1` = GPIO4/5 |
| `CAMERA_LED` | XL9555 IO0_0 |
| `CAMERA_RST` | 网络名 `CAMERA_RST`（见原理图第 1 页摄像头座） |

---

## 7. USB / 调试

| 接口 | 用途 |
| --- | --- |
| USB JTAG/Serial（模组原生 USB，`USB_DP/DN`） | 下载 + 监视，对应本机 **COM8** |
| CH340C Type-C（`USB2_D±`） | USB-UART，对应本机 **COM11**；不直连芯片下载脚时可能无数据 |
| UART0 | `TXD0` / `RXD0` 经 CH340 |

---

## 8. 点屏 checklist

1. 确认 J18 已把 `SDA3` 并到 `SDA1` 或 `SDA2`
2. 初始化对应 I2C，扫描并找到 XL9555，拉高 `IO1_6`（背光）
3. SPI 初始化：`MOSI=47, SCLK=21, CS=2, DC=3`
4. `esp_lcd_new_panel_st7789`，`reset_gpio_num=-1`，再 `invert_color` 按面板实测开关
5. 先刷纯色，确认分辨率/偏移后再写文字

---

## 9. 与此前实测的对照

| 实测 | 原理图 |
| --- | --- |
| GPIO4/5 上有 0x18 / 0x38 / 0x50 | ES8311 / FT6x36 / EEPROM |
| 无 SSD1306（0x3C/0x3D） | 屏是 ST7789 SPI，不是 I2C OLED |
| 屏幕全黑 | SPI 未驱动 + 背光在 XL9555 上未打开 |
