# UE5 4GB/6GB VRAM Guard & MS Hybrid Frame Pacer (DXGI/D3D12 Proxy)

**v2.2 Universal Public Release**

## English

### Overview

UE5 4GB/6GB VRAM Guard is a Windows x64 DXGI/D3D12 proxy for Unreal Engine 5 games running on low-VRAM GPUs and MS Hybrid notebooks. It is designed to reduce severe frame-time spikes when VRAM pressure causes render targets, geometry buffers and streaming textures to move across the PCIe bus.

Typical target hardware includes GTX 1650 4GB, RTX 3050 4GB and RX 6500 XT. On these configurations, VRAM exhaustion can produce drops from approximately 50 FPS to 7 FPS and long traversal stutters. The proxy does not create GPU performance; it manages residency priorities, frame latency and UE5 streaming settings to make available VRAM use more predictable.

### Architecture

- **3-Tier DirectX 12 Residency Priority Controller** — uses `ID3D12Device1::SetResidencyPriority`:
  - `MAXIMUM` for render-target, depth-stencil and UAV resources;
  - `HIGH` for `D3D12_RESOURCE_DIMENSION_BUFFER` geometry resources;
  - `LOW` for large BC-compressed background textures;
  - an `ID3D12Heap` rank map prevents a heap already promoted to `MAXIMUM` from being overwritten by `HIGH` or `LOW`.
- **Smart UE5 Streamer Budget Clamp** — adjusts `IDXGIAdapter3::QueryVideoMemoryInfo` and keeps a safety reserve for drivers, desktop memory and MS Hybrid iGPU operation.
- **Post-Present high-resolution frame pacer** — uses a kernel waitable timer and `SetMaximumFrameLatency(1)`. Set `TargetFPS=0` to disable pacing.
- **Automatic lossless UE5 Hub Anti-Stutter CVar injector** — preserves text before the optimizer marker and updates only the optimizer section in supported `Engine.ini` files.
- **Universal configuration** — `dxgi_optimizer.ini` is created next to the game executable and controls budget, FPS, texture threshold, CPU priority and INI patching.

### Installation

1. Build or download the x64 release.
2. Copy `release/dxgi.dll` and `release/dxgi_optimizer.ini` next to the target Unreal Engine game's Win64 executable.
3. Keep the original `dxgi.dll` backed up. Remove the proxy to uninstall.
4. Start the game and inspect `dxgi_optimizer.log` beside the executable.

The proxy is intended for local testing and game installations where DLL proxy loading is permitted. Do not overwrite system files.

### Recommended configuration

| GPU memory | `VramBudgetMB` | `TargetFPS` | Notes |
|---:|---:|---:|---|
| 4 GB | 2850 | 60 | Recommended reserve for GTX 1650 / RTX 3050 / RX 6500 XT |
| 6 GB | 4600 | 60 | Recommended starting value for RTX 3060 / RX 5600 XT |

Set `VramBudgetMB=0` to disable the budget clamp. Set `TargetFPS=0` to disable frame pacing. Set `AutoPatchUE5Ini=0` to disable automatic CVar updates.

### Build

Open a **Visual Studio 2022 or newer x64 Native Tools Command Prompt**, enter `src`, and run:

```bat
build.bat
```

The build requires the MSVC C++ x64 toolchain, Windows SDK headers, DXGI/D3D12 headers and `vswhere.exe`. The output is `src/dxgi.dll`.

### Measurement

Use CapFrameX or PresentMon. Compare the same route and save location with the proxy disabled and enabled. Focus on 1% low, 0.1% low and frame-time spikes rather than average FPS alone. The default 60 FPS pacer is intended to improve frame-time consistency, not to increase the GPU's raw rendering capability.

## Русский

### Назначение

UE5 4GB/6GB VRAM Guard — x64 DXGI/D3D12-прокси для игр на Unreal Engine 5, видеокарт с небольшим объёмом VRAM и ноутбуков с MS Hybrid. Проект уменьшает резкие провалы времени кадра, когда из-за переполнения VRAM render targets, буферы геометрии и потоковые текстуры начинают перемещаться через шину PCIe.

Целевые конфигурации: GTX 1650 4 ГБ, RTX 3050 4 ГБ и RX 6500 XT. При переполнении VRAM на таких системах возможны падения с примерно 50 до 7 FPS и длительные traversal-фризы. Прокси не добавляет вычислительную мощность GPU: он управляет приоритетами residency, задержкой кадров и потоковой загрузкой UE5.

### Архитектура

- **Трёхуровневый контроллер DirectX 12 Residency Priority** через `ID3D12Device1::SetResidencyPriority`:
  - `MAXIMUM` для RT/DS/UAV;
  - `HIGH` для буферов геометрии `D3D12_RESOURCE_DIMENSION_BUFFER`;
  - `LOW` для крупных фоновых BC-текстур;
  - карта рангов `ID3D12Heap` не позволяет понизить уже защищённую кучу с `MAXIMUM` до `HIGH` или `LOW`.
- **Smart UE5 Streamer Budget Clamp** — обработка `IDXGIAdapter3::QueryVideoMemoryInfo` с резервом памяти для драйвера, рабочего стола и встроенной графики MS Hybrid.
- **Post-Present frame pacing** — высокоточный waitable timer ядра и `SetMaximumFrameLatency(1)`. Значение `TargetFPS=0` отключает ограничитель.
- **Автоматический lossless-инжектор UE5 Hub Anti-Stutter CVar** — сохраняет текст до маркера и меняет только собственную секцию `Engine.ini`.
- **Универсальная конфигурация** — `dxgi_optimizer.ini` создаётся рядом с exe игры.

### Установка

1. Соберите или скачайте x64-релиз.
2. Скопируйте `release/dxgi.dll` и `release/dxgi_optimizer.ini` рядом с Win64-exe игры UE5.
3. Сохраните оригинальный `dxgi.dll` в резервную копию. Для удаления прокси уберите установленную DLL.
4. Запустите игру и проверьте `dxgi_optimizer.log` рядом с exe.

Не заменяйте системный `dxgi.dll`. Перед установкой сделайте резервную копию игровой DLL.

### Рекомендуемые значения

| VRAM | `VramBudgetMB` | `TargetFPS` | Назначение |
|---:|---:|---:|---|
| 4 ГБ | 2850 | 60 | GTX 1650 / RTX 3050 / RX 6500 XT |
| 6 ГБ | 4600 | 60 | Начальное значение для RTX 3060 / RX 5600 XT |

`VramBudgetMB=0` отключает ограничение бюджета. `TargetFPS=0` отключает frame pacing. `AutoPatchUE5Ini=0` отключает автоматическое обновление CVar.

### Сборка

Откройте **x64 Native Tools Command Prompt Visual Studio 2022 или новее**, перейдите в `src` и выполните:

```bat
build.bat
```

Нужны MSVC C++ x64, Windows SDK, заголовки DXGI/D3D12 и `vswhere.exe`. Результат сборки: `src/dxgi.dll`.

### Проверка эффекта

Используйте CapFrameX или PresentMon. Сравнивайте одинаковое сохранение и маршрут с включённым и отключённым прокси. Основные показатели — 1% low, 0.1% low и пики времени кадра. Frame pacing на 60 FPS предназначен для ровного времени кадра, а не для увеличения физического предела производительности GPU.

## License

MIT. See [LICENSE](LICENSE).
