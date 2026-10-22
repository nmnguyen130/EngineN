# VEIL OF THE SILENT
## Master Game Design Document — Bản chuẩn thiết kế v0.2

**Trạng thái tài liệu:** Bản thiết kế đã rà soát. Mục 1–4 là phạm vi được chốt cho giai đoạn hiện tại. Các thông số ghi là *prototype baseline* là giá trị khởi điểm để triển khai và playtest, chưa phải cân bằng cuối cùng.

| Hạng mục | Quyết định hiện tại |
|---|---|
| Thể loại | 2D Action Metroidvania / Precision Combat Platformer |
| Thế giới | Vaelen — vương quốc ngầm được duy trì bằng giọng nói, ký ức và sinh lực bị đánh cắp |
| Nhân vật chính | Kaelen — Kẻ Nhặt Phế Liệu Lệch Nhịp (*The Off-Beat Scavenger*) |
| Cổ vật trung tâm | Ngọn Đèn Cộng Hưởng (*The Resonance Lantern*) |
| Bản sắc cốt lõi | Một vũ khí; 12 chế độ cộng hưởng; tương tác Dấu Ấn có chủ đích; khám phá qua việc phát hiện quy luật |
| Runtime mục tiêu | EngineN custom runtime, C++23 |
| Nhịp mô phỏng | Fixed step 120 Hz; `dt = 1/120 s` (xấp xỉ 8,333 ms) |
| Phạm vi combat hiện tại | Một moveset vũ khí dùng chung, Triad Deck 3 Echo, hệ thống phản ứng dấu ấn, parry, Break, Harmony |
| Chủ động hoãn | 12 vũ khí/moveset riêng, 12 bộ Wing Assist, 12 kỹ năng traversal riêng, nâng cấp tầm đánh/sát thương theo Tier vũ khí |

**Tài liệu tham chiếu:** *Hollow Knight* về thế giới liên kết và khám phá; *Nine Sols* về parry có tín hiệu rõ ràng; *TEVI* và *BlazBlue Entropy Effect* về các ý tưởng combat hành động. Đây là tài liệu tham khảo, không phải danh sách tính năng cần sao chép. Mục tiêu là tạo bản sắc riêng thông qua quy luật cộng hưởng của Vaelen, cốt truyện, khám phá và các quyết định trong combat.

---

# 1. Tầm nhìn cốt lõi & bản sắc độc bản

## 1.1 High Concept

**Veil of the Silent** là game 2D Action Metroidvania lấy bối cảnh Vaelen, một vương quốc ngầm được duy trì bằng giọng nói, ký ức và sinh lực bị đánh cắp từ chính người dân.

Kaelen, một kẻ bị xã hội ruồng bỏ vì nhịp tim không thể đồng bộ với hệ thống cộng hưởng cai trị, sống sót sau một thảm họa và phát hiện Ngọn Đèn Cộng Hưởng — cổ vật cấm chứa mười hai dấu ấn cộng hưởng đã mất.

Thông qua các Echo, Kaelen học cách nhận diện và phá vỡ những quy luật ẩn điều khiển sinh vật, máy móc và thế giới. Khám phá hé lộ những gì Vaelen đã xóa bỏ. Combat yêu cầu người chơi đọc tín hiệu và phá vỡ các quy luật tấn công. Mỗi Echo thu hồi được làm sáng tỏ thêm về vương quốc và mở rộng những tổ hợp có thể dùng trong chiến đấu.

**Luận điểm thiết kế cốt lõi:** Người chơi không đơn thuần áp đảo một thế giới được xây dựng trên sự kiểm soát. Họ học quy luật của nó, phát hiện những mâu thuẫn bên trong và khai thác thứ hệ thống không thể giải thích hoàn toàn: sự Lệch Nhịp (*Off-Beat*).

## 1.2 Bốn trụ cột thiết kế

### Trụ cột 1 — Ngọn Đèn Lệch Nhịp (*The Off-Beat Lantern*)

Ngọn Đèn Cộng Hưởng liên kết combat, khám phá và cốt truyện. Nó chứa 12 Echo, chia thành 6 Sóng Nguyên Tố và 6 Hiện Tượng Sóng.

Trong phạm vi triển khai hiện tại, Kaelen dùng **một vũ khí chung với một moveset cốt lõi**. Đổi Echo chỉ đổi bảng màu/VFX cộng hưởng của vũ khí và `EchoId` dùng để áp dụng Dấu Ấn Cộng Hưởng. Đổi Echo **không** đổi loại vũ khí, sát thương cơ bản, tầm đánh, thời gian khởi động/thu hồi đòn, kích thước hitbox hoặc animation tấn công.

Vũ khí riêng, bộ Wing Assist và traversal mutation riêng cho từng Echo được hoãn cho tới khi hệ thống chung đã được kiểm chứng.

### Trụ cột 2 — Combat có chủ đích và giàu biểu đạt

Combat có ba kết quả riêng biệt:

- **HP Damage:** trừ máu của mục tiêu.
- **Break:** làm đầy thanh thế thủ/tư thế (*posture*) và có thể mở cửa sổ trừng phạt.
- **Khống chế (Crowd Control — CC):** thay đổi khả năng di chuyển, tấn công hoặc vị trí của mục tiêu.

Ba đại lượng này độc lập. Đòn gây sát thương cao không tự động gây Break cao hoặc khống chế mạnh. Phản ứng Echo là lựa chọn chiến thuật bổ sung, không phải chuỗi combo bắt buộc để duy trì DPS cơ bản.

### Trụ cột 3 — Khám phá bằng phát hiện quy luật

Thiết kế đường đi cần thưởng cho việc quan sát, thử nghiệm và mở khóa năng lực di chuyển lâu dài. Khi quay lại khu vực cũ, người chơi nên phát hiện tuyến đường, lối tắt hoặc cách hiểu mới, thay vì chỉ thấy thêm một cánh cửa có màu khác.

Hệ thống Echo đổi màu hiện tại không tự cấp mười hai năng lực traversal riêng. Các cổng tiến trình trong bản hiện tại phải dựa trên năng lực thế giới đã mở khóa vĩnh viễn, vật phẩm, điều kiện tuyến đường hoặc cờ trạng thái câu đố; không được đòi hỏi một Echo cụ thể đang được trang bị trừ khi ngoại lệ đó được ghi rõ trong đặc tả.

### Trụ cột 4 — Im lặng, ký ức và hệ quả

Im lặng là một hình thức kiểm soát: xóa ký ức, đàn áp bản ngã và che giấu sự thật. Thế giới, cư dân, kẻ thù và kiến trúc phải thể hiện chủ đề này qua gameplay lẫn kể chuyện môi trường.

Echo thu hồi được không chỉ là vật phẩm tăng sức mạnh. Chúng giữ lại những dấu ấn không hoàn chỉnh của người tạo ra chúng, đồng thời cung cấp bằng chứng rằng lịch sử chính thức của Vaelen là giả dối.

## 1.3 Vòng lặp gameplay cốt lõi

**Khám phá → Quan sát → Diễn giải → Mở khóa hoặc cấu hình lại → Vượt qua giới hạn mới → Phát hiện bằng chứng → Nhìn lại thế giới theo cách khác.**

Combat, traversal và cốt truyện phải củng cố cùng vòng lặp này. Một tính năng thành công khi nó tạo ra quyết định mới cho người chơi, không chỉ làm tăng con số hoặc hiệu ứng hình ảnh.

## 1.4 Ưu tiên thiết kế

1. Quyết định gameplay khác biệt quan trọng hơn số lượng tính năng.
2. Combat dễ đọc quan trọng hơn hiệu ứng và con số quá tải.
3. Quy luật thế giới nhất quán quan trọng hơn cổng khóa tùy tiện.
4. Phần thưởng khám phá có ý nghĩa quan trọng hơn quy mô bản đồ đơn thuần.
5. Phong cách chơi ưa thích vẫn phải hiệu quả; không ép người chơi tối ưu một chuỗi combo duy nhất.
6. Hoàn thiện một vertical slice nhỏ trước khi cam kết toàn bộ năng lực tương lai của 12 Echo.

---

# 2. Cốt truyện, thế giới quan và nhân vật

