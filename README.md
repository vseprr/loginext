# LogiNext

**Userspace Linux daemon that gives Logitech mice the per-control customization Logitech Options+ offers on Windows and macOS — starting with a polished, gesture-aware tab switcher on the MX Master 3S thumb wheel.**

Linux'ta Logitech'in resmi yazılımı yok. `solaar` cihazı yönetir, `libinput` scroll'u işler, ama hiçbiri "şu butona/tekerleğe şu eylemi bağla, sadece Firefox'tayken farklı davransın" demeyi sunmuyor. LogiNext bu boşluğu dolduruyor: düşük gecikmeli bir C++ daemon + ayrı bir UI. Cihazdan event'leri exclusive grab'le alır, sizin tanımladığınız preset'e göre dönüştürür ve `uinput` ile sisteme geri enjekte eder.

Proje iki aşamada ilerliyor:

- **Phase 1 (tamamlandı):** MX Master 3S thumb wheel'ını akıcı bir `Ctrl+Tab` / `Ctrl+Shift+Tab` gezgine çeviren heuristik motor. Hız algılayan, hayalet-event filtreli, 3 hassasiyet modlu.
- **Phase 2 (sıradaki):** Neumorphism dark temalı konfigürasyon UI'si. Her kontrolü istediğiniz "preset"e bağlayın, isterseniz yalnızca seçtiğiniz uygulamada çalışsın.

---

## Özellikler (Phase 1)

- **Otomatik cihaz tespiti** — Bolt receiver (`046d:b034`) ya da USB (`046d:c548`).
- **Exclusive grab** — thumb wheel event'leri sadece daemon'a düşer, başka kimse görmez.
- **Heuristik motor** — yavaş tek tıkta tam 1 sekme, hızlı swipe'ta pürüzsüz çoklu sekme.
  - Leaky-bucket accumulator
  - Velocity-aware dynamic threshold (fast/slow Δt lerp)
  - Idle-reset ile gesture sınırı
  - Leading-edge confirmation window (parmak wheel üstünde duruyorken oluşan 1mm hayalet hareketleri filtreler)
  - Emit cooldown + damping + pacing queue
- **3 hassasiyet modu:** `low`, `medium`, `high` — her biri bağımsız profil.
- **Config** — `~/.config/loginext/config.json` (XDG uyumlu) + CLI override.
- **Hot reload** — `SIGHUP` ile anında yeniden yükler, daemon'u durdurmaz.
- **Passthrough** — wheel dışındaki tüm mouse event'leri (tıklama, hareket, dikey scroll) sanal fareden olduğu gibi geçirilir.
- **Zero heap allocation** event loop'unda.
- **Tek binary**, harici çalışma zamanı bağımlılığı sadece `libevdev`.

---

## Gereksinimler

- Linux, `uinput` modülü (`CONFIG_INPUT_UINPUT=y` veya modül olarak yüklü).
- `libevdev` ≥ 1.13.
- CMake ≥ 3.25, Ninja, GCC 14+ ya da Clang 18+.
- Kullanıcının `/dev/input/eventX` ve `/dev/uinput`'a erişimi. Üretim için:
  - `input` grubuna üyelik,
  - ya da udev kuralıyla `uinput` grubuna yetki.
  - Test için `sudo ./build/loginext` yeterli.

Arch / CachyOS:

```bash
sudo pacman -S --needed cmake ninja libevdev gcc pkgconf
```

---

## Kurulum

```bash
git clone https://github.com/vseprr/loginext.git
cd loginext
cmake -S . -B build -G Ninja
cmake --build build
```

Derleme `-O2 -Wall -Wextra -Wpedantic -Werror` altında temiz olmalı.

---

## Çalıştırma

```bash
sudo ./build/loginext --mode=low
```

`sudo` yalnızca /dev/input erişimi içindir; uzun vadede udev kuralı + user service önerilir (Phase 2.6).

### CLI seçenekleri

| Bayrak | Açıklama |
|---|---|
| `--mode=low\|medium\|high` | Config'deki hassasiyeti override eder. |
| `--config=<path>` | Varsayılan yerine verilen JSON'u okur. |
| `--help` | Kullanım çıktısı. |

