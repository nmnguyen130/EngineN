# VEIL OF THE SILENT — MASTER GAME DESIGN DOCUMENT & C++23 ARCHITECTURE (2026 EDITION)

**Genre:** High-Speed 2D Action Metroidvania / Elemental & Phenomenon Wave-Combat Platformer  
**Engine & Target Standard:** EngineN Custom Runtime | C++23 (`-std=c++23 -O2 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror`)  
**Simulation Frequency:** Deterministic `120Hz` Fixed Step (`dt = 0.008333s`)  
**Design Benchmarks:** [Hollow Knight](https://store.steampowered.com/app/367520/Hollow_Knight/) (Interconnected World & Corpse-Run Tension), [Nine Sols](https://store.steampowered.com/app/1809540/Nine_Sols/) (High-Precision Deflection & Counter Flow), [TEVI](https://store.steampowered.com/app/2230650/TEVI/) & [BlazBlue Entropy Effect](https://store.steampowered.com/app/2273430/BlazBlue_Entropy_Effect/) (Zero-Recovery Off-Hand Assists, Air-Combo Momentum & Elemental Synergy)

---

## 1. Tầm Nhìn Cốt Lõi & Bản Sắc Độc Bản (Executive Vision & Core Pillars)

**Veil of the Silent** là một tựa game 2D Action Metroidvania tốc độ cao lấy bối cảnh tại **Vaelen** — một vương quốc ngầm khổng lồ được xây dựng trên một lời dối trá thần thánh và vừa sụp đổ bởi chính hệ thống thu hoạch linh hồn của nó.

### 4 Trụ Cột Thiết Kế (Core Pillars)
1. **The Resonance Lantern & 12 Elemental/Phenomenon Waves (Ngọn Đèn Cổ Vật & 12 Sóng Nguyên Tố / Hiện Tượng)**: Thay vì hệ thống phép thuật rời rạc hay các thuật ngữ vật lý khô khan, Kaelen chiến đấu thông qua **Ngọn Đèn Cộng Hưởng (*The Resonance Lantern*)** — cổ vật chứa **12 Tim Đèn (*Wicks / Echoes*)** chia làm **6 Sóng Nguyên Tố (*Elemental Waves*: Hỏa Âm, Địa Âm, Lôi Âm, Băng Âm, Phong Âm, Thủy Âm)** và **6 Hiện Tượng Sóng Đặc Biệt (*Phenomenon Waves*: Quang Âm, Ám Âm, Trọng Âm, Thời Âm, Sinh/Huyết Âm, Phá Âm)**. Mỗi Echo gắn liền với một **Vũ Khí Cận Chiến Độc Bản**, một **Kỹ Năng Di Chuyển/Né (*Traversal Mutation*)**, và một **Kỹ Năng Hỗ Trợ Tức Thời (*Wing Assist*)**.
2. **Amplitude Ascension (Thăng Cấp Biên Độ Vũ Khí — Không Phạt Spam, Chỉ Thưởng Kỹ Năng)**: Không bao giờ phạt người chơi khi dùng một vũ khí ưa thích (`Tier I Fundamental` luôn đạt 100% sức mạnh chuẩn). Khi người chơi tích lũy **Wave Energy** và chuyển Stance qua **Weave-Shift (`L1/RB`)**, năng lượng được truyền sang vũ khí kế tiếp giúp thăng cấp lên **Tier II (*Harmonic*: `+20%` tầm đánh, `+15%` sát thương)** và **Tier III (*Crescendo*: `+40%` tầm đánh, `+35%` sát thương, tự động phóng sóng xung kích miễn phí ở đòn kết thúc chuỗi)**.
3. **Standing-Wave Elemental Interference (Giao Thoa Sóng Nguyên Tố & Hiện Tượng Chủ Động)**: Thay thế hoàn toàn cơ chế đếm nhịp phách 4 gượng gạo. Mỗi đòn đánh hoặc Wing Assist để lại một **Dấu Ấn Sóng (*Standing-Wave Mark*)** tồn tại `3.5s` trên kẻ địch. Khi bồi thêm một Echo thứ hai khác hệ, hai sóng giao thoa ngay lập tức theo nhịp bấm của người chơi — kích nổ **10 Phản Ứng Sóng Nguyên Tố/Hiện Tượng Đặc Biệt** (như *Melt-Shock*, *Piezo-Quake*, *Steam Cavitation*, *Eclipse Singularity*) đồng thời **hồi lập tức `+1` lượt Air-Dash** để giữ chuỗi combo trên không.
4. **Survival in a Kingdom Built on a Lie (Sinh Tồn Giữa Vương Quốc Giả Dối)**: Cốt truyện đen tối, trưởng thành về một kẻ sống sót mang dị tật **Lệch Nhịp (*Off-Beat*)** vô tình nhặt được cổ vật cấm kỵ, từng bước bóc trần tội ác của tầng lớp thống trị đã biến cả vương quốc thành lò mổ linh hồn.

---

## 2. Cốt Truyện, Thế Giới Quan & Nhân Vật (Narrative, Lore & Worldbuilding)

### 2.1. Lời Dối Trá Vĩ Đại Của Vaelen (*The Grand Lie of the Spire*)
Hàng trăm năm trước, **Hội Đồng Chưởng Nhạc (*The High Conductors of the Spire*)** rao giảng rằng mặt đất bên trên đã bị hủy diệt hoàn toàn, và nhân loại chỉ có thể sống sót dưới lòng đất nhờ sự che chở của **Đại Tháp Cộng Hưởng (*The Spire of Unison*)**. Mọi công dân Vaelen từ khi sinh ra đều phải đeo **Vòng Cổ Điều Âm (*Tuning Collar*)**, tự nguyện hiến dâng một phần giọng nói, cảm xúc và "Tần Số Linh Hồn" mỗi ngày để duy trì kết giới bảo vệ vương quốc.

**Sự thật kinh hoàng**: Mặt đất chưa từng bị hủy diệt. Vaelen thực chất là một **Trại Thu Hoạch Tần Số Linh Hồn khổng lồ**. Tầng lớp *High Conductors* trên đỉnh Tháp đã dựng lên lời dối trá tận thế để giam cầm hàng triệu con người dưới lòng đất, rút cạn sinh lực và ký ức của họ qua Vòng Cổ Điều Âm nhằm duy trì sự bất tử vĩnh cửu cho giới quý tộc.

Khi **12 Bậc Thầy Dị Giáo (*The Twelve Apostate Resonators*)** khám phá ra sự thật và bí mật rèn nên **Ngọn Đèn Cộng Hưởng (*The Resonance Lantern*)** để phá hủy lõi Đại Tháp, Hội Đồng Chưởng Nhạc đã kích hoạt giao thức thanh trừng tàn khốc nhất: **Bức Màn Tịch Mịch (*The Veil of Silence*)**. Một xung chấn âm học cực đại quét qua toàn bộ vương quốc — đốt cháy dây thanh quản của mọi cư dân và biến tất cả những ai có tần số đồng bộ với Đại Tháp thành những xác sống điên loạn gọi là **Vỏ Xác Lệch Pha (*Dissonant Husks*)**. 12 Bậc Thầy bị đánh bại, linh hồn bị xé xác và giam cầm làm 12 phong ấn sống giữ cho Đại Tháp tiếp tục hút cạn những gì còn sót lại.

### 2.2. Nhân Vật Chính: Kaelen — Kẻ Lỗi Nhịp (*The Off-Beat Scavenger*)
- **Xuất thân**: Kaelen không phải hiệp sĩ hoàng gia hay đấng cứu thế được tiên tri. Anh chỉ là một kẻ nhặt phế liệu (*Scrap-Scavenger*) vô danh sống lay lắt dưới tầng đáy **Hầm Xỉ Than (*The Ash-Sump Slums*)**.
- **Dị tật "Lỗi Nhịp" (*The Arrhythmic Heart*)**: Kaelen sinh ra với một dị tật hiếm gặp: trái tim và hệ thần kinh của anh đập theo một nhịp lệch pha tự nhiên (*Dead-Frequency Pulse*) khiến **Vòng Cổ Điều Âm không thể đồng bộ hay hút năng lượng từ anh**. Trước thảm họa, xã hội Vaelen coi anh là "phế phẩm lỗi nhịp" và vứt anh xuống đáy vực làm phu đào xác.
- **Kẻ sống sót bất đắc dĩ**: Khi xung chấn *The Veil of Silence* biến toàn bộ những công dân "chuẩn nhịp" thành quái vật *Dissonant Husks*, chính **trái tim lỗi nhịp** đã giúp Kaelen miễn nhiễm hoàn toàn. Anh không mang lý tưởng giải cứu thế giới hào nhoáng — mục tiêu ban đầu của anh đơn giản là **sống sót qua từng giờ**, tìm đường trèo lên đỉnh Đại Tháp để thoát khỏi nấm mồ ngầm đang sụp đổ này.

### 2.3. Cổ Vật Định Mệnh: Ngọn Đèn Cộng Hưởng (*The Resonance Lantern*)
Trong lúc bị một bầy *Husk* truy sát dưới đáy hầm mỏ sập, Kaelen rơi xuống một mật thất bị chôn vùi và vô tình chạm tay vào **Ngọn Đèn Cộng Hưởng (*The Pharos of Twelve Wicks*)** — chiếc đèn cổ bằng đồng đen và thạch anh âm học do 12 Bậc Thầy Dị Giáo để lại.
- Chỉ một kẻ mang **Trái Tim Lệch Nhịp** mới có thể cầm chiếc đèn mà không bị tần số xung đột xé nát.
- Khi máu của Kaelen thấm vào lồng đèn vỡ, **Tim Đèn Thứ Nhất (*Ignis — Nhiệt Âm*)** bừng cháy thành thanh **Searing Katana** trong tay anh.
- Bên trong ngọn đèn là 12 bấc đèn tương ứng với tàn hồn của **12 Echoes**. Để mở khóa cổng thoát lên mặt đất trên đỉnh Spire, Kaelen buộc phải đi qua 12 khu vực tận thế, thu hồi các Echo vào ngọn đèn, và dần phát hiện ra sự thật về lời dối trá đã tạo nên vương quốc này.

### 2.4. Ba Kết Cục Cốt Truyện (Three Endings)
1. **Ending A — *The False Dawn* (Bình Minh Giả Tạo — Normal Ending)**: Kaelen hạ gục Đại Chưởng Nhạc ở đỉnh Tháp nhưng chưa thu thập đủ 12 Tim Đèn. Anh dùng năng lượng ngọn đèn mở cổng thoát thân một mình lên mặt đất, bỏ lại vương quốc Vaelen vĩnh viễn chìm dưới lớp sương mù câm lặng.
2. **Ending B — *The Shattered Spire* (Đập Tan Đại Tháp — Rebellion Ending)**: Kaelen dùng Ngọn Đèn kích nổ Lõi Cộng Hưởng để hủy diệt hoàn toàn hệ thống Vòng Cổ Điều Âm. Những người sống sót được giải phóng khỏi sự kiểm soát, nhưng toàn bộ cấu trúc ngầm Vaelen bắt đầu sụp đổ dữ dội.
3. **Ending C — *Symphony of the Off-Beat* (Bản Giao Hưởng Lệch Nhịp — True Ending)**: Khi thu thập đủ **12 Echoes** và hoàn thành chuỗi nhiệm vụ **Âm Thoa Ký Ức**, Kaelen truyền chính **Nhịp Tim Lệch Pha (*Off-Beat Frequency*)** của mình vào Ngọn Đèn để viết lại tần số của Đại Tháp — biến Tháp từ cỗ máy hút linh hồn thành ngọn hải đăng thanh tẩy, trả lại giọng nói và ý thức tự do cho toàn bộ những cư dân bị biến thành *Husk*.

---

## 3. Hệ Thống 12 Echoes: Sóng Nguyên Tố & Hiện Tượng Sóng (The Twelve Lantern Wicks)

Ngọn Đèn Cộng Hưởng cho phép trang bị **3 Echo cùng lúc vào Triad Deck (`Lead`, `Wing Left`, `Wing Right`)**. Toàn bộ 12 Echo được chia làm **6 Sóng Nguyên Tố (*Elemental Waves*)** và **6 Hiện Tượng Sóng Đặc Biệt (*Phenomenon Waves*)**:

### 3.1. Nhóm I — 6 Sóng Nguyên Tố (Six Elemental Waves)

| ID | Tên Echo | Nguyên Tố & Cơ Chế Sóng | Vũ Khí Lead Stance | Traversal Mutation (Di Chuyển / Né) | Wing Assist (`L2/R2` Không Đổi Vũ Khí) | Hiệu Ứng Đặc Trưng |
|---|---|---|---|---|---|---|
| **0** | **Ignis** | **Hỏa Âm — Thermal Acoustics (Sóng Nhiệt Âm)** | **Searing Katana** (Trường Đao Nhiệt Âm) | **Blaze Jet-Dash**: Lướt phản lực nhiệt tốc độ cao (`24 m/s`), đốt cháy gai độc/tường băng và để lại vệt lửa | **Thermo-Acoustic Flare**: Phóng vòng sóng nhiệt xung quanh Kaelen, đẩy lùi và đốt cháy quái áp sát | Chém ma sát cao tần tạo nhiệt độ cực đại, thiêu đốt máu (`Ignite`) và **nung chảy `-25%` Giáp** của kẻ địch |
| **1** | **Terra** | **Địa Âm — Seismic Infrasound (Hạ Âm Địa Chấn)** | **Executioner War-Axe** (Chiến Rìu Đoạt Mệnh) | **Tectonic Quake-Plunge**: Bổ nhào địa chấn tốc độ `26 m/s` đập nát sàn đá nứt và hất tung quái | **Faultline Bulwark**: Dựng vách đá sóng dừng chặn đạn và nổ gai địa chấn phản sát thương | Đòn bổ nặng có **Guard-Point (Chống đòn)** ở 4 frame đầu; tích lũy rung chấn cực lớn phá vỡ mọi loại khiên |
| **2** | **Fulgur** | **Lôi Âm — Piezoelectric Wave (Sóng Áp Điện)** | **Volt Twin-Daggers** (Song Đoản Đao Áp Điện) | **Piezo Blink-Step**: Dịch chuyển tia chớp tức thời (`0-tick`, `4.5m`) xuyên qua song sắt, tia laser và thân Boss | **Arc Lattice**: Phóng lưới điện áp cao tần khóa chân mục tiêu và truyền điện giữa các quái | Tốc độ đâm nhanh nhất game (`1.50x`); sát thương áp điện tăng tỷ lệ thuận với vận tốc di chuyển của Kaelen |
| **3** | **Glacies** | **Băng Âm — Cryo-Cymatics (Sóng Kết Tinh Lạnh)** | **Cryo Halberd** (Trường Kích Kết Tinh) | **Cymatic Ice-Vault**: Đóng băng hơi nước dưới chân tạo một **Bệ Tinh Thể Cymatics** giữa không trung và hồi `+1` Air-Jump | **Absolute-Zero Spire**: Triệu hồi cọc băng nhọn đâm xuyên từ mặt đất, làm chậm `50%` tốc độ địch | Tầm đánh xa (`3.10m`); tạo hoa văn băng hình học Cymatics làm giòn cấu trúc giáp của Boss |
| **4** | **Ventus** | **Phong Âm — Aero-Acoustic Vortex (Sóng Xoáy Khí Động)** | **Gale Twinblade** (Song Đao Khí Xoáy) | **Slipstream Updraft**: Phóng vọt thẳng đứng theo cột gió xoáy (`+16.5 m/s`) kèm trạng thái lượn áp suất thấp | **Cyclone Funnel**: Tạo lốc xoáy chân không gom toàn bộ quái nhỏ về một điểm trước mặt | Cho phép Kaelen **vừa chạy/bay 100% tốc độ vừa xoay đao liên hoàn**, tạo lực nâng giữ độ cao trên không |
| **5** | **Aqua** | **Thủy Âm — Hydro-Cavitation (Thủy Kích Chân Không)** | **Tidal Whip-Sword** (Kiếm Roi Thủy Kích) | **Cavitation Wire-Swing**: Quăng roi nước áp suất cao bám vào móc treo để đu lắc quán tính lớn (`27 m/s`) | **Implosion Surge**: Kích nổ toàn bộ bọt khí chân không trên người địch, **hồi `+8 HP`** cho Kaelen | Tầm quất cực rộng (`3.85m`); mỗi nhát quất cấy bọt khí áp suất cao nổ sập nội thương và hồi sinh lực |

### 3.2. Nhóm II — 6 Hiện Tượng Sóng Đặc Biệt (Six Exotic Wave Phenomena)

| ID | Tên Echo | Hiện Tượng Vật Lý Sóng | Vũ Khí Lead Stance | Traversal Mutation (Di Chuyển / Né) | Wing Assist (`L2/R2` Không Đổi Vũ Khí) | Hiệu Ứng Đặc Trưng |
|---|---|---|---|---|---|---|
| **6** | **Lux** | **Quang Âm — Sonoluminescence (Phát Quang Âm Học)** | **Radiant Claymore** (Đại Kiếm Quang Âm) | **Photon Ray-Grapple**: Hóa thành tia sáng âm học bắn thẳng tới mốc neo hoặc mục tiêu ở tốc độ `32 m/s` | **Prism Refraction Beam**: Bắn chùm tia khúc xạ xuyên táo toàn bộ kẻ địch trên đường thẳng | Nén sóng âm tới ngưỡng phát sáng như sao, soi sáng vùng tối *Dead-Air* và tăng mạnh sát thương chuẩn xác |
| **7** | **Umbra** | **Ám Âm — Anechoic Null-Wave (Sóng Triệt Tiêu / Hố Đen Âm)** | **Abyssal Great-Scythe** (Đại Liềm Hư Không) | **Null Phase-Step**: Hóa thành bóng tối tĩnh lặng lướt xuyên vật chất và mọi đòn tấn công với `12-tick` bất tử | **Anechoic Ward**: Mở vòm chân không hấp thụ và **xóa sạch toàn bộ đạn** của kẻ địch xung quanh | Triệt tiêu pha $180^\circ$ làm câm lặng kỹ năng của quái tinh anh; mỗi nhát chém để lại bóng âm lặp lại sát thương |
| **8** | **Gravitas** | **Trọng Âm — Acoustic Levitation (Sóng Trọng Trường)** | **Singularity Great-Maul** (Đại Chùy Trọng Lực) | **Graviton Slingshot**: Đảo ngược trọng trường cục bộ để bật phóng vọt qua các vực thẳm rộng | **Heavy-G Well**: Tạo trường trọng lực gấp 3 lần (`3G`) kéo sập toàn bộ quái bay xuống mặt đất | Vũ khí có sát thương và chỉ số Break đơn mục tiêu cao nhất (`52 Dmg / 60 Break`); đè bẹp mọi mục tiêu hạng nặng |
| **9** | **Tempus** | **Thời Âm — Temporal Delay-Line (Sóng Trễ Thời Gian)** | **Chronos Odachi** (Trường Kiếm Thời Không) | **Temporal Anchor Recall**: Bấm lần 1 đặt Bóng Thời Gian tại vị trí hiện tại; bấm lần 2 **giật ngược thời gian** quay về vị trí bóng kèm nhát chém | **Delay-Line Dome**: Tạo vòm trễ pha làm chậm kẻ địch và đạn trong vùng xuống còn `35%` tốc độ | Mọi nhát chém đều được "ghi âm" vào không gian và **tự động phát nổ lặp lại thêm `45%` sát thương sau `12 ticks`** |
| **10** | **Aether** | **Sinh Âm — Bio-Sympathetic Pulse (Sóng Cộng Hưởng Huyết)** | **Crimson Rapier** (Kiếm Liễu Cộng Hưởng Huyết) | **Pulse Tether-Swap**: Hoán đổi vị trí tức thời trong không gian với kẻ địch đang mang Dấu Ấn Sóng | **Sympathetic Thread**: Phóng tơ âm học nối liền nhịp tim của nhiều kẻ địch với nhau | Đâm cực nhanh và chuẩn xác; **truyền `45%` sát thương từ mục tiêu chính sang toàn bộ kẻ địch đang bị liên kết** |
| **11** | **Stridor** | **Phá Âm — Mach Cone Shockwave (Nón Xung Kích Siêu Thanh)** | **Sonic Pile-Gauntlets** (Thiết Quyền Siêu Thanh) | **Mach Recoil Pogo**: Bắn xung kích siêu thanh xuống dưới chân để bật nảy Pogo cao `18 m/s` và **hồi lại Air-Dash** | **Concussive Boom**: Vỗ tay tạo sóng xung kích siêu thanh ép quái thường gục xuống *Submissive* ngay lập tức | Đánh cận chiến tốc độ cực cao; **mỗi đòn đấm thứ 3 vượt tường âm thanh Mach 1**, nổ nón xung kích tăng `1.45x` sát thương và `+65 Break` |

---

## 4. Hệ Thống Chiến Đấu 4 Trụ Cột Mới (Redesigned High-Speed Combat System)

### 4.1. Trụ Cột 1: Amplitude Ascension (Thăng Cấp Biên Độ Vũ Khí — Thay Thế Hoàn Toàn Phạt Combo)
- **Triết lý thiết kế**: Người chơi không bao giờ bị phạt giảm sát thương nếu muốn dùng mãi một thanh kiếm yêu thích (như *Searing Katana* của Ignis hay *Executioner War-Axe* của Terra). Thay vào đó, việc luân chuyển vũ khí qua **Weave-Shift (`L1/RB`)** sẽ được **thưởng bằng sự tiến hóa trực quan và sức mạnh bùng nổ**:
  - **Tier I — *Fundamental* (Cơ Bản)**: Vũ khí ở kích thước chuẩn (`1.00x` tầm đánh, `1.00x` sát thương). Mỗi đòn đánh trúng tích lũy `+14..24 Wave Energy`.
  - **Tier II — *Harmonic* (Cộng Hưởng)**: Khi có ít nhất `30 Wave Energy`, bấm **Weave-Shift (`L1/RB`)** sẽ truyền năng lượng trong Ngọn Đèn sang vũ khí kế tiếp, nâng cấp lên **Tier II**: lưỡi vũ khí rực sáng sóng nguyên tố, tăng **`+20%` tầm đánh (`1.20x`)** và **`+15%` sát thương (`1.15x`)**.
  - **Tier III — *Crescendo* (Cực Đại)**: Tiếp tục tích lũy `30 Wave Energy` và **Weave-Shift** lần nữa sẽ nâng vũ khí lên **Tier III**: tăng **`+40%` tầm đánh (`1.40x`)**, **`+35%` sát thương (`1.35x`)**, và mọi đòn kết thúc chuỗi combo (`LightChain3` / `AirChain2`) sẽ tự động phóng thêm một luồng **Crescendo Wave-Burst** diện rộng miễn phí kèm `+20 Harmony`.

### 4.2. Trụ Cột 2: Standing-Wave Elemental & Phenomenon Interference (Ma Trận Giao Thoa Sóng Nguyên Tố)
Thay vì bắt người chơi canh nhịp đồng hồ 120 BPM cứng nhắc, hệ thống **Giao Thoa Sóng Dừng** hoạt động hoàn toàn theo nhịp độ điều khiển tự nhiên của người chơi:
1. **Gắn Dấu Ấn Sóng (*Imprint Wave Mark*)**: Đòn đánh của Lead Echo hoặc Wing Assist gắn dấu sóng nguyên tố/hiện tượng của Echo đó lên kẻ địch trong `3.5s` (`420 ticks`).
2. **Kích Nổ Giao Thoa (*Detonate Interference*)**: Chỉ cần bồi thêm một đòn từ Echo khác (gọi nhanh qua **Wing Assist `L2/R2`** ngay trong lúc đang chém mà không cần ngắt đòn, hoặc dùng **Weave-Shift `L1/RB`**), hai sóng giao thoa tức thì:
   - **Phần thưởng cơ động chung**: Mọi phản ứng giao thoa đều **hồi ngay lập tức `+1` lượt Air-Dash** và cộng **`+18..28 Harmony`**, cho phép người chơi lướt và nối combo vô tận trên không trung.
   - **10 Phản Ứng Sóng Nguyên Tố & Hiện Tượng Đặc Biệt (Signature Wave Reactions)**:

| Tên Phản Ứng Giao Thoa | Tổ Hợp Echo Kích Hoạt | Hiệu Ứng Vật Lý & Chiến Thuật Trong Thực Chiến | Chỉ Số Thưởng Thêm |
|---|---|---|---|
| **1. Melt-Shock (Sốc Nhiệt Toác Giáp)** | `Ignis` (Nhiệt Âm) + `Glacies` (Băng Âm) | Thay đổi nhiệt độ đột ngột làm nứt vỡ cấu trúc tinh thể, **xóa `40%` Giáp** của Boss trong `6s` | `+165 Dmg`, `+110 Break`, `+22 Harmony` |
| **2. Piezo-Quake (Địa Lôi Áp Điện)** | `Terra` (Địa Âm) + `Fulgur` (Lôi Âm) | Sóng hạ âm nén chặt thạch anh dưới đất tạo hiệu ứng Áp Điện, phóng cột sét làm **Choáng (`Stun 0.75s`)** | `+135 Dmg`, `+145 Break`, `+20 Harmony` |
| **3. Steam Cavitation (Thủy Nhiệt Siêu Áp)** | `Ignis` (Nhiệt Âm) + `Aqua` (Thủy Âm) | Nhiệt âm đun sôi bọt khí chân không thành vụ nổ hơi nước siêu áp đẩy lùi và đốt lan | `+150 Dmg`, `+105 Break`, `+22 Harmony` |
| **4. Superconductor Gale (Bão Từ Siêu Dẫn)** | `Fulgur` (Lôi Âm) + `Ventus` (Phong Âm) | Lốc xoáy khí động dẫn truyền lưới điện cao tần hút gom quái vào tâm và giật liên hoàn | `+130 Dmg`, `+115 Break`, `+24 Harmony` |
| **5. Pyro-Cyclone (Bão Lửa Khí Động)** | `Ignis` (Nhiệt Âm) + `Ventus` (Phong Âm) | Khuếch đại sóng nhiệt thành vòi rồng lửa di động, **thiêu rụi toàn bộ đạn địch** bay ngang | `+155 Dmg`, `+95 Break`, `+20 Harmony` |
| **6. Tectonic Shatter (Băng Chấn Nghiền Xương)** | `Glacies` (Băng Âm) + `Terra` hoặc `Stridor` | Đóng băng mục tiêu rồi đập nát bằng sóng địa chấn/xung kích siêu thanh, cực mạnh để phá thế thủ Boss | `+175 Dmg`, **`+180 Break`**, `+25 Harmony` |
| **7. Eclipse Singularity (Nhật Thực Kỳ Dị)** | `Lux` (Quang Âm) + `Umbra` (Ám Âm) | Giao thoa giữa Phát Quang và Hố Đen Âm Học tạo vụ nổ bức xạ hút gom toàn màn hình | **`+190 Dmg`**, `+150 Break`, `+28 Harmony` |
| **8. Graviton Quake (Trọng Trường Địa Chấn)** | `Gravitas` (Trọng Âm) + `Terra` (Địa Âm) | Nhân ba trọng lực kết hợp hạ âm kéo sập mọi mục tiêu trên không đập thẳng xuống đất | `+160 Dmg`, `+170 Break`, `+24 Harmony` |
| **9. Chrono-Overdrive (Đứt Gãy Thời Không Mach 3)** | `Tempus` (Thời Âm) + `Stridor` (Phá Âm) | Nén các nhát chém trễ thời gian rồi kích nổ đồng loạt ở vận tốc siêu thanh Mach 3 | `+185 Dmg`, `+140 Break`, `+26 Harmony` |
| **10. Sanguine Boil (Huyết Cộng Hưởng Sôi Trào)** | `Aether` (Sinh Âm) + `Ignis` hoặc `Aqua` | Đun sôi hoặc kích nổ bọt khí bên trong mạch máu của toàn bộ kẻ địch đang bị nối dây liên kết | `+160 Dmg`, `+110 Break`, `+25 Harmony` |

### 4.3. Trụ Cột 3: Phase-Inversion Counter (Parry Đảo Pha Khắc Hệ)
- Boss và quái tinh anh sử dụng hệ thống cảnh báo kép **Glint & Tone (Ánh Sáng & Âm Hiệu)**:
  - **Đòn Ánh Vàng (*Gold Harmonic Wave*)**: Có thể Parry thông thường ở bất kỳ Stance nào. Tuy nhiên, nếu người chơi chuyển sang **Echo Khắc Hệ Ngược Pha** (ví dụ: dùng `Glacies` đỡ đòn nhiệt của Boss `Ignis`, hoặc dùng `Fulgur` đỡ đòn địa chấn của `Terra`), Kaelen tung đòn **Phase-Inversion Counter**:
    - Phản ngược toàn bộ sóng năng lượng vào Boss gây **350 Break Damage**.
    - Hồi ngay lập tức **`+50 Harmony`** và thăng cấp thẳng vũ khí lên **Amplitude Tier III (*Crescendo*)**.
    - Ép Boss từ trạng thái hung hãn (*Dominant*) quỳ sụp xuống trạng thái **Submissive** (nhận `1.50x` sát thương trong `3.0s`).
  - **Đòn Ánh Đỏ (*Crimson Overload Wave*)**: Không thể đỡ hay Parry; bắt buộc người chơi phải sử dụng **Traversal Mutation** của Ngọn Đèn (như *Piezo Blink-Step*, *Null Phase-Step*, *Temporal Anchor Recall*, hoặc *Mach Recoil Pogo*) để né và phản công từ góc chết.

### 4.4. Trụ Cột 4: Polyphonic Unison & 4 Hợp Âm Tối Thượng (Sacred Chords)
Khi thanh **Harmony Gauge** đạt `50/100`, người chơi có thể kích hoạt **Dual Unison**; khi đạt `100/100`, người chơi có thể kích hoạt **Triad Unison**. Đặc biệt, nếu 3 Tim Đèn trong Ngọn Đèn khớp với **1 trong 4 bộ Sacred Chords**, chiêu cuối biến hình thành tuyệt kỹ cấp độ thần thoại:
1. **Elemental Cataclysm (`Ignis + Terra + Fulgur`)**: Cơn đại hồng thủy Nhiệt - Địa - Lôi làm nứt toác mặt đất và giáng bão sét nhiệt hạch toàn màn hình.
2. **Cryo-Tempest Protocol (`Glacies + Ventus + Aqua`)**: Đóng băng không gian trong lốc xoáy thủy kích rồi kích nổ hàng ngàn mảnh băng siêu thanh.
3. **Eclipse of Eternity (`Lux + Umbra + Tempus`)**: Ngưng đọng thời gian toàn màn hình trong nhật thực âm học rồi chém 12 nhát kiếm ánh sáng và bóng tối đồng loạt.
4. **Singularity Overdrive (`Gravitas + Aether + Stridor`)**: Nối dây sinh mệnh toàn bộ kẻ địch vào một hố đen trọng trường rồi đấm xuyên tâm ở vận tốc Mach 5.

---

## 5. Toàn Bộ Cơ Chế Metroidvania Ngoài Combat (Complete Non-Combat & World Systems)

1. **Tuning Resonators (Đài Điều Chỉnh Ngọn Đèn — Checkpoints)**:
   - Hồi đầy HP, nạp đầy bình Harmony, lưu tự động (`SaveGameData`).
   - Là nơi Kaelen mở lồng **Ngọn Đèn Cộng Hưởng** để thay đổi **3 Tim Đèn trong Triad Deck** từ 12 Echo đã thu thập và gắn các mảnh cổ ngữ **Resonance Sigils**.
2. **Lost Wick Husk (Cơ Chế Rớt Tim Đèn Khi Tử Nạn — Corpse Run)**:
   - Khi Kaelen gục ngã, Ngọn Đèn bị nứt khiến một phần ánh sáng rơi lại hiện trường dưới dạng **Bóng Ma Tim Đèn (*Lost Wick Husk*)**, giữ toàn bộ **Tuning Shards** (tiền tệ) và khóa `25%` thanh HP tối đa.
   - Người chơi phải quay lại vị trí tử nạn và chém vỡ *Lost Wick Husk* để thu hồi toàn bộ Shards, mở khóa lại `100%` HP và nạp đầy thanh Harmony.
3. **Acoustic Fog of War, Spatial Clues & Frequency Gates**:
   - Bản đồ mở dần theo bước chân khám phá; hỗ trợ cắm **5 loại Map Pins** (*FrequencyGate*, *UnreachedLedge*, *LoreTablet*, *BossArena*, *TuningForkQuest*).
   - **Cửa Tần Số (*Frequency Gates*)**: Mỗi cánh cửa cổ đại chỉ mở khi Kaelen thắp đúng **Tim Đèn (Lead Echo)** tương ứng và dùng kỹ năng đặc trưng (ví dụ: dùng *Searing Katana* của `Ignis` nung chảy chốt băng vĩnh cửu, dùng *Photon Ray-Grapple* của `Lux` khúc xạ qua gương thạch anh, hoặc dùng *Tectonic Quake-Plunge* của `Terra` đập sập sàn đá cổ).

---

## 6. Kiến Trúc Phần Mềm C++23 Clean Trên EngineN (Software Architecture)

Mã nguồn được thiết kế phẳng, sạch tuyệt đối theo tiêu chuẩn `user:enginen-architecture` và `user:cpp23-code-style`:
- `include/veil/Core/Types.hpp`: Định nghĩa toàn bộ kiểu dữ liệu cơ sở, `EchoId` (`0..11`), `WaveCategory`, `WavePolarity`, `WeaponArchetype`, `TraversalType`, `AmplitudeTier`, `WaveReactionId`, và `SacredChordId`.
- `include/veil/Core/EchoPantheon.hpp`: Bảng hằng số `constexpr kEchoPantheon` cho 12 Echoes, bảng `kWaveReactionTable` cho 10 Phản Ứng Giao Thoa Sóng, và hàm `resolveWaveReaction` / `resolveSacredChord` chạy trong `O(1)`.
- `include/veil/Core/Components.hpp`: Các thành phần dữ liệu thuần (`ResonanceLanternComponent`, `StandingWaveComponent`, `KinematicsComponent`, `CombatStatsComponent`, `ActionStateComponent`, `BossConductorComponent`, `WorldProgressComponent`).
- `include/veil/Movement/TraversalSystem.hpp`: Bộ giải vật lý `120Hz` xử lý Coyote Time, Jump Buffer và toàn bộ **12 Traversal Mutations** của Ngọn Đèn.
- `include/veil/Combat/ActionTable.hpp` & `CombatSystem.hpp`: Xử lý trọn vẹn **Amplitude Ascension (Tier I -> II -> III)**, **Standing-Wave Elemental Interference**, **Phase-Inversion Counter**, và **Sacred Chords** trong một hàm `CombatSystem::step` duy nhất mỗi tick.
- `include/veil/AI/BossSystem.hpp` & `include/veil/World/WorldSystem.hpp`: Quản lý cảnh báo *Gold Harmonic / Crimson Overload*, chuyển pha nhạc nền ở `50% HP`, Đài nghỉ *Tuning Resonator*, và cơ chế *Lost Wick Husk*.