> **Trạng thái thiết kế:** Đây là bản cốt truyện đề xuất hiện tại. Bản này làm rõ các lỗ hổng nhân quả trong tài liệu trước, đồng thời giữ lại tiền đề, tên riêng và cấu trúc ba kết cục ban đầu.

## 2.1 Vaelen — Vương quốc xây trên lời dối trá

Qua nhiều thế hệ, người dân Vaelen sống dưới lòng đất và tin rằng mặt đất đã bị hủy diệt từ lâu. Hội Đồng Chưởng Nhạc (*The High Conductors of the Spire*) tuyên bố rằng chỉ có cộng hưởng bảo vệ của Đại Tháp Cộng Hưởng (*The Spire of Unison*) mới giúp nhân loại sống sót.

Mọi công dân đều được đeo **Vòng Cổ Điều Âm (*Tuning Collar*)** từ khi sinh ra. Theo lời tuyên truyền, vòng cổ bảo vệ người đeo khỏi tần số nguy hiểm và góp phần duy trì sự sống của vương quốc. Trên thực tế, chúng kết nối mỗi người vào một mạng lưới thu hoạch khổng lồ.

Thông qua các nghi thức cộng hưởng hằng ngày, Đại Tháp từ từ thu hoạch những mảnh giọng nói, ký ức, cảm xúc và tần số sinh lực của công dân. Năng lượng thu được duy trì tuổi thọ bất thường của tầng lớp Chưởng Nhạc. Quá trình này diễn ra chậm có chủ đích: người dân vẫn lao động, hình thành quan hệ và sống cuộc đời thường nhật trong khi dần mất đi những phần bản ngã.

Mặt đất chưa từng bị hủy diệt. Lời tuyên truyền về tận thế là công cụ chính trị nhằm ngăn việc bỏ trốn và khám phá sự thật. Bản đồ bị xóa hoặc làm giả, tư liệu lịch sử bị kiểm duyệt, còn mọi nỗ lực tiếp cận thế giới bên trên đều bị coi là mối đe dọa sống còn đối với Vaelen.

## 2.2 Bức Màn Tịch Mịch và 12 Bậc Thầy Dị Giáo

Mười hai Bậc Thầy Cộng Hưởng (*The Twelve Apostate Resonators*) phát hiện Đại Tháp không phải nơi trú ẩn mà là một cỗ máy thu hoạch. Họ tạo ra **Ngọn Đèn Cộng Hưởng**, một cổ vật có thể lưu giữ dấu ấn cộng hưởng riêng biệt và phá vỡ cơ chế đồng bộ tập trung của Đại Tháp.

Hội Đồng phát hiện cuộc nổi dậy và kích hoạt **Bức Màn Tịch Mịch (*The Veil of Silence*)**, một giao thức khẩn cấp vốn được thiết kế để đàn áp phản kháng và buộc dân chúng trở lại trạng thái đồng bộ.

Mười hai Bậc Thầy phá hoại giao thức nhằm ngăn việc hệ thống tái lập hoàn toàn. Sự can thiệp của họ gây ra một sự cố phản hồi cộng hưởng vượt tầm kiểm soát. Bức Màn lan khắp Vaelen. Nhiều công dân mắc kẹt trong mạng đồng bộ đã hỏng; bản năng và ký ức còn sót lại của họ bị biến dạng thành hành vi thù địch của **Vỏ Xác Lệch Pha (*Dissonant Husks*)**.

Mười hai Bậc Thầy bị đánh bại. **Ý thức nguyên bản** của họ bị giam trong mười hai phong ấn sống giúp ổn định Đại Tháp. Trong khi đó, **dấu ấn cộng hưởng** của họ bị tách khỏi ý thức nguyên bản và phân tán tại các vùng bị phong tỏa. Đây là hai thứ khác nhau: thu hồi một Echo không tự động giải phóng hoặc khôi phục con người nguyên bản.

Thảm họa không phải một cuộc cách mạng thành công, cũng không phải kế hoạch tận diệt đã được tính toán hoàn hảo. Nó là hậu quả của việc chế độ cố duy trì quyền kiểm soát bằng mọi giá, cộng với nỗ lực tuyệt vọng của phe nổi dậy nhằm ngăn chặn hệ thống.

## 2.3 Kaelen — Kẻ Nhặt Phế Liệu Lệch Nhịp

Kaelen là kẻ nhặt phế liệu sống ở **Hầm Xỉ Than (*The Ash-Sump Slums*)**. Anh không có dòng máu hoàng gia, không được tiên tri lựa chọn, không được huấn luyện quân sự và không có quyền thừa kế Đại Tháp.

Anh sinh ra với nhịp tim và hệ thần kinh bất thường. Tần số tự nhiên của anh không thể được Vòng Cổ Điều Âm đồng bộ hoàn toàn, khiến Đại Tháp khó theo dõi, kiểm soát hoặc thu hoạch anh bằng quy trình thông thường.

Điều này không khiến anh bất khả xâm phạm. Anh vẫn có thể bị thương, bị áp đảo bởi hiện tượng môi trường hoặc chịu hậu quả vật lý từ cộng hưởng hỏng. Lợi thế của anh có phạm vi cụ thể: Đại Tháp không dễ dàng ghi đè bản ngã anh bằng cơ chế đồng bộ thông thường.

Trước Bức Màn Tịch Mịch, sự khác biệt khiến anh bị coi là đồ bỏ. Anh sống sót bằng cách tránh bị chú ý và nhặt máy móc bị vứt bỏ. Khi thảm họa xảy ra, anh sống qua đợt sụp đổ ban đầu nhưng thấy mình đơn độc giữa vương quốc mà mọi quy luật đã trở nên chết chóc.

Mục tiêu ban đầu rất thực tế: sống sót, tìm đường lên mặt đất và thoát khỏi Vaelen. Mục tiêu ấy bắt đầu thay đổi khi anh phát hiện bằng chứng rằng một số Vỏ Xác vẫn giữ được phản ứng nhận biết, ký ức hoặc hành vi cá nhân bên dưới sự tha hóa.

Kaelen không thể cứu tất cả chỉ bằng cách chống lại mệnh lệnh của Đại Tháp. Anh phải tìm hiểu điều gì còn sót lại bên trong những người mắc kẹt trong hệ thống, và liệu có thể tháo dỡ cỗ máy mà không phá hủy những gì nó từng được dựng lên để duy trì hay không.

Xung đột cốt lõi của Kaelen không phải anh có sẵn lòng trở thành anh hùng hay không. Mà là anh có chấp nhận trách nhiệm với một thế giới mà bản thân chưa từng chọn thuộc về hay không.

## 2.4 Ngọn Đèn Cộng Hưởng — Pharos of Twelve Wicks

Ngọn Đèn Cộng Hưởng được tạo ra bởi 12 Bậc Thầy Dị Giáo. Mục đích ban đầu của nó là lưu trữ dấu ấn cộng hưởng riêng của họ và cung cấp phương tiện phá vỡ hệ thống đồng bộ tập trung của Đại Tháp.

Ngọn Đèn bị hư hại trong cuộc nổi dậy. Mười hai dấu ấn Echo trở nên bất hoạt hoặc phân tán, trong khi ý thức nguyên bản tương ứng vẫn bị giam trong mười hai phong ấn sống của Đại Tháp.

Kaelen phát hiện cổ vật khi chạy trốn khỏi một mỏ sập. Khi máu của anh chạm vào tinh thể bên trong đã nứt, Tim Đèn đầu tiên, **Ignis**, thức tỉnh. Tần số Lệch Nhịp cho phép anh tương tác với Ngọn Đèn mà không bị ép đồng bộ theo cơ chế thông thường của Đại Tháp.

Mỗi Echo thu hồi được đem lại một bản sắc cộng hưởng và hé lộ ký ức của người đã tạo ra dấu ấn đó. Những dấu ấn này không hoàn chỉnh: chúng có thể thiếu ký ức, bị bóp méo bởi cảm xúc hoặc đưa ra những cách giải thích mâu thuẫn về cuộc nổi dậy. Kaelen phải đối chiếu lời kể với bằng chứng trong Vaelen.

Ngọn Đèn vừa là công cụ chiến đấu, vừa là phương tiện khám phá. Công dụng cuối cùng của nó phụ thuộc vào điều Kaelen học được và quyết định thực hiện.

