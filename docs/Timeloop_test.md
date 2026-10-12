
## 6. Minimum validation checklist

- [ ] Fake-clock test: đúng 120 tick mỗi giây simulation khi không clamp/drop.
- [ ] Render ở 30/60/75/120/144/165/240 FPS: tick count phụ thuộc elapsed time, không phụ thuộc render-frame count.
- [ ] Ngẫu nhiên frame delta và hitch 50–500 ms: catch-up bounded, dropped ticks được ghi nhận, `alpha` luôn trong `[0, 1)`.
- [ ] Nhấn rồi nhả jump giữa hai fixed ticks: không mất edge hoặc trigger lặp.
- [ ] Focus loss, pause, suspend/resume: không có movement bị kẹt hoặc bước nhảy thời gian.
- [ ] Teleport/respawn/scene transition: không có interpolation streak.
- [ ] Time scale 0/0.25/0.5/1/2: `dt` giữ nguyên, tốc độ gameplay đúng policy.
- [ ] Telemetry: raw/clamped delta, steps/frame, dropped ticks, accumulator remainder, `alpha`, CPU simulation/render và thời gian block trong `Present()`.
