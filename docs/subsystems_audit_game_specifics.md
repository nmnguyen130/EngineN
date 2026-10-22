# EngineN Subsystems Audit: Phần Thừa (Đặc Thù Game) vs Chuẩn Engine

> **Mục tiêu**: Bóc tách các thành phần mang tư duy/logic riêng của một tựa game (Action / Platformer / Fighting) bị "lọt" vào tầng Core Engine của 6 subsystem: **`time`**, **`platform`**, **`input`**, **`renderer`**, **`assets`**, **`world`**.

---

## 1. Subsystem: `time` (`engine/time/`) — ⚠️ Thừa 1 phần nhỏ

| File & Vị trí | Nội dung đặc thù game | Đánh giá & Khuyến nghị chuẩn Engine |
| :--- | :--- | :--- |
| [TimeTypes.h:23-32](file:///d:/Coding/Game/EngineN/engine/time/TimeTypes.h#L23-L32) | `enum class TimeChannel : u8`: Khai báo sẵn các kênh `Player = 1`, `Enemy = 2`, `Projectile = 3`. | **Thừa ontology game**: Engine 2D tổng quát (giải đố, đua xe, chiến thuật) không có khái niệm "Player/Enemy/Projectile". Nên chuyển thành các kênh tổng quát: `Channel0..7` hoặc `World`, `Sim0`, `Sim1`, `Ui`, `Vfx`, `Custom`. |
| [TimeTypes.h:21](file:///d:/Coding/Game/EngineN/engine/time/TimeTypes.h#L21) | `kGameplayChannelsMask = 0x3FU` (Mặc định cố định các kênh 0..5 là gameplay và bị đóng băng khi pause). | **Thừa giả định**: Việc kênh nào bị pause nên do game logic cấu hình (runtime bitmask) thay vì hardcode trong core engine. |
| [TimeTypes.h:54](file:///d:/Coding/Game/EngineN/engine/time/TimeTypes.h#L54) | `ChannelModSlot::hitstopTicks` | **Thừa thuật ngữ game**: "Hitstop" (khựng hình khi chém trúng) là cơ chế chiến đấu của Action/Fighting game. Tầng engine chỉ cần gọi là `freezeTicks` hoặc `pauseTicks`. |

* **Phần chuẩn Engine**: Bộ tích lũy nhịp 120Hz rational accumulator, VSync harmonic snapping (1440Hz LCM), [BakedTickState](file:///d:/Coding/Game/EngineN/engine/time/TimeTypes.h#L44) 64B L1-resident, [RenderTimeView](file:///d:/Coding/Game/EngineN/engine/time/TimeTypes.h#L62) 48B render interpolation.

---

## 2. Subsystem: `platform` (`engine/platform/`) — ✅ HOÀN TOÀN KHÔNG THỪA

* **Đánh giá**: Subsystem cực kỳ sạch sẽ và đạt chuẩn engine hệ thống 100%.
* **Nội dung đảm nhiệm**:
  * Đóng gói cửa sổ (`Window`, `Extent2i`), High-DPI scaling, Fullscreen/Borderless.
  * Giám sát pin & nhiệt độ thiết bị cầm tay/laptop ([PowerInfo](file:///d:/Coding/Game/EngineN/engine/platform/PlatformTypes.h#L88) 4B).
  * Xử lý chuột, clipboard, đa tay cầm Gamepad SoA (lên tới 4 gamepad).
* **Không chứa bất kỳ dòng mã hay khái niệm game nào.**

---

## 3. Subsystem: `input` (`engine/input/`) — ⚠️ Thừa cơ chế game đối kháng

| File & Vị trí | Nội dung đặc thù game | Đánh giá & Khuyến nghị chuẩn Engine |
| :--- | :--- | :--- |
| [ActionBuffer.h:178-210](file:///d:/Coding/Game/EngineN/engine/input/ActionBuffer.h#L178-L210) | `CheckMotion()`, `ConsumeMotion()`: Tự động quét chuỗi chuyển hướng Numpad ring buffer ({2, 3, 6} Hadouken/QCF, {6, 2, 3} Shoryuken/DP, {6, 5, 6} Dash). | **Thừa cơ chế Fighting Game**: Logic nhận diện "tuyệt chiêu xoay cần" (Motion Specials) là cơ chế đặc thù của game đối kháng (Street Fighter, Guilty Gear). Nên nằm ở module `gameplay` hoặc `FightingInputController`. |
| [ActionBuffer.h:213-240](file:///d:/Coding/Game/EngineN/engine/input/ActionBuffer.h#L213-L240) | `CheckChargeMotion()`: Tự động tính thời gian nạp chiêu (giữ lùi tới [4]6, giữ xuống lên [2]8, coi hướng 1 nạp cả 4 và 2). | **Thừa cơ chế nạp chiêu của game đối kháng** (như Guile Sonic Boom). Không thuộc về Core Engine Input thông thường. |

* **Phần chuẩn Engine**: [InputPipeline](file:///d:/Coding/Game/EngineN/engine/input/InputPipeline.h) chuyển trục analog thành nút ảo qua Schmitt-trigger hysteresis, lọc triệt tiêu hướng SOCD (Neutral, LastInputWins), đệm lệnh sớm (Input Buffer press/release) và tha thứ bấm tổ hợp phím (Chord forgiveness).

---

## 4. Subsystem: `renderer` (`engine/renderer/`) — ⚠️ Thừa 1 thuộc tính Action Game

| File & Vị trí | Nội dung đặc thù game | Đánh giá & Khuyến nghị chuẩn Engine |
| :--- | :--- | :--- |
| [RendererTypes.h:117, 124](file:///d:/Coding/Game/EngineN/engine/renderer/RendererTypes.h#L117) | `QuadCmd::flash8` và hàm `SetHitFlash(f32 ratio)` | **Thừa hiệu ứng chém trúng (Hit-flash)**: Hiệu ứng nhân vật bị chém chớp trắng được nhúng thẳng vào struct lệnh vẽ [QuadCmd](file:///d:/Coding/Game/EngineN/engine/renderer/RendererTypes.h#L99) (80B). Chuẩn engine thường xử lý bằng shader/material hoặc vertex color tint. |

* **Phần chuẩn Engine**: Radix Sort 64-bit khóa ghép state/depth, gom nhóm drawcall tự động ([DrawBatch](file:///d:/Coding/Game/EngineN/engine/renderer/RendererTypes.h#L192)), [PreparedQuad](file:///d:/Coding/Game/EngineN/engine/renderer/RendererTypes.h#L170) 64B L1-resident, [ViewportExtent](file:///d:/Coding/Game/EngineN/engine/renderer/RendererTypes.h#L51) phi phụ thuộc platform, đa pass vẽ, Y-sorting depth.

---

## 5. Subsystem: `assets` (`engine/assets/`) — ✅ HOÀN TOÀN KHÔNG THỪA

* **Đánh giá**: Subsystem quản lý tài nguyên thuần túy, sạch nhất engine.
* **Nội dung đảm nhiệm**:
  * Quản lý bộ nhớ đệm arena 64B và Intrusive Freelist sub-allocator (tái chế slice không phân mảnh).
  * I/O streaming từ file gói `.enpak` qua `SDL_IOStream` (`SDL_SeekIO`/`SDL_ReadIO`), không ép nạp toàn bộ vào RAM.
  * Bộ giải nén LZ4 an toàn bộ nhớ.
  * Quản lý vòng đời theo `ScopeId` cho 4 loại tài nguyên thô: `Texture`, `Atlas`, `Audio PCM`, `Binary Blob`.
* **Không chứa bất kỳ dòng mã hay khái niệm game nào.**

---

## 6. Subsystem: `world` (`engine/world/`) — ⚠️ Thừa ở tầng nhúng System vào Container

| File & Vị trí | Nội dung đặc thù game | Đánh giá & Khuyến nghị chuẩn Engine |
| :--- | :--- | :--- |
| [World.cpp:520-534](file:///d:/Coding/Game/EngineN/engine/world/World.cpp#L520-L534) | `StepAnimationsAndLifetimes()`: Vòng lặp tự động trừ `Lifetime2D`, tự đổi alpha của `Sprite2D.tint.a`, tự động gọi `DeferDestroy()` khi hết giờ. | **Thừa logic System bên trong ECS Container**: Trong chuẩn ECS thuần, `World` chỉ là kho lưu trữ Component và Query. Logic giảm thời gian sống, làm mờ rồi xóa entity phải là một System bên ngoài (`LifetimeSystem`). |
| [World.cpp:536-548](file:///d:/Coding/Game/EngineN/engine/world/World.cpp#L536-L548) | Vòng lặp tự động nhấp nháy đèn `PointLight2D` (`pulsePhase += pulseSpeed * dt`). | **Thừa logic hiệu ứng**: Tương tự như trên, việc nhấp nháy ánh sáng nên là một System riêng thay vì nhúng trực tiếp trong core tick của World container. |
| [WorldTypes.h:206](file:///d:/Coding/Game/EngineN/engine/world/WorldTypes.h#L206) | `CameraRig2D::trauma` (Độ rung chấn màn hình). | **Thừa biến Game-feel**: Thuộc tính rung lắc camera khi trúng đòn/nổ bom thuộc về Gameplay Camera Controller hơn là thuộc tính của một Camera Viewport cơ bản. |

* **Phần chuẩn Engine**: Sparse-Set ECS 192B (3 Cache Lines), [RawComponentPool](file:///d:/Coding/Game/EngineN/engine/world/ComponentPool.h#L11) không con trỏ hàm, cây phân cấp Scene LCRS phi đệ quy 64B ([TransformSolver](file:///d:/Coding/Game/EngineN/engine/world/TransformSolver.h#L11)), FIFO entity slot recycling chống lỗi ABA, Command Tape buffer hoãn lệnh an toàn.