## 2.5 Tiến trình cốt truyện

### Hồi I — Sinh tồn

Kaelen tìm thấy Ngọn Đèn, thoát khỏi những Vỏ Xác đầu tiên và nhận ra trạng thái Lệch Nhịp giúp anh chống lại một phần mạng cộng hưởng đã hỏng. Mục tiêu trước mắt của anh là tìm đường lên mặt đất.

### Hồi II — Mâu thuẫn

Khi khám phá Vaelen và thu hồi các Echo, lịch sử chính thức liên tục mâu thuẫn với bằng chứng vật lý. Kaelen phát hiện mục đích thực sự của Vòng Cổ Điều Âm, những mảnh sự thật về cuộc nổi dậy và dấu hiệu cho thấy một số Vỏ Xác vẫn còn lưu lại bản ngã cũ. Lối thoát vẫn tồn tại, nhưng bỏ lại Vaelen đồng nghĩa bỏ lại những câu hỏi chưa được giải đáp và những sinh mạng có thể vẫn còn cứu được.

### Hồi III — Trách nhiệm

Kaelen đến Đại Tháp phía trên và khám phá toàn bộ mục đích của mạng lưới thu hoạch. Những Echo cho thấy trốn thoát, phá hủy hệ thống và biến đổi nó là ba lựa chọn khác nhau, mỗi lựa chọn có một cái giá riêng.

## 2.6 Ba kết cục

### Ending A — The False Dawn

Kaelen đánh bại Đại Chưởng Nhạc và lên được mặt đất mà không tháo dỡ mạng lưới thu hoạch. Anh thoát khỏi Vaelen và phát hiện mặt đất vẫn tồn tại. Bình minh là thật, nhưng tự do của anh chưa trọn vẹn: những người bị bỏ lại vẫn mắc kẹt trong hệ thống chưa hề thay đổi về bản chất.

### Ending B — The Shattered Spire

Kaelen phá hủy lõi trung tâm của Đại Tháp và cắt kết nối giữa Vòng Cổ Điều Âm với mạng lưới thu hoạch. Hội Đồng mất cơ chế duy trì tuổi thọ bất thường, còn dân chúng được giải phóng khỏi sự đồng bộ tập trung.

Tuy nhiên, Đại Tháp cũng duy trì hạ tầng thiết yếu của nhiều khu vực Vaelen. Khi nó bị phá hủy, một phần vương quốc ngầm mất ổn định. Cơ chế kiểm soát bị phá bỏ, nhưng tự do không tự động xóa đi mọi tổn thất đã tích lũy.

### Ending C — Symphony of the Off-Beat

Sau khi thu hồi cả 12 Echo và hoàn thành chuỗi nhiệm vụ **Âm Thoa Ký Ức (*Memory Tuning*)**, Kaelen tìm ra cách sửa đổi kiến trúc cộng hưởng của Đại Tháp thay vì chỉ giữ nguyên hoặc phá hủy nó.

Dùng tần số Lệch Nhịp, anh phá vỡ quyền lực tập trung của mạng lưới thu hoạch và biến Đại Tháp thành một mạng truyền thông cộng hưởng không còn đòi hỏi dân chúng phải hy sinh. Một số Vỏ Xác lấy lại được các mảnh bản ngã cũ nếu ký ức còn đủ để phục hồi; những người khác không thể được cứu, và người chết vẫn không sống lại.

Để hoàn tất sự biến đổi, Kaelen phải từ bỏ khả năng độc nhất cho phép anh dẫn truyền mười hai Echo. Anh sống sót, nhưng không còn có thể sử dụng Ngọn Đèn làm vũ khí. Kết quả không phải sự khôi phục hoàn hảo của vương quốc cũ, mà là khởi đầu cho một tương lai nơi người dân có quyền lựa chọn việc kết nối với nhau.

**Symphony of the Off-Beat** không phải sự phục hồi một thiên đường đã mất. Đó là sự từ chối một hệ thống đòi hỏi mọi người phải phát ra cùng một nhịp.

---

# 3. Hệ thống 12 Echo — Ngọn Đèn Cộng Hưởng

## 3.1 Quy tắc cốt lõi và phạm vi hiện tại

Ngọn Đèn chứa 12 Echo: 6 Sóng Nguyên Tố và 6 Hiện Tượng Sóng. Mỗi Echo đại diện cho một bản sắc cộng hưởng, không phải một vũ khí riêng.

### Quy tắc một vũ khí

Kaelen sử dụng một vũ khí chung, tạm gọi là **Resonance Blade**. Hình dáng, moveset, hitbox, tầm đánh, tốc độ tấn công, sát thương cơ bản và animation tấn công giống nhau với cả 12 Echo.

Đổi Echo chỉ thay đổi:

1. Bảng màu cộng hưởng và VFX của vũ khí.
2. `EchoId` đang hoạt động, dùng để áp dụng Dấu Ấn Cộng Hưởng lên mục tiêu.

Đổi Echo **không trực tiếp** tăng hoặc giảm HP Damage, Break, tầm đánh, thời gian khởi động/thu hồi đòn, chỉ số di chuyển hay kích thước hitbox. Khác biệt gameplay xuất hiện khi bản sắc Echo tham gia một phản ứng hợp lệ, được định nghĩa rõ.

Để dễ tiếp cận và dễ đọc combat, màu sắc không được là dấu hiệu duy nhất. Mỗi Echo cần có glyph/icon riêng và tên nhất quán trong HUD.

### Triad Deck hiện tại

Người chơi trang bị tối đa ba Echo đã mở khóa vào **Triad Deck**: một Echo đang hoạt động và hai Echo chuyển nhanh. Cả ba đều là chế độ của cùng một vũ khí, không phải ba ô vũ khí khác nhau. Bộ ba được cấu hình tại Tuning Resonator. Có thể chuyển qua lại giữa ba Echo đã trang bị mà không cần quay về Resonator.

Phạm vi hiện tại không bao gồm Wing Assist riêng cho từng Echo, 12 moveset vũ khí riêng hoặc 12 traversal mutation riêng. Các hệ thống đó được hoãn cho tới khi vũ khí dùng chung và hệ phản ứng được kiểm chứng.

Người chơi không kích hoạt phản ứng vẫn phải giữ đủ hiệu quả cơ bản của vũ khí chung. Phản ứng là lợi thế tùy chọn, không phải nghĩa vụ xoay vòng sát thương.

## 3.2 Bảng bản sắc Echo

Màu dưới đây là nhận diện hình ảnh sơ bộ, không phải bộ điều chỉnh chỉ số. Giá trị màu, độ tương phản, animation và glyph cuối cùng phải được kiểm tra trong game với mọi nền môi trường và HUD.

| ID | Echo | Loại cộng hưởng | Bản sắc hư cấu chính | Bảng màu tạm thời |
|---:|---|---|---|---|
| 0 | **Ignis** | Nhiệt Âm (*Thermal*) | Nhiệt, bắt lửa, ứng suất nhiệt | Cam than hồng |
| 1 | **Terra** | Địa Âm (*Seismic*) | Khối lượng, rung chấn, lực cấu trúc | Vàng đất / đá |
| 2 | **Fulgur** | Áp Điện (*Piezoelectric*) | Dẫn điện, tích điện, truyền lực đột ngột | Cyan điện |
| 3 | **Glacies** | Băng Kết Tinh (*Cryo-crystalline*) | Kết tinh, giòn vỡ, trật tự hình học | Xanh băng |
| 4 | **Ventus** | Khí Động Âm (*Aero-acoustic*) | Luồng khí, lực nâng, chênh áp | Xanh ngọc |
| 5 | **Aqua** | Thủy Kích Chân Không (*Hydro-cavitation*) | Áp suất chất lỏng, bọt khí, implosion | Xanh nước đậm |
| 6 | **Lux** | Quang Âm (*Photonic*) | Ánh sáng, khả năng quan sát, khúc xạ | Vàng ngà |
| 7 | **Umbra** | Ám Âm / Triệt Tiêu (*Null*) | Triệt tiêu, im lặng, sự vắng mặt | Đen tím |
| 8 | **Gravitas** | Trọng Âm (*Gravitational*) | Khối lượng, lực hút, lực thay đổi vị trí | Chàm |
| 9 | **Tempus** | Thời Âm (*Temporal delay*) | Trì hoãn, lưu hành động, lệch thời gian | Bạc cyan |
| 10 | **Aether** | Sinh Âm (*Bio-sympathetic*) | Hệ sống và hệ quả được chia sẻ | Đỏ crimson |
| 11 | **Stridor** | Phá Âm (*Shockwave*) | Va chạm, phản lực, mặt sóng áp suất | Trắng cyan |

