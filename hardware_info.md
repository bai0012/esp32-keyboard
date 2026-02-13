# **ESP32-S3 Custom MacroPad \- 硬件参考手册**

**版本**: v1.0

**主控**: Espressif ESP32-S3

**日期**: 2026-02-13

## **1\. 项目概述 (Overview)**

本项目是一个基于 **ESP32-S3** 的高性能客制化小键盘（MacroPad）。它集成了机械按键、EC11 旋钮、OLED 显示屏、电容触摸条以及 RGB 灯光系统。通过 ESP32-S3 的 Native USB 接口，既可以作为普通键盘使用，也可以通过编程实现复杂的宏指令、媒体控制或系统监控功能。

## **2\. 核心规格 (Specifications)**

| 参数 | 描述 |
| :---- | :---- |
| **MCU** | **ESP32-S3** (Xtensa® 32-bit LX7 Dual-core, 240MHz) |
| **Flash / PSRAM** | 8MB Flash |
| **电源输入** | USB Type-C (5V) |
| **工作电压** | 3.3V (板载 AMS1117-3.3 LDO) |
| **输入交互** | 12x 机械x EC11 旋转编码器 (带按键), 2x 触摸滑条 |
| **视觉反馈** | 15x SK6812MINI-E (RGB LED), 0.96" OLED (128x64) |
| **听觉反馈** | 无源蜂鸣器 (Passive Buzzer) |
| **连接性** | Native USB (GPIO 19/20), Wi-Fi 2.4GHz, BLE 5.0 |

## **3\. 引脚映射表 (Pinout Map)**

### **3.1 输入设备 (Inputs)**

| 组件标签 | GPIO 引脚 | 硬件逻辑 | 备注 | 对应灯光顺序 |
| :---- | :---- | :---- | :---- | :---- |
| **Key 1** | **IO 7** | Active Low (按下接地) | 需开启内部上拉 | **Index 4** |
| **Key 2** | **IO 8** | Active Low | 需开启内部上拉 | **Index 5** |
| **Key 3** | **IO 9** | Active Low | 需开启内部上拉 | **Index 6** |
| **Key 4** | **IO 17** | Active Low | 需开启内部上拉 | **Index 7** |
| **Key 5** | **IO 18** | Active Low | 需开启内部上拉 | **Index 11** |
| **Key 6** | **IO 12** | Active Low | 需开启内部上拉 | **Index 10** |
| **Key 7** | **IO 13** | Active Low | 需开启内部上拉 | **Index 9** |
| **Key 8** | **IO 14** | Active Low | 需开启内部上拉 | **Index 8** |
| **Key 9** | **IO 1** | Active Low | 需开启内部上拉 | **Index 12** |
| **Key 10** | **IO 2** | Active Low | 需开启内部上拉 | **Index 13** |
| **Key 11** | **IO 40** | Active Low | 需开启内部上拉 | **Index 14** |
| **Key 12** | **IO 41** | Active Low | 需开启内部上拉 | **Index 15** |
| **EC11 A相** | **IO 4** | 脉冲信号 | 建议硬件/内部上拉 |  |
| **EC11 B相** | **IO 5** | 脉冲信号 | 建议硬件/内部上拉 |  |
| **EC11 按键** | **IO 6** | Active Low | 旋钮中键 |  |
| **Touch Left** | **IO 11** | 电容触摸通道 11 | 滑条左侧区域 |  |
| **Touch Right** | **IO 10** | 电容触摸通道 10 | 滑条右侧区域 |  |

### **3.2 输出设备 (Outputs)**

| 组件标签 | GPIO 引脚 | 协议/驱动 | 详细参数 |
| :---- | :---- | :---- | :---- |
| **RGB LED** | **IO 38** | RMT (1-Wire) | SK6812MINI-E (GRB时序), 共15颗 |
| **OLED SDA** | **IO 15** | I2C Data | SSD1315/SSD1306, 地址 0x3C |
| **OLED SCL** | **IO 16** | I2C Clock |  |
| **Buzzer** | **IO 21** | PWM (LEDC) | 无源蜂鸣器,2.7kHz |

### **3.3 系统引脚 (System)**

| 功能 | GPIO 引脚 | 描述 |
| :---- | :---- | :---- |
| **USB D-** | **IO 19** | Native USB Data- |
| **USB D+** | **IO 20** | Native USB Data+ |
| **Boot** | **IO 0** | 按下上电进入下载模式 |
| **Reset** | **EN** | 复位芯片 |

## **4\. 组件详解与布局逻辑 (Component Details)**

### **4.1 OLED 显示屏**

* **大小**: 0.96寸  
* **驱动芯片**: SSD1315 (完全兼容 SSD1306)  
* **分辨率**: 128 x 64 单色  
* **通信**: I2C (Standard Mode 100kHz / Fast Mode 400kHz)  
* **软件库**: 推荐 u8g2 (通用性强) 或 esp\_lcd。

### **4.2 RGB 灯光布局 (SK6812)**

灯珠采用串联方式连接，数据流方向如下：

1. **Index 0 \- 2**: 功能指示灯 (位于顶部，用于显示层级/状态/触摸反馈)。  
   * Index 0: 对应 C 键位  
   * Index 1: 对应 B 键位  
   * Index 2: 对应 A 键位  
2. **Index 3 \- 14**: 按键背光 (对应 Key 1 \- Key 12)。

### **4.3 EC11 旋转编码器**

* **滤波**: 硬件已有100nF滤波，建议在软件中使用 PCNT 驱动的 Glitch Filter (毛刺过滤器)，设定为 1us (1000ns) 可完美消除抖动。  
* **逻辑**: 正转 (CW) 值增加，反转 (CCW) 值减少。

### **4.4 电容触摸条**

* **形状**: 有两个三角形组成一个矩形，左侧为IO11，右侧为IO10