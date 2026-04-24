export interface CardOptions {
  title?: string;
}

// Raised neumorphic card. Pass `title` for the uppercase header.
export function card({ title }: CardOptions = {}): HTMLElement {
  const el = document.createElement("section");
  el.className = "card";
  if (title) {
    const h = document.createElement("h2");
    h.className = "card__title";
    h.textContent = title;
    el.appendChild(h);
  }
  return el;
}
