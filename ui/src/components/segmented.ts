export interface SegmentedOption<T extends string> {
  value: T;
  label: string;
}

export interface SegmentedOptions<T extends string> {
  options: readonly SegmentedOption<T>[];
  value?: T;
  onChange?: (next: T) => void;
}

// Pill-shaped 3-segment selector (Low / Medium / High, etc).
// Selection state lives on the DOM (`aria-selected`), not in a closure —
// external sync via setAttribute() must be reflected on the next click.
export function segmented<T extends string>(opts: SegmentedOptions<T>): HTMLElement {
  const el = document.createElement("div");
  el.className = "segmented";
  el.setAttribute("role", "radiogroup");

  for (const o of opts.options) {
    const btn = document.createElement("button");
    btn.type = "button";
    btn.className = "segmented__option";
    btn.setAttribute("role", "radio");
    btn.textContent = o.label;
    btn.dataset.value = o.value;
    btn.setAttribute("aria-selected", String(o.value === opts.value));
    btn.addEventListener("click", () => {
      if (btn.getAttribute("aria-selected") === "true") return;
      for (const child of Array.from(el.children)) {
        child.setAttribute("aria-selected", String(child === btn));
      }
      opts.onChange?.(o.value);
    });
    el.appendChild(btn);
  }
  return el;
}
