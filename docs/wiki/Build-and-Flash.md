# Build and Flash

## 1) Environment
- Required: ESP-IDF `v5.5.x`
- Shell: PowerShell

Initialize the configured IDF profile:

```powershell
. "C:\Espressif/Initialize-Idf.ps1" -IdfId esp-idf-b29c58f93b4ca0f49cdfc4c3ef43b562
```

## 2) Build
```powershell
idf.py build
```

## 3) Flash + Monitor
```powershell
idf.py -p <PORT> flash monitor
```

## 4) Common Build-Time Config
- `sdkconfig.defaults` sets default target and TinyUSB options.
- `main/Kconfig.projbuild` exposes project-level options for Wi-Fi/SNTP/TZ.
