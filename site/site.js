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
      this.field("page", { boost: 4 });
      this.field("text");
      docs.forEach((d, i) => this.add({ i, title: d.title, page: d.page, text: d.text }));
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
      const results = hits.map((h) => ({ ...docs[h.ref], score: h.score, terms: Object.keys(h.matchData.metadata) }));
      // each page's own text (its start, with "In short"), as the lead under its title
      const leads = {};
      for (const r of results) leads[r.location.split("#")[0]] ??= docs.find((d) => d.location === r.location.split("#")[0])?.text;
      postMessage({ id, results, leads });
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
          data.error ? reject(new Error(data.error)) : resolve({ id, results: data.results, leads: data.leads });
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
    // Up to three passages of about 350 characters around the matches, nearby matches merged into
    // one passage; with no match in the text (a match in the title only), its start.
    const passages = (text, rx) => {
      if (!text) return [];
      const spans = [];
      for (const m of text.matchAll(rx)) {
        let a = Math.max(0, m.index - 140);
        const b = Math.min(text.length, m.index + m[0].length + 200);
        const last = spans[spans.length - 1];
        if (last && a <= last[1] && b - last[0] <= 480) last[1] = b;
        else spans.push([last ? Math.max(a, last[1]) : a, b]);
      }
      if (!spans.length) spans.push([0, Math.min(text.length, 350)]);
      return spans.slice(0, 3).map(([a, b]) => {
        if (a > 0) a = text.indexOf(" ", a) + 1 || a;
        const sp = text.lastIndexOf(" ", b);
        if (b < text.length && sp > a) b = sp;
        return highlight((a > 0 ? "\u2026" : "") + text.slice(a, b) + (b < text.length ? " \u2026" : ""), rx, "r-ex");
      });
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
    const setActive = (o, scroll = true) => {
      list.querySelectorAll("[aria-selected=true]").forEach((x) => x.setAttribute("aria-selected", "false"));
      o.setAttribute("aria-selected", "true");
      q.setAttribute("aria-activedescendant", o.id);
      if (!scroll) return;
      // keep it in view by scrolling the list only, never the page
      const r = o.getBoundingClientRect();
      const lr = list.getBoundingClientRect();
      // (its top first, when it is taller than the list)
      if (r.top < lr.top || r.height > lr.height - 16) list.scrollTop += r.top - lr.top - 8;
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
    // A result on another page loads it. One on this page (a section of it) only moves the page,
    // so the dialog is closed here and focus goes to that section: otherwise the dialog stayed open
    // with focus lost, and the arrow keys scrolled the results instead of moving between them.
    const samePage = (u) => u.origin === location.origin &&
      u.pathname.replace(/index\.html$/, "") === location.pathname.replace(/index\.html$/, "");
    const pick = (o) => {
      if (!o) return;
      const u = new URL(o.dataset.url);
      if (!samePage(u)) return void (location.href = u.href);
      dlg.close();
      const t = (u.hash && document.getElementById(decodeURIComponent(u.hash.slice(1)))) ||
        document.querySelector("main h1") || document.querySelector("main");
      if (u.hash !== location.hash) history.pushState(null, "", u.hash || u.pathname);
      if (!t) return;
      if (!t.hasAttribute("tabindex")) t.setAttribute("tabindex", "-1");
      t.focus({ preventScroll: true });
      t.scrollIntoView({ block: "start" });
      dispatchEvent(new HashChangeEvent("hashchange"));
    };

    const run = async () => {
      const term = q.value.trim();
      if (!term) return clear();
      const reply = await ask(term).catch(() => null);
      if (!reply) return clear("Search is not available right now. Use the tabs or the menu.");
      if (reply.id !== seq) return; // a newer query is on its way
      const hits = reply.results;
      if (!hits.length) return clear(`No matching documents for “${term}”.`);
      const typed = term.toLowerCase().split(/[\s\-]+/).filter(Boolean);
      // results grouped by page; the page with the most and best matches first
      const pages = new Map();
      for (const h of hits) {
        const url = h.location.split("#")[0];
        if (!pages.has(url)) pages.set(url, { title: h.page, top: null, subs: [], words: new Set(typed), score: 0 });
        const g = pages.get(url);
        h.terms.forEach((t) => g.words.add(t));
        g.score += h.score;
        if (h.location.includes("#")) g.subs.push(h);
        else g.top = h;
      }
      n = 0;
      const groups = [...pages].sort((a, b) => b[1].score - a[1].score).map(([url, g], k) => {
        const rx = matcher([...g.words]);
        // the page's own entry: an icon, its title and, if it matched, a passage before its first
        // section; then its sections, the best first
        const title = highlight(g.title, rx, "r-title");
        title.id = `search-g-${k}`;
        const icon = el("span", { className: "r-icon" });
        icon.innerHTML = ICON;
        const page = opt("r-page", url, icon, title);
        page.append(...passages(g.top ? g.top.text : reply.leads[url], rx));
        const sec = g.subs.map((s) => opt("r-sub", s.location, highlight(s.title, rx, "r-title"), ...passages(s.text, rx)));
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
      status.textContent = `${hits.length} ${hits.length === 1 ? "match" : "matches"} on ${pages.size} ${pages.size === 1 ? "page" : "pages"}`;
    };

    // The query and the results stay when the dialog closes (Escape, Close, a click outside), as
    // in MkDocs: the header's search shows the query, and reopening shows the same results. Only
    // the clear button empties it.
    const clearBtn = dlg.querySelector(".search-clear");
    const label = opener.querySelector(".search-label");
    const field = bar.querySelector(".search-field");
    const sync = () => {
      const v = q.value.trim();
      field.value = q.value;
      label.textContent = v || "Search";
      opener.setAttribute("aria-label", v ? `Search: ${v}` : "Search");
      opener.classList.toggle("has-query", !!v);
      clearBtn.hidden = !q.value;
    };
    const show = (caret = q.value.length) => {
      if (!dlg.open) dlg.showModal();
      q.focus();
      q.setSelectionRange(caret, caret);
      ask("").catch(() => {}); // start building the index
    };
    opener.addEventListener("click", () => show());
    // On a wide screen the header has a real search field. Typing in it opens the dialog at once,
    // with the text and the caret moved to the dialog's field (no keystroke is lost); ↓ or Enter
    // open it too, with the results of the query so far.
    field.hidden = false;
    field.addEventListener("input", () => {
      q.value = field.value;
      if (!q.value) return clear(), sync();
      show(field.selectionStart);
      q.dispatchEvent(new Event("input"));
    });
    field.addEventListener("keydown", (e) => {
      if (e.key === "ArrowDown" || e.key === "Enter") e.preventDefault(), show();
    });
    // A click opens it at once. Tab only moves focus to the field: opening a dialog on focus alone
    // would be a change of context (WCAG 3.2.1). Shift+Tab from the dialog's field closes it and
    // goes on backwards, to the item before the header field.
    field.addEventListener("click", () => show());
    q.addEventListener("keydown", (e) => {
      if (e.key !== "Tab" || !e.shiftKey || !field.checkVisibility()) return;
      e.preventDefault();
      dlg.close();
      const stops = [...bar.querySelectorAll("a, input, button")].filter((x) => x.checkVisibility());
      stops[stops.indexOf(field) - 1]?.focus();
    });
    // "/" or "s" opens search, as in MkDocs, when focus is not in a text field
    document.addEventListener("keydown", (e) => {
      if ((e.key === "/" || e.key === "s") && !e.ctrlKey && !e.metaKey && !e.altKey && !dlg.open &&
          !e.target.closest("input, textarea, select, [contenteditable]")) {
        e.preventDefault();
        show();
      }
    });
    closeBtn.addEventListener("click", () => dlg.close());
    dlg.addEventListener("close", sync);
    clearBtn.addEventListener("click", () => {
      q.value = "";
      clear();
      sync();
      q.focus();
    });

    q.addEventListener("input", () => {
      sync();
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
    // the pointer and the arrow keys move one and the same active result
    list.addEventListener("mousemove", (e) => {
      const o = e.target.closest("[role=option]");
      if (o && o !== current()) setActive(o, false);
    });
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