## 3.3 Dấu Ấn Cộng Hưởng — Quy tắc trạng thái chính xác

Một đòn vũ khí đánh trúng hợp lệ sẽ áp dụng **Resonance Mark** lên mục tiêu theo Echo đang hoạt động.

- **Thời lượng:** 420 tick mô phỏng (3,5 giây ở 120 Hz).
- **Số dấu tối đa:** Mỗi mục tiêu chỉ có một Resonance Mark đang hoạt động. Dấu không cộng dồn.
- **Đánh bằng cùng Echo:** Làm mới thời gian dấu của Echo đó về 420 tick. Không kích hoạt phản ứng.
- **Đánh bằng Echo khác, cặp hợp lệ:** Xử lý đòn đánh thông thường, sau đó áp dụng hiệu ứng bổ sung của phản ứng đúng theo bảng. Dấu cũ và dấu mới bị tiêu thụ. Mục tiêu không còn Resonance Mark đang hoạt động.
- **Đánh bằng Echo khác, cặp không hợp lệ:** Dấu cũ bị thay bằng dấu của Echo mới, thời lượng 420 tick. Không kích hoạt phản ứng có tên.
- **Dấu hết hạn:** Bị xóa trước khi kiểm tra phản ứng trong tick đó. Dấu đã hết hạn không thể dùng để kích hoạt phản ứng.
- **Giới hạn mỗi đòn:** Một attack instance chỉ áp dụng dấu lên cùng một mục tiêu tối đa một lần, trừ khi một đòn nhiều hit trong tương lai được đặc tả ngoại lệ rõ ràng. Sát thương phụ do phản ứng tạo ra không áp dụng dấu và không thể kích hoạt phản ứng khác.
- **Giải quyết phản ứng:** Phản ứng không tự kích hoạt đệ quy. Cặp Echo được tra từ bảng cụ thể tại Mục 4.4; thứ tự hai Echo không quan trọng.

### Ví dụ

1. Ignis đánh trúng mục tiêu: mục tiêu nhận sát thương cơ bản và dấu Ignis tồn tại 420 tick.
2. Glacies đánh cùng mục tiêu trước khi dấu hết hạn: đòn đánh gây sát thương cơ bản, sau đó kích hoạt Melt-Shock với lượng sát thương bổ sung, Break và hiệu ứng trạng thái đã định nghĩa. Dấu Ignis và Glacies bị tiêu thụ.
3. Đòn Glacies kế tiếp tạo dấu Glacies mới. Nó không tự kích hoạt phản ứng lần nữa vì cặp trước đã bị tiêu thụ.

## 3.4 Phạm vi traversal và năng lực

Ý tưởng ban đầu về 12 Traversal Mutation riêng cho từng Echo vẫn có thể được phát triển về sau, nhưng không phải yêu cầu triển khai hiện tại. Không cổng tiến trình nào được giả định rằng 12 mutation này đã tồn tại.

Trong bản thiết kế hiện tại, nâng cấp traversal là năng lực thế giới được lưu vĩnh viễn. Người chơi không bị mất quyền vào khu vực bắt buộc chỉ vì đang dùng Echo khác hoặc một Echo không nằm trong Triad Deck.

## 3.5 Quy ước phản ứng

Phiên bản đầu hỗ trợ mười loại phản ứng có tên, với mười hai công thức ghép hợp lệ. Hiệu ứng, giá trị bổ sung, loại mục tiêu và giới hạn được định nghĩa ở Mục 4.4. Các cặp khác không có hiệu ứng đặc biệt ngầm định; chúng tuân theo quy tắc thay dấu mặc định.

---

# 4. Combat System — Quy tắc rõ ràng và thông số prototype

> **Lưu ý cân bằng:** Các giá trị trong mục này tạo thành một bộ thông số khởi điểm nhất quán để làm prototype. Chúng giúp triển khai không bị suy diễn, nhưng chưa phải tuyên bố về cân bằng cuối cùng. Mọi thời lượng đều tính bằng tick ở nhịp mô phỏng 120 Hz, trừ khi ghi khác.

## 4.1 Nguyên tắc combat

1. **Một vũ khí, một moveset cốt lõi.** Đổi Echo không thay đổi chỉ số cơ bản hoặc bộ animation của vũ khí.
2. **Có thể chơi tốt mà không combo Echo.** Game phải hoàn thành được mà không phụ thuộc vào một chuỗi phản ứng cụ thể. Phản ứng cải thiện hiệu quả và tạo lựa chọn chiến thuật, nhưng không bắt buộc để giữ sát thương cơ bản.
3. **Tách biệt các đại lượng.** HP Damage, Break và CC được thiết kế và xử lý độc lập.
4. **Không tạo vòng lặp vô hạn miễn phí.** Phản ứng không tự hồi Air-Dash, không kích hoạt đệ quy và không hồi Harmony ngoài các quy tắc được liệt kê.
5. **Quy tắc địch dễ đọc.** Đòn có thể parry, không thể đỡ, phá thế thủ hoặc miễn khống chế phải có tín hiệu rõ ràng và cách xử lý nhất quán.
6. **Boss không phải quái thường có nhiều HP hơn.** Boss nhận sát thương và Break, nhưng hard-stun, cưỡng ép dịch chuyển và ngắt đòn liên tục không tự động áp dụng như đối với quái thường.

## 4.2 Vũ khí chung và chỉ số combat cơ bản

### Bộ HP mục tiêu dùng cho prototype

| Loại mục tiêu | HP prototype | Break Meter | Ghi chú |
|---|---:|---:|---|
| Quái thường | 100 | 100 | Dùng quy tắc stagger/CC thông thường |
| Quái nặng / Elite | 300 | 100 | Có thể kháng dịch chuyển; phải định nghĩa bằng cờ dữ liệu |
| Boss | Theo encounter; khởi điểm thử nghiệm 1.200 mỗi phase | 100 | Kháng và quy tắc riêng của từng Boss phải được chỉ rõ |

HP là giá trị khởi điểm cho prototype. HP Boss có thể khác theo encounter; Break Meter giữ mức 100 trừ khi đặc tả encounter ghi rõ ngoại lệ.

### Chuỗi Light Attack trên mặt đất

| Đòn | HP Damage | Break | Định nghĩa |
|---|---:|---:|---|
| Light 1 | 10 | 4 | Đòn mở đầu nhanh |
| Light 2 | 12 | 5 | Tiếp nối chuỗi mặt đất |
| Light 3 | 18 | 9 | Đòn kết thúc chuỗi ba hit |

Một chuỗi đủ ba hit, không bị gián đoạn, gây tổng cộng 40 HP Damage và 18 Break trước phần thưởng phản ứng. Một mục tiêu chỉ bị trúng một lần bởi mỗi attack instance.

### Chuỗi Air Attack

| Đòn | HP Damage | Break | Định nghĩa |
|---|---:|---:|---|
| Air 1 | 10 | 4 | Đòn trên không thứ nhất |
| Air 2 | 15 | 7 | Đòn kết thúc chuỗi trên không |

Một chuỗi hai hit trên không gây 25 HP Damage và 11 Break trước phần thưởng phản ứng.

### Heavy Attack

- **Startup:** 24 tick (200 ms) trước khi hitbox hoạt động.
- **HP Damage:** 24.
- **Break:** 18.
- **Vai trò:** Đòn có cam kết thời gian lớn hơn, dễ bị trừng phạt nếu dùng sai thời điểm nhưng gây Break cao hơn một đòn Light đơn lẻ.
- **Quy tắc:** Heavy Attack không tự động hất tung, làm choáng hoặc đẩy lùi mục tiêu. Các hiệu ứng đó phải được đặc tả bởi một năng lực hay phản ứng riêng.

### Quy tắc input và hit resolution

