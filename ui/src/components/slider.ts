export interface SliderOptions {
  min?: number;
  max?: number;
  value?: number;
  step?: number;
  onChange?: (next: number) => void;
}

// Pressed track + raised thumb. Continuous drag; no snapping unless step > 1.
export function slider(opts: SliderOptions = {}): HTMLElement {
  const min = opts.min ?? 0;
  const max = opts.max ?? 100;
  const step = opts.step ?? 1;
  let value = clamp(opts.value ?? (min + max) / 2, min, max);

  const el = document.createElement("div");
  el.className = "slider";
  el.setAttribute("role", "slider");
  el.setAttribute("aria-valuemin", String(min));
  el.setAttribute("aria-valuemax", String(max));

  const track = document.createElement("div");
  track.className = "slider__track";
  const fill = document.createElement("div");
  fill.className = "slider__fill";
  track.appendChild(fill);

  const thumb = document.createElement("div");
  thumb.className = "slider__thumb";

  el.appendChild(track);
  el.appendChild(thumb);

  const render = () => {
    const pct = ((value - min) / (max - min)) * 100;
    el.style.setProperty("--fill", `${pct}%`);
    el.setAttribute("aria-valuenow", String(value));
  };
  render();

  const pick = (clientX: number) => {
    const rect = el.getBoundingClientRect();
    const pct = clamp((clientX - rect.left) / rect.width, 0, 1);
    const raw = min + pct * (max - min);
    const stepped = Math.round(raw / step) * step;
    const next = clamp(stepped, min, max);
    if (next !== value) {
      value = next;
      render();
      opts.onChange?.(value);
    }
  };

  let dragging = false;
  el.addEventListener("pointerdown", (e) => {
    dragging = true;
    el.setPointerCapture(e.pointerId);
    pick(e.clientX);
  });
  el.addEventListener("pointermove", (e) => {
    if (dragging) pick(e.clientX);
  });
  el.addEventListener("pointerup", (e) => {
    dragging = false;
    el.releasePointerCapture(e.pointerId);
  });

  return el;
}

function clamp(n: number, lo: number, hi: number): number {
  return Math.max(lo, Math.min(hi, n));
}
