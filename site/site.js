/* PSX RiPPER site: progressive enhancement only. Without this script every page still reads and
   navigates in full: the tabs are all shown, "On this page" is a plain list of links, and the
   search button stays hidden. With it: the tabs fold into a Menu button where they do not fit,
   "On this page" marks the section being read, and search works (lunr, loaded on first use). */

// In a worker (started below by the search) this file is the search engine: on the first query it
// loads lunr (vendor/, 2.3.9, the engine MkDocs uses) and search_index.json (written by
// build.py), builds the index the way MkDocs does (title weighted ten times) and answers each
// query with the matching entries, best first.
if (typeof document === "undefined") {
  let index = null;
  let docs = [];
  const build = async () => {
    const r = await fetch("search_index.json");
    if (!r.ok) throw new Error(`search_index.json: HTTP ${r.status}`);
    docs = (await r.json()).docs;
    importScripts("vendor/lunr-2.3.9.min.js");
    return lunr(function () {
      this.ref("i");
      this.field("title", { boost: 10 });
      this.field("text");
      docs.forEach((d, i) => this.add({ i, title: d.title, text: d.text }));
    });
  };
  onmessage = async ({ data: { id, q } }) => {
    try {
      index ||= await build();
      // every word as typed (stemmed) and as the start of a longer word, like MkDocs
      const words = lunr.tokenizer(q).map(String).filter(Boolean);
      const hits = words.length
        ? index.query((query) =>
            words.forEach((w) => {
              query.term(w, { boost: 10 });
              query.term(w, { usePipeline: false, wildcard: lunr.Query.wildcard.TRAILING });
            }))
        : [];
      postMessage({ id, results: hits.map((h) => ({ ...docs[h.ref], terms: Object.keys(h.matchData.metadata) })) });
    } catch (err) {
      postMessage({ id, error: String(err) });
    }
  };
} else (() => {
  const root = document.documentElement;
  const bar = document.querySelector(".bar");
  if (!bar) return;
  const script = document.currentScript.src;
  const base = new URL(".", script);

  // The sticky header's real height, so anchors and focus land below it (scroll-margin-top).
  const headH = () => (getComputedStyle(bar).position === "sticky" ? bar.offsetHeight : 0);
  const setHead = () => root.style.setProperty("--head-h", headH() + "px");
  addEventListener("resize", setHead);
  new ResizeObserver(setHead).observe(bar);

  // A link in a table that scrolls sideways is brought fully into view when it gets focus
  // (Chrome leaves it half outside the table), WCAG 2.4.12.
  document.addEventListener("focusin", (e) => {
    if (e.target.matches?.(".table-wrap a")) e.target.scrollIntoView({ block: "nearest", inline: "nearest" });
  });

  // ---------------------------------------------------------------- the Menu button
  const btn = bar.querySelector(".menu-btn");
  const menu = document.getElementById("menu");
  const isOpen = () => btn.getAttribute("aria-expanded") === "true";
  const setOpen = (open) => btn.setAttribute("aria-expanded", String(open));
  btn.hidden = false;
  btn.addEventListener("click", () => setOpen(!isOpen()));
  // Escape closes the menu and gives focus back to the button; focus is never trapped
  document.addEventListener("keydown", (e) => {
    if (e.key !== "Escape" || e.defaultPrevented || !isOpen() || !btn.offsetParent) return;
    if (menu.contains(document.activeElement) || document.activeElement === btn) {
      setOpen(false);
      btn.focus();
    }
  });
  document.addEventListener("click", (e) => {
    if (isOpen() && !bar.contains(e.target) && !e.target.closest("dialog")) setOpen(false);
  });

  // ---------------------------------------------------------------- search
  // lunr, in a worker running this same file, finds the entries of search_index.json (one per page and per
  // section). The results show in a native modal <dialog>, which keeps focus inside, closes on
  // Escape and gives focus back to the button that opened it. The field is a combobox (ARIA 1.2):
  // it keeps focus while ↑ and ↓ move the active result (aria-activedescendant) and Enter opens
  // it. Each page's results are a listbox; the rest of a page's matches are in a native
  // <details> ("N more on this page"), reached with Tab. The page behind does not scroll.
  const opener = bar.querySelector(".search-open");
  const dlg = document.querySelector(".search-dlg");
  if (opener && dlg && dlg.showModal) {
    const q = dlg.querySelector("input");
    const list = dlg.querySelector(".search-list");
    const status = dlg.querySelector("[role=status]");
    const closeBtn = dlg.querySelector(".search-close");
    const SHOWN = 3; // sections per page before "N more on this page"
    let worker = null;
    let seq = 0;
    let timer;
    let n = 0;
    opener.hidden = false;

    // the index is built in a worker the first time search is opened (lunr, search_index.json)
    const ask = (text) =>
      new Promise((resolve, reject) => {
        worker ||= new Worker(script);
        const id = ++seq;
        const on = ({ data }) => {
          if (data.id !== id) return;
          worker.removeEventListener("message", on);
          data.error ? reject(new Error(data.error)) : resolve({ id, results: data.results });
        };
        worker.addEventListener("message", on);
        worker.addEventListener("error", reject, { once: true });
        worker.postMessage({ id, q: text });
      });

    const el = (tag, props = {}, ...kids) => {
      const e = Object.assign(document.createElement(tag), props);
      e.append(...kids);
      return e;
    };
    const ICON = '<svg viewBox="0 0 16 16" aria-hidden="true"><path d="M3.5 1.5h6l3 3v10h-9z" fill="none" stroke="currentColor" stroke-width="1.3" stroke-linejoin="round"/><path d="M9.5 1.5v3h3M5.5 8h5M5.5 10.5h5" fill="none" stroke="currentColor" stroke-width="1.3"/></svg>';
    const esc = (w) => w.replace(/[.*+?^${}()|[\]\\]/g, "\\$&");
    // the searched words and the stems lunr matched, each as the start of a word
    const matcher = (words) =>
      words.length ? new RegExp(`(?<![\\p{L}\\p{N}])(?:${words.map(esc).join("|")})[\\p{L}\\p{N}_-]*`, "giu") : null;
    const highlight = (text, rx, cls) => {
      const s = el("span", { className: cls });
      if (!rx) return (s.textContent = text), s;
      let at = 0;
      for (const m of text.matchAll(rx)) {
        s.append(text.slice(at, m.index), el("mark", { textContent: m[0] }));
        at = m.index + m[0].length;
      }
      s.append(text.slice(at));
      return s;
    };
    // about 160 characters of the text around the first match
    const excerpt = (text, rx) => {
      if (!text) return null;
      rx.lastIndex = 0;
      const m = rx.exec(text);
      rx.lastIndex = 0;
      let start = m ? Math.max(0, m.index - 60) : 0;
      if (start > 0) start = text.indexOf(" ", start) + 1 || start;
      let end = Math.min(text.length, start + 170);
      if (end < text.length) end = text.lastIndexOf(" ", end) > start ? text.lastIndexOf(" ", end) : end;
      return highlight((start > 0 ? "…" : "") + text.slice(start, end) + (end < text.length ? " …" : ""), rx, "r-ex");
    };
    const opt = (cls, url, ...kids) => {
      const o = el("li", { id: `search-opt-${n++}`, role: "option", className: cls }, ...kids);
      o.setAttribute("aria-selected", "false");
      o.dataset.url = new URL(url || "./", base).href;
      return o;
    };

    // the options that can be seen: not those in a closed "more on this page"
    const options = () => [...list.querySelectorAll("[role=option]")].filter((o) => o.checkVisibility());
    const current = () => document.getElementById(q.getAttribute("aria-activedescendant"));
    const setActive = (o) => {
      current()?.setAttribute("aria-selected", "false");
      o.setAttribute("aria-selected", "true");
      q.setAttribute("aria-activedescendant", o.id);
      // keep it in view by scrolling the list only, never the page
      const r = o.getBoundingClientRect();
      const lr = list.getBoundingClientRect();
      if (r.top < lr.top) list.scrollTop -= lr.top - r.top + 8;
      else if (r.bottom > lr.bottom) list.scrollTop += r.bottom - lr.bottom + 8;
    };
    const clear = (msg = "") => {
      seq++;
      list.replaceChildren();
      list.hidden = true;
      q.setAttribute("aria-expanded", "false");
      q.removeAttribute("aria-activedescendant");
      status.textContent = msg;
    };
    const pick = (o) => o && (location.href = o.dataset.url);

    const run = async () => {
      const term = q.value.trim();
      if (!term) return clear();
      const reply = await ask(term).catch(() => null);
      if (!reply) return clear("Search is not available right now. Use the tabs or the menu.");
      if (reply.id !== seq) return; // a newer query is on its way
      const hits = reply.results;
      if (!hits.length) return clear(`No matching documents for “${term}”.`);
      const typed = term.toLowerCase().split(/[\s\-]+/).filter(Boolean);
      // results grouped by page, the pages in the order of their best match
      const pages = new Map();
      for (const h of hits) {
        const url = h.location.split("#")[0];
        if (!pages.has(url)) pages.set(url, { title: h.page, top: null, subs: [], words: new Set(typed) });
        const g = pages.get(url);
        h.terms.forEach((t) => g.words.add(t));
        if (h.location.includes("#")) g.subs.push(h);
        else g.top = h;
      }
      n = 0;
      const groups = [...pages].map(([url, g], k) => {
        const rx = matcher([...g.words]);
        // the page's own entry: an icon, its title and, if it matched, a passage before its first
        // section; then its sections, the best first
        const title = highlight(g.title, rx, "r-title");
        title.id = `search-g-${k}`;
        const icon = el("span", { className: "r-icon" });
        icon.innerHTML = ICON;
        const page = opt("r-page", url, icon, title);
        if (g.top) page.append(excerpt(g.top.text, rx) || "");
        const sec = g.subs.map((s) => opt("r-sub", s.location, highlight(s.title, rx, "r-title"), excerpt(s.text, rx) || ""));
        const box = (...kids) => {
          const ul = el("ul", { role: "listbox" }, ...kids);
          ul.setAttribute("aria-labelledby", title.id);
          return ul;
        };
        const group = el("section", { className: "r-group" }, box(page, ...sec.slice(0, SHOWN)));
        if (sec.length > SHOWN)
          group.append(el("details", {}, el("summary", { textContent: `${sec.length - SHOWN} more on this page` }), box(...sec.slice(SHOWN))));
        return group;
      });
      list.replaceChildren(...groups);
      list.scrollTop = 0;
      list.hidden = false;
      q.setAttribute("aria-expanded", "true");
      q.removeAttribute("aria-activedescendant");
      status.textContent = `${hits.length} matching ${hits.length === 1 ? "document" : "documents"}`;
    };

    opener.addEventListener("click", () => {
      dlg.showModal();
      q.select();
      ask("").catch(() => {}); // start building the index
    });
    closeBtn.addEventListener("click", () => dlg.close());

    q.addEventListener("input", () => {
      clearTimeout(timer);
      timer = setTimeout(run, 150);
    });
    q.addEventListener("keydown", (e) => {
      const opts = options();
      if (e.key === "ArrowDown" || e.key === "ArrowUp") {
        e.preventDefault();
        if (!opts.length) return;
        const i = opts.indexOf(current());
        setActive(opts[e.key === "ArrowDown" ? (i + 1) % opts.length : i > 0 ? i - 1 : opts.length - 1]);
      } else if (e.key === "Enter") {
        e.preventDefault();
        clearTimeout(timer);
        if (opts.length) pick(current() || opts[0]);
        else run();
      }
    });
    list.addEventListener("click", (e) => pick(e.target.closest("[role=option]")));
  }

  // ---------------------------------------------------------------- "On this page"
  const toc = document.querySelector("nav.toc");
  if (!toc || !("IntersectionObserver" in window)) return;
  const links = [...toc.querySelectorAll('a[href^="#"]')];
  const heads = links.map((a) => document.getElementById(decodeURIComponent(a.hash.slice(1))));
  let current = null;
  const mark = (a) => {
    if (a === current) return;
    current?.removeAttribute("aria-current");
    current = a || null;
    if (!a) return;
    a.setAttribute("aria-current", "true");
    // in the sticky column, keep the marked entry in view (scrolls the column only)
    if (toc.scrollHeight > toc.clientHeight + 1) {
      const r = a.getBoundingClientRect();
      const t = toc.getBoundingClientRect();
      if (r.top < t.top || r.bottom > t.bottom) toc.scrollTop += r.top - t.top - t.height / 3;
    }
  };
  // the section being read is the last one whose heading has passed under the header
  const update = () => {
    const line = headH() + 24;
    let i = -1;
    heads.forEach((h, k) => {
      if (h && h.getBoundingClientRect().top <= line) i = k;
    });
    // at the end of the page a short last section cannot reach the line: a followed link wins
    if (innerHeight + scrollY >= root.scrollHeight - 2) {
      const t = heads.indexOf(document.getElementById(decodeURIComponent(location.hash.slice(1))));
      if (t > i) i = t;
    }
    mark(links[i]);
  };
  // Every block of the page is watched, not only the headings: a long jump (End, a scroll bar
  // drag) can pass from one long section to the next with no heading in view.
  const blocks = document.querySelectorAll(".doc-body > *, .doc-body > section > *");
  let io;
  const watch = () => {
    io?.disconnect();
    io = new IntersectionObserver(update, { rootMargin: `-${headH() + 24}px 0px 0px 0px` });
    blocks.forEach((b) => io.observe(b));
  };
  watch();
  addEventListener("resize", watch);
  addEventListener("hashchange", update);
})();