### Config dosyası

Varsayılan yol: `$XDG_CONFIG_HOME/loginext/config.json` (yoksa `~/.config/loginext/config.json`).

```jsonc
{
    "sensitivity": "low",     // "low" | "medium" | "high"
    "invert_hwheel": true     // MX Master 3S için true tavsiye
}
```

Örnek için [config/example.json](./config/example.json).

### Hot reload

```bash
# Config'i değiştir
echo '{"sensitivity":"high"}' > ~/.config/loginext/config.json
# Daemon'a yeniden yükletmesini söyle
pkill -HUP loginext
```

Daemon'dan şöyle bir stderr satırı görürsünüz:

```
[loginext] config reloaded: mode=high invert=true
```

---

## Hassasiyet modları

| Mode | Hissiyat | Uygun senaryo |
|---|---|---|
| `low` | Çok kararlı; wheel üstündeki parmak bile tetiklemez, leading-edge 80ms'lik teyit penceresi var. | Tek tek dolaşırken, az sekme değişikliği istendiğinde. |
| `medium` | Dengeli. | Günlük kullanım. |
| `high` | Hızlı, en küçük hareketi bile yakalar. | Uzun sekme listelerinde hızlı tarama. |

Parametreler [src/config/profile.hpp](./src/config/profile.hpp) içinde `constexpr`. İleride UI'dan ayarlanacak.

---

## Nasıl çalışıyor

```
MX Master 3S thumb wheel
        │  (REL_HWHEEL)
        ▼
/dev/input/eventX  ── libevdev exclusive grab ──┐
                                                 │
                                                 ▼
                              ┌─────────────────────────────────┐
                              │  epoll event loop (single thr.) │
                              └─────────────────────────────────┘
                                      │            │
                          HWHEEL event│            │passthrough (clicks, moves, vwheel)
                                      ▼            ▼
                      ┌───────────────────────┐  ┌───────────────┐
                      │ heuristics/scroll     │  │ virtual mouse │
                      │   - accumulator       │  │ (uinput)      │
                      │   - velocity curve    │  └───────────────┘
                      │   - confirmation win  │
                      │   - cooldown          │
                      └───────────────────────┘
                                  │ ActionResult
                                  ▼
                      ┌───────────────────────┐
                      │ core/pacer            │
                      │   - ring buffer       │
                      │   - timerfd           │
                      │   - damping           │
                      └───────────────────────┘
                                  │
                                  ▼
                      ┌───────────────────────┐
                      │ virtual keyboard      │
                      │ Ctrl+Tab / Ctrl+Shift │
                      │ (uinput)              │
                      └───────────────────────┘
```

Detaylı mimari için [agents.md](./agents.md) ve [progress.md](./progress.md).

---

## Proje yapısı

```
src/
├── core/         # event loop, device grab, emitter, pacer
├── heuristics/   # scroll state + velocity engine
├── config/       # constants, profiles, CLI args, JSON loader
└── main.cpp
config/example.json
```

---

## Yol haritası

- [x] **Phase 1** — Thumb wheel → tab navigation engine
- [ ] **Phase 2** — Neumorphism (dark) UI: cihaz listesi + kontrol → preset atama + uygulama bazında kurallar
- [ ] **Phase 3** — Back/forward, gesture button, vertical wheel, mode-shift; yeni preset'ler (window switcher, volume, zoom, custom keystroke, run command)

Detaylı madde listesi için [progress.md](./progress.md).

---

## Katkı

Phase 1 kararlı. Phase 2 IPC + UI katmanları için PR'lar açık. Katkıdan önce [agents.md](./agents.md) dosyasındaki kurallara ve "no framework / no heap in hot path" ilkesine bakın.

```bash
cmake --build build           # -Werror altında temiz olmalı
./build/loginext --help
```

---

## Lisans

[MIT](./LICENSE).

---

## Yasal uyarı

LogiNext; Logitech International S.A. ile bağlantılı, desteklenen ya da onaylanan bir proje **değildir**. "Logitech", "MX Master", "Options+" tüm ilgili marka sahiplerinin tescilli markalarıdır ve burada yalnızca uyumluluk açıklaması için kullanılmıştır.
