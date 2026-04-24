export interface ToggleOptions {
  checked?: boolean;
  onChange?: (next: boolean) => void;
}

// Accessible neumorphic toggle. `aria-checked` drives the active styling.
export function toggle(opts: ToggleOptions = {}): HTMLElement {
  const el = document.createElement("button");
  el.type = "button";
  el.className = "toggle";
  el.setAttribute("role", "switch");

  const thumb = document.createElement("span");
  thumb.className = "toggle__thumb";
  el.appendChild(thumb);

  const set = (next: boolean) => {
    el.setAttribute("aria-checked", String(next));
  };
  set(opts.checked ?? false);

  el.addEventListener("click", () => {
    const next = el.getAttribute("aria-checked") !== "true";
    set(next);
    opts.onChange?.(next);
  });
  return el;
}