- Vũ khí chung dùng cùng animation, timing, tầm đánh và cấu hình hitbox với cả 12 Echo.
- `EchoId` đang hoạt động được chụp lại tại thời điểm đòn đánh bắt đầu. Attack instance giữ nguyên EchoId này cho tới khi hoàn tất; đổi Echo giữa animation không thể thay loại dấu của một đòn đã bắt đầu.
- Đổi Echo không hủy đòn đang đánh và không cấp bất tử. VFX có thể chuyển màu ngay, nhưng đòn hiện tại vẫn xử lý theo EchoId đã chụp lúc bắt đầu.
- Input buffer, cancel window và phím cuối cùng cho keyboard/gamepad sẽ được quy định tại mục Controls. Chúng không được thay đổi các quy tắc Damage, Break hay phản ứng đã định nghĩa ở đây.

## 4.3 HP Damage, Break và Crowd Control — định nghĩa độc lập

### HP Damage

HP Damage là lượng bị trừ khỏi máu mục tiêu sau các hệ số sát thương áp dụng. **Bonus Damage** của phản ứng được cộng thêm vào sát thương bình thường của đòn đã kích hoạt phản ứng. Sát thương phụ đánh vào mục tiêu khác cũng được cộng thêm theo đúng bảng phản ứng.

### Break

Break là thanh tư thế có phạm vi 0–100.

- Mỗi đòn cộng lượng Break được đặc tả, sau khi nhân hệ số kháng Break của mục tiêu nếu dữ liệu mục tiêu có định nghĩa hệ số đó.
- Khi quái thường đạt 100 Break, nó vào trạng thái **Stagger** trong 45 tick (0,375 giây). Trong Stagger, Break cộng thêm bị bỏ qua. Thanh Break đặt lại về 0 khi Stagger kết thúc.
- Khi quái nặng/Elite hoặc Boss đạt 100 Break, nó vào **Break State** trong 150 tick (1,25 giây). Trong Break State, mục tiêu không thể khởi động đòn tấn công mới; một đòn đang wind-up và có thể ngắt sẽ bị hủy khi Break State bắt đầu. Mục tiêu nhận `1.25x` HP Damage. Thanh Break đặt lại về 0 khi Break State bắt đầu.
- Khi quái nặng/Elite hoặc Boss thoát Break State, nó có **Break Recovery** kéo dài 240 tick (2 giây). Trong thời gian này, mọi Break cộng thêm bị bỏ qua. HP Damage và CC được phép rõ ràng vẫn có hiệu lực.
- Boss không thể bị khóa trong trạng thái Break liên tục bằng cách làm đầy thanh trong Break State hoặc Break Recovery.
- Trừ khi một hiệu ứng ghi rõ khác đi, các hệ số HP Damage hợp lệ được nhân với nhau. Ví dụ, Fracture (`1.15x`) và Boss Break State (`1.25x`) tạo ra `1.4375x` sát thương trước hệ số kháng riêng của mục tiêu. Không có hệ số sát thương ngầm nào khác.

Một encounter có thể định nghĩa hệ số kháng Break, nhưng phải ghi rõ hệ số đó. Không được âm thầm thay đổi thanh chung 0–100.

### Crowd Control (CC)

CC không đồng nghĩa với sát thương hoặc Break. Mỗi hiệu ứng CC phải nêu loại, nhóm mục tiêu, cường độ và thời lượng.

- **Interrupt:** Hủy wind-up của đòn địch nếu đòn đó có thể bị ngắt. Không tự động ngăn đòn tiếp theo.
- **Micro-stagger:** Trong thời gian đã định, mục tiêu không thể chủ động di chuyển hoặc tấn công. Hiệu ứng này ngắn hơn Stun toàn phần và chỉ dùng cho quái thường nếu không có ngoại lệ rõ ràng.
- **Stun:** Mục tiêu không thể chủ động di chuyển hoặc tấn công trong thời lượng đã nêu. Chỉ áp dụng cho Standard và Heavy/Elite khi hiệu ứng liệt kê rõ nhóm đó. Boss không bị Stun trừ khi encounter Boss cho phép cụ thể.
- **Slow:** Nhân tốc độ di chuyển và tiến trình animation/hành động đủ điều kiện với hệ số đã định, trong thời lượng đã nêu. Slow 40% nghĩa là tốc độ giảm còn 60% bình thường.
- **Knockback:** Áp dụng thay đổi vận tốc tức thời được liệt kê lên mục tiêu cho phép dịch chuyển. Quái nặng và Boss có thể bỏ qua dịch chuyển; điều này không hủy Damage hoặc Break đã ghi riêng trong phản ứng.
- **Pull:** Di chuyển mục tiêu đủ điều kiện về một điểm đã xác định trong thời gian được nêu. Boss không bị cưỡng ép thay đổi vị trí trừ khi encounter cho phép rõ ràng.
- **Xóa đạn:** Chỉ xóa projectile thường đủ điều kiện. Bẫy màn chơi, chiêu đặc trưng của Boss và projectile được bảo vệ không bị xóa trừ khi encounter cho phép.

Boss không nhận Stun thông thường hoặc cưỡng ép dịch chuyển từ các phản ứng chuẩn. Nếu một phản ứng có tác động khống chế lên Boss, bảng phản ứng phải ghi chính xác tác động đó.

Thời lượng CC không cộng dồn. Khi áp dụng lại cùng loại CC, thời gian được làm mới và giữ cường độ mạnh hơn. Với hiệu ứng khóa hành động, thứ tự ưu tiên là `Break State > Stun > Micro-stagger`; hiệu ứng ưu tiên thấp hơn không được rút ngắn hiệu ứng ưu tiên cao hơn. Slow có thể cùng tồn tại với hiệu ứng khóa hành động, nhưng các hiệu ứng Slow lặp lại không cộng phần trăm; dùng mức Slow mạnh nhất và làm mới thời gian.

## 4.4 Phản ứng Sóng Dừng — công thức và hiệu ứng chính xác

### Trình tự kích hoạt chung

1. Đòn Echo đầu tiên gây Damage/Break cơ bản và đặt dấu.
2. Đòn từ Echo khác gây Damage/Break cơ bản của đòn đó.
3. Nếu cặp dấu cũ/mới là công thức hợp lệ, áp dụng đúng một lần Damage bổ sung, Break bổ sung và hiệu ứng ghi trong bảng.
4. Tiêu thụ cả hai dấu. Mục tiêu phụ trúng sát thương phản ứng không nhận dấu và không kích hoạt phản ứng khác.
5. Cộng **10 Harmony** cho mỗi sự kiện Signature Reaction được giải quyết thành công.

Không phản ứng nào hồi Air-Dash. Không phản ứng nào cấp Harmony nhiều hơn một lần cho cùng sự kiện. Không phản ứng nào tự nhắm toàn màn hình; mỗi phản ứng dưới đây có bán kính và số mục tiêu giới hạn. Hit pipeline kiểm tra mục tiêu chính và dấu hiện tại tại thời điểm va chạm, xử lý đòn cơ bản cùng phản ứng như một sự kiện hit, sau đó mới dọn trạng thái chết/despawn. Vì thế phản ứng vẫn có thể xảy ra khi đòn kích hoạt làm mục tiêu chính chết. Sát thương phụ bỏ qua mục tiêu đã chết hoặc không còn hợp lệ.

### Bảng Signature Reaction

