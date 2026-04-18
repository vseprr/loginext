# LogiNext — Progress

Arch/CachyOS üzerinde çalışan, userspace'te Logitech cihazlarını Options+ benzeri bir kontrol yüzeyiyle yöneten C++ daemon'u. İlk hedef: MX Master 3S thumb wheel ile pürüzsüz sekme geçişi, sonraki hedef: her tuş ve tekerleği uygulama bazında konfigüre edilebilir yapan bir UI.

---

## Phase 1 — Thumb Wheel → Tab Navigation Engine

Tamamlandı. Gerçek cihaz üzerinde kalibre edildi.

### Device I/O
- [x] `/dev/input/*` taraması, VID/PID ile MX Master 3S otomatik tespit
- [x] `libevdev` üzerinden cihaza **exclusive grab**
- [x] `uinput` ile iki sanal cihaz: sekme için sanal klavye, passthrough için sanal fare
- [x] Tek thread, `epoll` (edge-triggered) + `timerfd` event loop'u
- [x] Sinyaller: `SIGINT` / `SIGTERM` ile düzgün kapanış, `SIGHUP` ile config reload

### Heuristic Engine
- [x] `REL_HWHEEL` için **leaky-bucket accumulator** (`ScrollState`)
- [x] **Velocity-aware dynamic threshold**: Δt'ye göre fast/slow threshold arasında lerp
- [x] **Idle reset**: `idle_reset_ns`'den uzun sessizlik → accumulator sıfır, yeni gesture
- [x] **Leading-edge confirmation window**: gesture'ın ilk event'i hemen emit edilmez; `confirmation_window_ns` içinde aynı yönde ikinci event gelirse teyit edilir → wheel üstünde duran parmağın tetiklediği tek-event hayaletler filtrelenir
- [x] **Emit cooldown**: aynı gesture içinde minimum emit aralığı
- [x] **Axis invert** (config'den; MX Master 3S için varsayılan `true`)

### Output & Pacing
- [x] `uinput` üzerinden `Ctrl+Tab` ve `Ctrl+Shift+Tab`
- [x] Ring buffer'lı pacing queue + `timerfd` (`pacing_interval_ns`)
- [x] Damping: giriş kesilince kuyrukta biriken event'leri frenle (`damping_timeout_ns`)
- [x] Tab-switching dışında kalan tüm event'ler sanal fareden passthrough

### Config Layer
- [x] 3 preset: `low` / `medium` / `high` (`Profile` struct, `constexpr`)
- [x] Flat JSON parser (bağımlılıksız, ~100 satır) → `~/.config/loginext/config.json`
- [x] CLI override: `--mode=low|medium|high`, `--config=<path>`, `--help`
- [x] `SIGHUP` → hot reload, gesture state sıfırlanır

### Build & Tooling
- [x] CMake 3.25+ / Ninja, tek binary target
- [x] `-O2 -Wall -Wextra -Wpedantic -Werror` temiz derleme
- [x] `compile_commands.json` export (IDE entegrasyonu)

---

## Phase 2 — Konfigürasyon UI'si (sıradaki)

Hedef: Logitech Options+'a benzer bir kontrol paneli. Kullanıcı:

1. Bağlı Logitech cihazları görür (şimdilik yalnızca MX Master 3S).
2. Cihaz üzerinde fiziksel bir kontrol seçer (Phase 2 için **thumb wheel**; ileride diğer butonlar/wheel'lar).
3. O kontrol için bir **action preset** atar. İlk preset: **"Navigate between tabs"** — Phase 1'de üretilen motor bu preset'in arkasındaki engine olur.
4. Preset seçilince alt kısımda ona özel parametreler açılır (tab navigation için: sensitivity `low`/`medium`/`high` ya da sürekli slider, invert axis).
5. Bu kuralı **global** veya **uygulama bazında** (ör. sadece Firefox) uygular.

### Tema
- **Neumorphism — dark variant**. Soft kabartma/yuvarlatma dilini karanlık palete uyarla: `#1e1f24`/`#262830` yüzey, `rgba(0,0,0,0.55)` iç/dış gölge + `rgba(255,255,255,0.04)` highlight, `#6c7cff` accent. Köşeler 16–24px, aktif eleman "basılmış" (inset shadow), pasif eleman "kaldırılmış" (outer shadow).

### Mimari karar — daemon ↔ UI IPC
- UI bağımsız bir process, daemon hot path'e zarar vermez.
- Transport: **Unix domain socket** (`$XDG_RUNTIME_DIR/loginext.sock`). D-Bus gibi framework yok (agents.md kuralı).
- Protokol: line-delimited JSON. Komut seti: `list_devices`, `list_controls`, `list_presets`, `get_bindings`, `set_binding`, `get_profile`, `set_profile`, `reload`.
- Daemon mevcut `Settings` yapısına "bindings" eklenir, config JSON şeması genişletilir. UI config dosyasına yazdıktan sonra daemon'a `reload` der (SIGHUP altyapısı hazır).

### Tech stack adayları
- **Tauri (Rust shell + web frontend)**: küçük binary, tek runtime, neumorphism CSS ile doğal. Tercih edilen.
- Alternatif: **Qt 6 / QML** (native ama ağır), **GTK4 + libadwaita** (GNOME-centric).
- Seçim Phase 2.1'de netleşir; `ui/` alt dizini ana repo içinde kalır (monorepo).

### Phase 2.1 — Daemon IPC katmanı
- [ ] `src/ipc/` modülü: UDS listener, epoll'e register, non-blocking accept
- [ ] JSON komut dispatch'i (loader'daki mini parser genişletilir)
- [ ] `bindings` kavramı: `(device_id, control_id) → (preset_id, preset_params, scope)` eşlemesi
- [ ] Config şeması v2: geri uyumlu, eski flat key'ler fallback olarak okunur
- [ ] Integration test: CLI client (`loginext-ctl`) ile socket'e komut yolla, cevap parse et

### Phase 2.2 — UI iskeleti
- [ ] Tech stack kararı ve `ui/` bootstrap (Tauri varsayımı)
- [ ] Neumorphism dark design tokens (CSS variables): surface, shadow, accent, radius, typography
- [ ] Ortak bileşenler: `Card`, `RaisedButton`, `PressedButton`, `Toggle`, `Slider`, `ListItem`
- [ ] UDS client: daemon'a bağlan, heartbeat, reconnection

### Phase 2.3 — Device & Control browser
- [ ] Sol kolon: bağlı cihazların listesi (MX Master 3S)
- [ ] Orta kolon: seçili cihazın kontrolleri (thumb wheel başta tek; ileride butonlar, wheel'lar)
- [ ] Sağ kolon: seçili kontrol için atanmış preset + parametreleri
- [ ] Device/control görselleri için basit SVG placeholder

### Phase 2.4 — "Navigate between tabs" preset paneli
- [ ] Preset seçici dropdown (ilk giriş: Navigate between tabs; diğerleri ileride)
- [ ] Sensitivity seçimi: 3 segmentli toggle (Low / Medium / High) + opsiyonel sürekli slider
- [ ] Invert axis toggle'ı
- [ ] "Live preview": UI açıkken üretilen event'leri göster (debug overlay)

### Phase 2.5 — Scope: global vs per-app
- [ ] Kural kapsamı seçici: "All applications" | "Only in…"
- [ ] Aktif pencere tespiti: Wayland için `wlr-foreign-toplevel` veya `ext-foreign-toplevel-list`, X11 için `_NET_ACTIVE_WINDOW`. Compositor desteği değişken — fallback: kullanıcı uygulamayı manuel seçer (executable adı / WM_CLASS).
- [ ] Daemon tarafı: aktif pencereye göre binding lookup (hot path'e eklenirken cache + invalidation şart)
- [ ] Kural çakışması: en spesifik eşleşme kazanır (per-app > global).

### Phase 2.6 — Polishing
- [ ] Import/export: ayar dosyasını başka makineye taşıma
- [ ] Autostart: `~/.config/systemd/user/loginext.service` şablonu (user unit; uinput izinleri için udev kuralı da gerekebilir)
- [ ] Tray / indicator (opsiyonel)

---

## Phase 3 — Diğer kontroller (gelecek)

Phase 2 biter bitmez aşağıdaki kontroller `bindings` sistemine dahil edilecek. Her biri ayrı bir preset ailesi gerektirir:

- Back / Forward butonları
- Gesture button (thumb altı)
- Dikey scroll wheel (SmartShift toggle entegrasyonu araştırılacak)
- Mode-shift button (cihaz tarafında değişim mümkünse)

Preset adayları: "Window switcher", "Workspace switch", "Volume", "Zoom", "Custom keystroke", "Run command".

---

## Tasarım notları — kalıcı kurallar

- Hot path'te heap allocation yok; hâlâ geçerli.
- UI sadece config dosyasına yazar + daemon'a `reload` der. Hot path'e UI'dan doğrudan erişim yok.
- Per-app lookup daemon hot path'inde olacak, bu yüzden O(1) hash map + pencere değişimine subscribe şart.
- Her yeni preset için: `heuristics/` altında bir state + `core/emitter` üzerinden bir çıktı. Diğer her şey `bindings` ve `ipc`'ten gelir.
