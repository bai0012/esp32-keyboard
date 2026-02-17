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

## 3) Size Report
```powershell
idf.py size
```

Use this after build to verify app image growth against OTA slot capacity.

## 4) Flash + Monitor
```powershell
idf.py -p <PORT> flash monitor
```

USB enumeration note:
- Some hosts show one COM port for bootloader and another for the app.
- App-level `MACROPAD` logs are held until CDC is connected to avoid dropping early lines during that switch.
- Firmware startup does not wait for CDC; peripheral init and startup feedback continue immediately.

## 5) Common Build-Time Config
- `sdkconfig.defaults` sets default target and TinyUSB options.
- `main/Kconfig.projbuild` exposes project-level options for Wi-Fi/SNTP/TZ.
- `partitions_8mb_ota.csv` defines 8MB flash layout with dual OTA + `cfgstore`.