| Phản ứng | Cặp Echo hợp lệ | HP Damage bổ sung | Break bổ sung | Hiệu ứng thêm và giới hạn |
|---|---|---:|---:|---|
| **1. Melt-Shock** | Ignis + Glacies | 24 | 16 | Áp dụng **Fracture** trong 720 tick (6 giây): các đòn sau gây `1.15x` HP Damage lên mục tiêu. Fracture không cộng dồn; áp dụng lại làm mới thời gian. Có hiệu lực với Boss. |
| **2. Piezo-Quake** | Terra + Fulgur | 20 | 22 | Quái Standard và Heavy/Elite bị Interrupt và Stun trong 42 tick (0,35 giây). Boss không bị Stun, chỉ nhận Damage và Break đã liệt kê. |
| **3. Steam Cavitation** | Ignis + Aqua | 22 | 12 | Mục tiêu chính và tối đa hai kẻ địch phụ đủ điều kiện trong phạm vi 1,75 m bị ảnh hưởng. Mỗi mục tiêu phụ nhận 8 HP Damage, không nhận Break. Quái nhẹ đủ điều kiện nhận thay đổi vận tốc hướng ra ngoài `2.5 m/s`; dịch chuyển của Heavy/Elite và Boss tùy theo kháng của chúng. Sát thương phụ không áp dụng dấu. |
| **4. Superconductor Gale** | Fulgur + Ventus | 18 | 16 | Truyền điện tới tối đa hai mục tiêu phụ trong phạm vi 2,5 m từ mục tiêu chính. Mỗi mục tiêu phụ nhận 9 HP Damage và 4 Break. Mục tiêu phụ không nhận dấu và không kích hoạt phản ứng khác. |
| **5. Pyro-Cyclone** | Ignis + Ventus | 20 | 10 | Tối đa bốn kẻ địch phụ đủ điều kiện trong phạm vi 2,5 m nhận 12 HP Damage mỗi mục tiêu, không nhận Break. Xóa tối đa ba projectile thường đủ điều kiện trong cùng bán kính. Không xóa bẫy được kịch bản hóa hoặc projectile Boss được bảo vệ. |
| **6. Tectonic Shatter** | Glacies + Terra **hoặc** Glacies + Stridor | 28 | 28 | Hủy wind-up có thể ngắt và áp dụng Micro-stagger 30 tick (0,25 giây) lên quái Standard. Nếu mục tiêu có thuộc tính `BreakableGuard` và đang thủ, phản ứng phá trạng thái thủ hiện tại và áp dụng `GuardBroken` trong 90 tick (0,75 giây), thời gian này không thể kích hoạt lại kiểu thủ đó. Không tự động phá mọi loại khiên/giáp. Boss không bị Stun. |
| **7. Eclipse Singularity** | Lux + Umbra | 26 | 18 | Kéo tối đa bốn quái nhẹ đủ điều kiện trong phạm vi 3,5 m của mục tiêu chính lại gần mục tiêu đó tối đa 1,5 m trong 12 tick (0,1 giây), dừng cách mục tiêu ít nhất 0,5 m. Boss và mục tiêu neo cố định không bị dịch chuyển. Không hút toàn màn hình. |
| **8. Graviton Quake** | Gravitas + Terra | 22 | 25 | Buộc quái thường đủ điều kiện đang ở trên không chạm đất. Tối đa bốn kẻ địch phụ đủ điều kiện trong phạm vi 2 m từ mục tiêu chính nhận 8 HP Damage, không nhận Break. Boss không bị ép chạm đất hoặc dịch chuyển. |
| **9. Chrono-Overdrive** | Tempus + Stridor | 20 | 14 | Sau 12 tick (0,1 giây), một đòn trễ gây thêm 10 HP Damage lên mục tiêu gốc nếu mục tiêu vẫn tồn tại và hợp lệ. Đòn trễ không cộng Break, không đặt dấu, không cấp Harmony và không thể kích hoạt phản ứng. Nếu mục tiêu gốc đã chết hoặc không còn hợp lệ, đòn trễ bị hủy. |
| **10. Sanguine Boil** | Aether + Ignis **hoặc** Aether + Aqua | 18 | 12 | Tối đa hai kẻ địch phụ đủ điều kiện trong phạm vi 2 m của mục tiêu chính nhận 8 HP Damage mỗi mục tiêu, không nhận Break. Trong phạm vi hiện tại, phản ứng không đòi hỏi một năng lực liên kết riêng; nó không lan dấu hoặc nối chuỗi thêm mục tiêu. |

Bảng này định nghĩa **10 loại phản ứng có tên và 12 công thức ghép hợp lệ**, vì Tectonic Shatter và Sanguine Boil mỗi loại có hai cặp thay thế. Các cặp Echo còn lại tuân theo quy tắc thay dấu mặc định.

## 4.5 Parry — Phase-Inversion Counter

### Quy tắc tín hiệu

Elite và Boss sử dụng tín hiệu tấn công dễ đọc, kết hợp dấu hiệu thị giác và âm thanh riêng biệt. Màu sắc phải đi kèm hình dạng, icon, animation hoặc âm thanh để người chơi vẫn nhận ra đòn tấn công khi không phân biệt được màu.

- **Gold Harmonic Wave — Có thể Parry:** Parry thành công vô hiệu hóa hit đó lên Kaelen và ngắt kẻ tấn công nếu nó đang ở trạng thái có thể bị ngắt.
- **Crimson Overload Wave — Không thể đỡ:** Parry không thể hóa giải. Người chơi phải tránh đòn bằng di chuyển, nhảy hoặc Dash chung. Đòn này không được bắt buộc dùng một traversal mutation riêng của Echo vì các năng lực ấy đang nằm ngoài phạm vi hiện tại.
- **Đòn khác:** Khả năng Parry/Dodge phải được khai báo trong dữ liệu attack của kẻ địch. Không được suy luận chỉ dựa trên sát thương hoặc màu sắc.

### Cửa sổ Parry và phần thưởng

- **Cửa sổ thời gian:** Parry thành công khi input được phát ra trong vòng 10 tick (xấp xỉ 83 ms) trước khi hit parryable chạm tới Kaelen. Số frame active trong animation phải được căn chỉnh đúng với quy tắc này khi triển khai.
- **Parry thành công:** Vô hiệu hóa hit đang đến, ngắt attacker nếu có thể ngắt, cộng 15 Harmony và thêm 18 Break lên attacker nếu attacker có Break Meter.
- **Thưởng Phase-Inversion:** Dữ liệu attack của Boss có thể khai báo một `CounterEchoId`. Nếu Echo được chụp tại thời điểm Parry khớp với ID đó, Parry cộng thêm 12 Break. Như vậy, Parry đúng Echo cộng tổng 30 Break (18 cơ bản + 12 thưởng), còn Parry bình thường cộng 18 Break. Đây là lợi thế bổ sung, không phải điều kiện để Parry thành công.
- **Không cộng thưởng dây chuyền:** Phase-Inversion Parry không tự nâng lên Tier III, không cộng Harmony ngoài 15 điểm Parry cơ bản và không ép Boss vào Break State trừ khi thanh Break thực sự đạt 100.
- **Parry thất bại:** Nếu bỏ lỡ cửa sổ thời gian, đòn nhận sát thương theo quy tắc thông thường. Không có cơ chế block hoặc bất tử ẩn khi Parry sai thời điểm.

## 4.6 Di chuyển trên không và quy tắc chống vòng lặp vô hạn

- Kaelen bắt đầu mỗi chuỗi trên không với **một Air-Dash charge**.
- Dùng Air-Dash tiêu thụ charge. Charge được hồi khi Kaelen trở lại trạng thái đứng đất hợp lệ.
- Signature Reaction không hồi Air-Dash.
- Một năng lực di chuyển trong tương lai có thể cấp thêm Air-Dash nếu điều kiện kích hoạt và giới hạn tối đa được ghi rõ. Hiện tại chưa có năng lực riêng cho từng Echo như vậy.
- Hit-stop, nếu được dùng, là hiệu ứng game-feel/trình bày; nó không được tạo hit event bổ sung, lặp áp dụng dấu, làm timer hết hạn không nhất quán hoặc khiến một attack instance gây sát thương nhiều lần.

Các quy tắc này ngăn hệ phản ứng cơ bản biến thành động cơ combo trên không vô hạn. Người chơi vẫn có thể nối hành động trên không bằng nhảy, vị trí, thời điểm tấn công và một Air-Dash hiện có.

## 4.7 Harmony Gauge và Unison

### Quy tắc Harmony

- **Phạm vi:** 0–100.
- **Tăng:** +10 cho một Signature Reaction được giải quyết thành công; +15 cho một Parry thành công.
- **Không tăng:** Đòn vũ khí thông thường, đòn đánh hụt, sát thương phụ của phản ứng, sát thương trễ và sát thương theo thời gian không tạo Harmony.
- **Giới hạn:** Harmony không vượt quá 100; phần vượt mức bị bỏ.
- **Chết/checkpoint:** Quy tắc death và checkpoint được định nghĩa ở Mục 5. Harmony không bao giờ được âm.

### Standard Unison Burst

Standard Unison và Sacred Chord là hai gameplay action riêng; phím/nút cuối cùng sẽ được xác định tại mục Controls. Người chơi có thể dùng Standard Unison khi Harmony từ 50 trở lên, kể cả khi bộ Echo hiện tại đủ điều kiện dùng Sacred Chord.

- **Chi phí:** 50 Harmony, thanh toán ngay lúc kích hoạt.
- **Phạm vi:** Bán kính 3 m quanh Kaelen.
- **Mục tiêu:** Tối đa năm kẻ địch đủ điều kiện, chọn theo quy tắc target của combat.
- **Hiệu ứng trên mỗi mục tiêu:** 18 HP Damage và 15 Break.
- **Khống chế:** Quái Standard bị Interrupt trong 30 tick (0,25 giây). Boss không bị Stun hoặc cưỡng ép dịch chuyển.
- **Dấu và phản ứng:** Damage từ Unison không áp dụng Resonance Mark và không kích hoạt Signature Reaction.

### Sacred Chords

Sacred Chord chỉ dùng được khi Harmony đầy 100 và ba Echo khác nhau trong Triad Deck khớp với một trong các bộ chính xác dưới đây; thứ tự ô không quan trọng. Kích hoạt action Sacred Chord riêng sẽ tiêu thụ toàn bộ 100 Harmony. Action Standard Unison vẫn có thể được dùng độc lập với chi phí 50 Harmony thay vì Sacred Chord.

| Sacred Chord | Triad Deck yêu cầu | Hiệu ứng chính xác |
|---|---|---|
| **Elemental Cataclysm** | Ignis + Terra + Fulgur | Vụ nổ bán kính 4 m quanh Kaelen. Mỗi mục tiêu trong tối đa năm kẻ địch đủ điều kiện nhận 30 HP Damage và 24 Break. Quái nhẹ đủ điều kiện nhận thay đổi vận tốc hướng ra ngoài `2.0 m/s`. Chỉ địa hình được gắn cờ phá hủy mới bị hư hại; không phá tùy ý hình học màn chơi. |
| **Cryo-Tempest Protocol** | Glacies + Ventus + Aqua | Vụ nổ bán kính 4 m. Mỗi mục tiêu trong tối đa năm kẻ địch đủ điều kiện nhận 22 HP Damage và 18 Break. Quái không phải Boss bị Slow 40% trong 180 tick (1,5 giây). Boss nhận Damage và Break nhưng không bị Slow. |
| **Eclipse of Eternity** | Lux + Umbra + Tempus | Ba nhát chém ảnh ảo đánh tối đa ba kẻ địch đủ điều kiện; mỗi nhát gây 8 HP Damage, tổng 24 Damage và 18 Break cho mỗi mục tiêu. Quái Standard không thể hành động trong 72 tick (0,6 giây). Boss không bị đóng băng hoặc Stun, chỉ nhận Damage và Break đã liệt kê. |
| **Singularity Overdrive** | Gravitas + Aether + Stridor | Trong bán kính 4,5 m, quái nhẹ đủ điều kiện bị kéo về điểm trung tâm trong 36 tick (0,3 giây), sau đó nhận 28 HP Damage và 25 Break. Tối đa năm mục tiêu bị ảnh hưởng. Boss không bị dịch chuyển nhưng vẫn nhận Damage và Break. |

Sacred Chord không áp dụng dấu, không kích hoạt phản ứng và không cấp thêm Harmony. Đây là lựa chọn sức mạnh cao, không phải điều kiện bắt buộc để đánh bại bất kỳ Boss nào.

## 4.8 Amplitude Ascension — trạng thái hiện tại

Hệ thống cũ tăng tầm đánh và sát thương thông qua Tier của từng vũ khí được **loại khỏi phạm vi hiện tại**. Game chỉ có một vũ khí; đổi Echo không thay chỉ số vũ khí. Thêm một thanh Wave Energy nữa chưa có lý do được kiểm chứng và sẽ làm tăng độ phức tạp không cần thiết.

Không triển khai `Tier I / Tier II / Tier III`, hệ số tầm đánh, Crescendo Wave-Burst tự động hoặc thưởng sát thương khi đổi Echo trong prototype hiện tại. Nếu playtest sau này cho thấy việc đổi Echo thiếu giá trị chiến thuật, hãy xem xét tiến trình nâng cấp sau khi vũ khí chung, dấu, phản ứng, Parry, Break và Harmony đã ổn định.

## 4.9 Khả năng đọc combat và yêu cầu test

Mọi phản ứng, Parry, Break State, CC và thay đổi Harmony phải có phản hồi dễ nhận biết. Tối thiểu:

- Hiển thị glyph Echo đang hoạt động trong HUD và icon của dấu trên mục tiêu.
- Dùng dấu hiệu thị giác/âm thanh riêng cho hit thường, tăng Break, Break State, Parry thành công, kích hoạt phản ứng và dấu hết hạn.
- Không dùng màu sắc làm dấu hiệu duy nhất để phân biệt đòn an toàn hoặc loại phản ứng.
- Sát thương phản ứng không được tạo dấu mới, kích hoạt phản ứng đệ quy hoặc cấp Harmony ngoài quy tắc.
- Người chơi chỉ dùng một Echo vẫn phải đánh bại được quái thường và hoàn thành encounter cơ bản mà không bị phạt sát thương ngầm.
- Kiểm tra mục tiêu chết/despawn, dấu hết hạn đúng tick bị hit, hit đồng thời, phản ứng nhiều mục tiêu, Boss Break Recovery và chuyển trạng thái death/checkpoint.

---

# 5. Hệ thống thế giới Metroidvania

## 5.1 Tuning Resonators — Checkpoint

Tuning Resonator là checkpoint: hồi HP, đưa Harmony về 100, lưu tiến trình và cho phép cấu hình Triad Deck gồm ba Echo đã mở khóa.

Không cần dùng Resonator để chuyển qua lại giữa ba Echo đã trang bị. Năng lực traversal bắt buộc đã mở khóa được lưu vĩnh viễn, không bị vô hiệu hóa do Echo đang hoạt động hoặc cấu hình deck.

**Resonance Sigils** có thể được trang bị tại Resonator nếu được phát triển thành hệ nâng cấp riêng. Tác dụng của từng Sigil phải được định nghĩa riêng và không được âm thầm thay đổi hợp đồng combat ở Mục 4.

## 5.2 Lost Wick Husk — Thu hồi sau khi chết

Khi chết, Kaelen hồi sinh tại Tuning Resonator được kích hoạt gần nhất, Harmony đặt về 0 và một **Lost Wick Husk** được đặt tại vị trí tử nạn. Husk giữ toàn bộ **Tuning Shards** đang mang theo và áp dụng mức giảm `25%` HP tối đa cho tới khi được thu hồi. Mức phạt HP tối đa không cộng dồn vượt quá 25%.

Tương tác và phá Lost Wick Husk sẽ thu hồi Shards, gỡ mức giảm HP tối đa và hồi Harmony về 100. HP sau khi khôi phục bị giới hạn ở HP tối đa mới; không được hồi vượt quá mức tối đa.

Prototype chỉ cho phép tồn tại một Lost Wick Husk có thể thu hồi tại một thời điểm. Nếu Kaelen chết lần nữa trước khi thu hồi Husk, Husk hiện tại vẫn nằm ở vị trí ban đầu và Tuning Shards mới đang mang theo được cộng vào cùng kho đó; không tạo Husk thứ hai. Quy tắc này tránh nhân đôi mộ và không âm thầm làm mất tiền đã mang.

## 5.3 Acoustic Fog of War và Map Pins

Bản đồ được mở dần khi khám phá. Hỗ trợ các pin:

- `FrequencyGate`
- `UnreachedLedge`
- `LoreTablet`
- `BossArena`
- `TuningForkQuest`

Map pin là ghi chú do người chơi đặt; chúng không tự động tiết lộ lời giải bí mật. Tiến trình chính cần cung cấp đủ phản hồi môi trường để người chơi hình thành giả thuyết, nhưng không xóa bỏ khám phá tùy chọn.

## 5.4 Frequency Gates và quy tắc tiến trình

Frequency Gate là khóa môi trường, điều kiện của nó phải tương thích với năng lực thực sự có trong build hiện tại.

Trong phạm vi hiện tại, cổng có thể yêu cầu nâng cấp traversal đã lưu vĩnh viễn, vật phẩm, trạng thái câu đố môi trường hoặc world-progress flag. Cổng **không được** yêu cầu một trong 12 traversal mutation đang hoãn hoặc bắt buộc một Echo nhất định phải đang hoạt động, trừ khi cơ chế đó được đưa trở lại phạm vi và ghi đặc tả rõ ràng.

Mỗi cổng tiến trình phải truyền đạt:

1. Thuộc tính quan sát được nào đang ngăn người chơi đi qua.
2. Loại tương tác nào có thể thay đổi thuộc tính đó.
3. Cổng thuộc tuyến chính hay nội dung tùy chọn.

Không yêu cầu người chơi quay lại checkpoint chỉ để thay Echo.

---

# 6. Ranh giới triển khai EngineN / C++23

Mục này định nghĩa các khái niệm phục vụ triển khai, không yêu cầu tạo một file nguồn riêng cho từng khái niệm. Giữ ranh giới trách nhiệm rõ ràng nhưng tránh chia nhỏ quá mức.

## 6.1 Runtime baseline

- Ngôn ngữ: C++23.
- Runtime: EngineN custom runtime.
- Nhịp mô phỏng mục tiêu: fixed step 120 Hz.
- Bộ cờ biên dịch tham chiếu của dự án: `-std=c++23 -O2 -fno-exceptions -fno-rtti -Wall -Wextra -Wpedantic -Werror` khi toolchain hỗ trợ.
- Fixed step không tự bảo đảm tính deterministic xuyên nền tảng. Nếu cần replay deterministic, thứ tự xử lý, random, collision resolution và lấy mẫu input cũng phải tuân theo quy tắc có thể tái lập.

## 6.2 Khái niệm dữ liệu cốt lõi

Hệ combat cần các khái niệm tương đương:

- `EchoId` với 12 giá trị hợp lệ (`0..11`).
- `TriadDeck` chứa ba slot Echo đã được kiểm tra tính hợp lệ.
- `ResonanceMark` gồm `EchoId`, số tick còn lại và định danh mục tiêu.
- `WaveReactionId` cho 10 loại phản ứng có tên.
- Bảng công thức phản ứng gồm 12 cặp không phân biệt thứ tự.
- Thuộc tính combat: HP Damage, Break, nhóm mục tiêu, kháng CC, kháng dịch chuyển và hệ số sát thương.
- `HarmonyGauge` giới hạn trong `0..100`.
- Metadata attack của Boss, gồm loại telegraph và `CounterEchoId` tùy chọn.
- Cờ tiến trình thế giới được lưu, dữ liệu checkpoint và save.

## 6.3 Trách nhiệm hệ thống đề xuất

- **Dữ liệu Echo/Cộng hưởng:** định nghĩa bất biến về ID, tên, glyph, palette và công thức phản ứng hợp lệ.
- **Weapon combat:** trạng thái attack chung, EchoId được chụp theo attack instance, xác nhận hit, Damage cơ bản và Break cơ bản.
- **Mark/reaction resolution:** hết hạn dấu, tra cặp hợp lệ, tiêu thụ/thay dấu, áp dụng payload phản ứng và ngăn đệ quy.
- **Defense và posture:** kiểm tra cửa sổ Parry, tiến trình Break Meter, Break State, Break Recovery và CC rõ ràng.
- **Harmony/Unison:** giới hạn tài nguyên, quy tắc tăng/tiêu thụ, giới hạn mục tiêu và kiểm tra Sacred Chord.
- **World progression:** checkpoint, thu hồi sau death, save state, traversal upgrade lưu vĩnh viễn và điều kiện cổng.

Các trách nhiệm này có thể được tổ chức trong một số ít module gắn kết. Tránh một hàm combat khổng lồ che giấu quy tắc, nhưng cũng không tạo một file riêng cho từng helper nhỏ nếu chưa có nhu cầu thực tế.

## 6.4 Invariant khi triển khai

1. Một weapon attack instance dùng EchoId được chụp tại lúc đòn bắt đầu.
2. Mỗi mục tiêu có tối đa một Resonance Mark đang hoạt động trong phạm vi hiện tại.
3. Phản ứng hợp lệ được tra bằng bảng cặp không phân biệt thứ tự.
4. Sát thương phụ của phản ứng không áp dụng dấu, không kích hoạt phản ứng đệ quy và không cấp Harmony.
5. Break giới hạn trong `0..100`; Harmony giới hạn trong `0..100`.
6. Mọi timer được biểu diễn bằng tick mô phỏng và thứ tự hết hạn phải nhất quán.
7. Boss không thể bị kích hoạt Break State lần nữa trong Break Recovery.
8. Parry trượt không cấp block hoặc bất tử ẩn.
9. Không quy tắc nào ở Mục 3–4 phụ thuộc vào vũ khí riêng, Wing Assist hoặc traversal mutation đang hoãn.
10. Chuyển trạng thái checkpoint/death phải giữ save và world state hợp lệ, không nhân đôi kho Lost Wick Husk.

## 6.5 Test case khuyến nghị

- Làm mới dấu bằng cùng Echo; cặp hợp lệ/không hợp lệ; kiểm tra đúng biên hết hạn 420 tick.
- Kiểm thử cả 12 công thức hợp lệ và ít nhất một trường hợp cho mỗi lớp cặp không hợp lệ.
- Mục tiêu phụ trúng sát thương phản ứng không được nhận dấu hoặc kích hoạt phản ứng thứ hai.
- Damage và Break của phản ứng được cộng thêm đúng một lần trên đòn kích hoạt.
- Boss đạt 100 Break; vào Break State; bỏ qua Break trong Break State/Recovery; có thể bị Break lại sau khi hồi phục.
- Parry thành công và thất bại tại biên cửa sổ 10 tick.
- `CounterEchoId` chỉ cộng đúng 12 Break bổ sung, không cấp Tier/Harmony ngoài quy tắc.
- Harmony không vượt 100; Unison trả đúng chi phí; Sacred Chord cần đúng bộ ba Echo không phân biệt thứ tự.
- Chết trước khi thu hồi vẫn giữ một Lost Wick Husk và không nhân đôi/mất Shards.
- Đổi Echo giữa đòn không thay EchoId đã chụp trong attack instance đó.

---

# Phụ lục A — Tóm tắt phạm vi hiện tại

## Trong phạm vi

- Một Resonance Blade với moveset dùng chung.
- 12 bản sắc Echo với palette/glyph riêng và Resonance Mark ID.
- Triad Deck gồm ba Echo chuyển nhanh, cấu hình tại Tuning Resonator.
- Một dấu trên mỗi mục tiêu, thời lượng 420 tick, 10 loại phản ứng có tên với 12 công thức cặp.
- HP Damage, Break và CC được tách biệt rõ.
- Parry, Phase-Inversion bonus, Break State/Recovery, Harmony, Standard Unison và bốn Sacred Chord.
- Nền cốt truyện Vaelen, ba kết cục, checkpoint, cơ chế thu hồi sau death, map pin và quy tắc cổng tiến trình.

## Hoãn

- 12 loại vũ khí và 12 moveset riêng.
- 12 Wing Assist riêng.
- 12 traversal mutation riêng theo Echo.
- Weapon reach/damage tier và Amplitude Ascension.
- Phản ứng không được liệt kê, tự hồi Air-Dash, hồi máu không giới hạn, phản ứng toàn màn hình hoặc vòng lặp khống chế vô hạn.
- Phím gán cuối cùng, đặc tả movement toàn phần, frame data animation, bảng dữ liệu từng loại địch và kịch bản encounter cụ thể cho từng Boss.

## Kiểm soát thay đổi

Mọi thay đổi trong tương lai đối với giá trị phản ứng, thời gian dấu, quy tắc Break, phần thưởng Parry, kinh tế Harmony hoặc hiệu ứng Sacred Chord phải cập nhật đồng thời bảng GDD liên quan và test triển khai. GDD là hợp đồng gameplay; code không được suy diễn hiệu ứng chưa ghi từ tên gọi hoặc VFX.
