#!/usr/bin/env python3
"""Builds the project site into _site/.

The hand-written parts live in site/: the home page (home.html), the page shell
(page.html), the style sheet and the images. Everything that changes with a
release is read from the repository at build time: the version from version.txt,
the changelog from CHANGELOG.md and the docs from docs/*.md.

    pip install -r site/requirements.txt
    python3 site/build.py            # writes _site/
"""

import html
import posixpath
import re
import shutil
import sys
from pathlib import Path

import markdown

ROOT = Path(__file__).resolve().parent.parent
SITE = ROOT / "site"
OUT = ROOT / "_site"
REPO = "https://github.com/bjornwiberg/psx-ripper"

# The docs pages in nav order: source file, page name, short nav label, and a plain-language
# summary shown under the page's title (WCAG 3.1.5, reading level).
DOCS = [
    ("user-guide.md", "user-guide", "User guide",
     "Install the app on a PS3 with custom firmware, start it, put in a PS1 disc and press "
     "✕. The app copies the disc to files on the hard disk and checks that the copy is exact. "
     "This page explains the screen and every step, from installing to playing your copies."),
    ("configuration.md", "configuration", "Configuration",
     "All settings live in one text file on the PS3. You can change them in the app, or edit the "
     "file while the app is closed. This page lists every setting, and explains the remote view, "
     "the notifications (webhooks) and the Redump check."),
    ("mcp.md", "mcp", "MCP",
     "MCP lets an AI agent on your computer use the app over your home network, the same way you "
     "would with the pad. It is off until you turn it on. This page shows how to turn it on, how "
     "to connect an agent, and every command (tool) the agent can use."),
    ("faq.md", "faq", "FAQ",
     "Short answers to common questions: which consoles work, why a copy may not match Redump, "
     "which USB drives work, where the copies go, and how to get help."),
    ("how-it-works.md", "how-it-works", "How it works",
     "The PS3 normally does not let apps read a disc's raw data. While it copies a disc, the app "
     "lifts that block in the console's memory, reads every sector, checks it, and puts the block "
     "back. For USB drives it has its own driver. You do not need this page to use the app."),
]

# Words that may be new to a reader (WCAG 3.1.3 unusual words, 3.1.4 abbreviations). The first
# use on each page links to its entry on the glossary page: id, term, pattern, definition.
GLOSSARY = [
    ("bit-perfect", "Bit-perfect", r"\bbit-perfect\b",
     "An exact copy: every bit of the copy is the same as on the disc."),
    ("cobra", "Cobra", r"\bCobra\b",
     "A part of PS3 custom firmware that mounts disc images, so a copied game starts as if its "
     "disc were in the drive."),
    ("cfw", "Custom firmware (CFW)", r"\bCFW\b|\bcustom firmwares?\b",
     "Modified PS3 system software that can run homebrew apps such as PSX RiPPER. Evilnat is "
     "one."),
    ("edc-ecc", "EDC/ECC", r"\bEDC/ECC\b",
     "Error detection code and error correction code: checksums stored in every CD-ROM data "
     "sector. They show whether a sector was read correctly, and can fix small read errors."),
    ("homebrew", "Homebrew", r"\bhomebrew\b",
     "Software made by hobbyists for a console, not by the console's maker."),
    ("libcrypt", "LibCrypt", r"\bLibCrypt\b",
     "A copy protection on some European (PAL) PlayStation discs: some sectors carry "
     "deliberately wrong subchannel data. A copy without that data stops working partway "
     "through the game. See SBI."),
    ("lv1", "lv1", r"\blv1\b",
     "The PS3's hypervisor, the layer under the system software that controls the hardware. "
     "While it copies from the internal drive, the app changes four instructions of it in "
     "memory so it may read raw sectors. A restart undoes the change."),
    ("mcp", "MCP (Model Context Protocol)", r"(?<!\[)\bMCP\b(?!\])",
     "An open standard that lets an AI agent, a program built on a language model, use the tools "
     "of another program. In PSX RiPPER it lets an agent read the screen and start copies."),
    ("md5", "MD5", r"\bMD5s?\b",
     "A checksum: a 32-character fingerprint of a file. Two files with the same MD5 are, in "
     "practice, identical. Redump lists the MD5 of every track."),
    ("pal", "PAL", r"\bPAL\b",
     "The European TV standard. PAL discs are the European releases."),
    ("ps3mapi", "PS3MAPI (PS3 Manager API)", r"\bPS3MAPI\b",
     "An interface in custom firmware that lets apps read and change the console's memory. "
     "PSX RiPPER uses it for the lv1 change."),
    ("pregap", "Pregap", r"\bpregaps?\b",
     "The gap stored on a CD before a track starts. An exact copy has to keep it."),
    ("redump", "Redump", r"\bRedump\b",
     "redump.org, a database of checksums of exact disc copies, each checked by people who own "
     "the disc. A copy whose checksums match Redump is an exact copy."),
    ("sbi", "SBI (.sbi file)", r"\bSBI\b",
     "A small file that holds a LibCrypt disc's protection data (its altered subchannel), so "
     "Cobra or an emulator can play the copy."),
    ("sector", "Sector", r"\bsectors?\b",
     "The smallest piece of data on a CD: 2352 bytes. A PS1 disc holds up to about 330,000."),
    ("subchannel", "Subchannel (Q subchannel)", r"\b(?:Q )?subchannel\b",
     "Extra data stored beside every CD sector that says where on the disc the sector is. "
     "LibCrypt hides its protection there."),
    ("superdrive", "SuperDrive", r"\bSuperDrives?\b",
     "Apple's slim USB DVD drive (model A1379). The app can copy from two of them at once, "
     "besides the PS3's own drive."),
    ("webhook", "Webhook", r"\bwebhooks?\b",
     "A web address the app sends a short message to when something happens, for example when a "
     "copy is done. A home automation system can turn it into a phone notification."),
    ("webman", "webMAN MOD", r"\bwebMAN(?: MOD)?\b",
     "A popular PS3 plugin for custom firmware that mounts images, lists games and installs "
     "packages."),
    ("xmb", "XMB (XrossMediaBar)", r"\bXMB\b",
     "The PS3's home menu."),
]

# The top navigation: page name, label, path from the site root.
NAV = [("home", "Home", "")] + [(n, label, f"docs/{n}.html") for _, n, label, _ in DOCS] + [
    ("changelog", "Changelog", "changelog.html")
]


def slugify(value, separator="-"):
    """GitHub's heading anchors, so links written for GitHub keep working."""
    value = re.sub(r"<[^>]+>", "", value)
    value = html.unescape(value).strip().lower()
    value = re.sub(r"[^\w\- ]", "", value)
    return value.replace(" ", separator)


def render_md(text):
    md = markdown.Markdown(
        extensions=["tables", "fenced_code", "toc", "sane_lists"],
        extension_configs={"toc": {"slugify": slugify, "permalink": False}},
        output_format="html",
    )
    return md.convert(text)


def rewrite_links(body, src_dir, page_dir):
    """Points links that GitHub resolves inside the repository at the right place on the site.

    src_dir: the markdown file's folder relative to the repository root ("docs" or "").
    page_dir: the built page's folder relative to the site root ("docs" or "").
    """
    doc_pages = {posixpath.join("docs", src): f"docs/{name}.html" for src, name, _, _ in DOCS}
    doc_pages["CHANGELOG.md"] = "changelog.html"

    def fix(m):
        attr, url = m.group(1), html.unescape(m.group(2))
        if re.match(r"^[a-z][a-z0-9+.-]*:", url, re.I) or url.startswith(("#", "//", "/")):
            return m.group(0)
        path, _, frag = url.partition("#")
        target = posixpath.normpath(posixpath.join(src_dir, path)) if path else ""
        if target in doc_pages:
            new = posixpath.relpath(doc_pages[target], page_dir or ".")
        elif attr == "src" and target.startswith("docs/images/"):
            new = posixpath.relpath(target, page_dir or ".")
        else:
            kind = "tree" if target.endswith("/") or "." not in posixpath.basename(target) else "blob"
            new = f"{REPO}/{kind}/main/{target}"
        if frag:
            new += "#" + frag
        return f'{attr}="{html.escape(new, quote=True)}"'

    return re.sub(r'\b(href|src)="([^"]*)"', fix, body)


def first_heading(text):
    m = re.search(r"^# (.+)$", text, re.M)
    return m.group(1).strip() if m else "PSX RiPPER"


def nav_html(current, to_root):
    items = []
    for name, label, path in NAV:
        href = to_root + path if path else (to_root or "./")
        cur = ' aria-current="page"' if name == current else ""
        items.append(f'<li><a href="{href}"{cur}>{label}</a></li>')
    return "\n".join(items)


FOOT_NAV = [("accessibility", "Accessibility statement", "accessibility.html"),
            ("glossary", "Glossary", "glossary.html")]


def foot_nav_html(current, to_root):
    items = []
    for name, label, path in FOOT_NAV:
        cur = ' aria-current="page"' if name == current else ""
        items.append(f'<li><a href="{to_root}{path}"{cur}>{label}</a></li>')
    return "\n".join(items)


def page(shell, *, title, current, to_root, body, version, description, main_class):
    out = shell
    for key, val in {
        "title": html.escape(title),
        "description": html.escape(description, quote=True),
        "root": to_root or "./",
        "nav": nav_html(current, to_root),
        "footnav": foot_nav_html(current, to_root),
        "main_class": main_class,
        "body": body,
    }.items():
        out = out.replace("{{" + key + "}}", val)
    # last, so the version also reaches the body and the shell's footer
    return out.replace("{{version}}", html.escape(version))


EXT_A = re.compile(r'<a ((?:(?!>).)*?)href="(https?://[^"]+|mailto:[^"]+)"((?:(?!>).)*)>')


NEW_TAB = "(opens in a new tab)"


def mark_external(content):
    """A link that leaves the site (http or https) opens in a new tab. The arrow after it comes from
    the CSS, which matches the same addresses; a screen reader hears "(opens in a new tab)" from a
    visually hidden text, or from the end of the aria-label. A download stays in this tab."""
    def fix(m):
        before, url, after = m.group(1), m.group(2), m.group(3)
        attrs = before + after
        if url.startswith("mailto:") or "target=" in attrs or "/download/" in url:
            return m.group(0)
        tag = f'<a {before}href="{url}"{after} target="_blank" rel="noopener" data-newtab>'
        return re.sub(r'aria-label="([^"]*)"', lambda a: f'aria-label="{a.group(1)} {NEW_TAB}"', tag)

    content = EXT_A.sub(fix, content)
    # the hidden text goes last inside each such link, unless an aria-label already names it
    return re.sub(r'(<a (?:(?!>).)*? data-newtab>)(.*?)</a>',
                  lambda m: m.group(0).replace(" data-newtab", "") if "aria-label=" in m.group(1)
                  else f'{m.group(1).replace(" data-newtab", "")}{m.group(2)}'
                       f'<span class="sr-only"> {NEW_TAB}</span></a>',
                  content, flags=re.S)


def scrollables(body):
    """Code blocks and tables can scroll sideways on a narrow screen; a keyboard user reaches them
    with Tab and scrolls them with the arrow keys (WCAG 2.1.1). A table sits in a named region,
    named after the heading it is under. A list of links to the page's own sections becomes an
    "On this page" navigation."""
    body = body.replace("<pre>", '<pre tabindex="0">')
    # a list of nothing but links to this page's sections is its table of contents
    body = re.sub(r'<ul>\n((?:<li><a href="#[^"]*">[^<]*</a></li>\n)+)</ul>',
                  r'<nav class="toc" aria-labelledby="toc-h"><h2 id="toc-h" class="toc-h">On this page</h2>'
                  r'\n<ul>\n\1</ul></nav>', body)
    out, heading, pos, used = [], "", 0, {}
    for m in re.finditer(r"<h[1-6][^>]*>(.*?)</h[1-6]>|<table>.*?</table>", body, re.S):
        out.append(body[pos:m.start()])
        if m.group(0).startswith("<h"):
            heading = html.unescape(re.sub(r"<[^>]+>", "", m.group(1))).strip()
            out.append(m.group(0))
        else:
            name = f"Table: {heading}" if heading else "Table"
            used[name] = used.get(name, 0) + 1
            if used[name] > 1:   # a second table under the same heading gets a number
                name += f" ({used[name]})"
            label = html.escape(name, quote=True)
            out.append(f'<div class="table-wrap" tabindex="0" role="region" aria-label="{label}">'
                       f"{m.group(0)}</div>")
        pos = m.end()
    out.append(body[pos:])
    return "".join(out)


def name_changelog_links(body):
    """The changelog's links are a version, an issue number or a commit hash. A hidden text says
    where each goes, so the link makes sense on its own (WCAG 2.4.9)."""
    body = re.sub(r'(<a href="[^"]*/compare/(v[^".]+\.[^".]+\.[^".]+)\.\.\.[^"]*">)([^<]+)</a>',
                  r'\1\3<span class="sr-only"> (code changes since \2 on GitHub)</span></a>', body)
    body = re.sub(r'(<a href="[^"]*/issues/\d+">)#(\d+)</a>',
                  r'\1<span class="sr-only">GitHub issue or pull request </span>#\2</a>', body)
    return re.sub(r'(<a href="[^"]*/commit/[0-9a-f]+">)([0-9a-f]{7,})</a>',
                  r'\1<span class="sr-only">commit </span>\2</a>', body)


GLOSSARY_SKIP = ("a", "code", "pre", "script", "style", "svg", "h1", "h2", "h3", "h4", "h5", "h6",
                 "figcaption", "title")


def glossarize(body, to_root):
    """Links the first use of each glossary word on a page to its entry (WCAG 3.1.3, 3.1.4).
    Words in headings, links and code are left alone."""
    seen, out, depth = set(), [], 0
    pats = [(gid, re.compile(pat)) for gid, _, pat, _ in GLOSSARY]
    for part in re.split(r"(<[^>]+>)", body):
        if part.startswith("<"):
            m = re.match(r"<(/?)([a-zA-Z0-9]+)", part)
            if m and m.group(2).lower() in GLOSSARY_SKIP and not part.endswith("/>"):
                depth += -1 if m.group(1) else 1
            out.append(part)
            continue
        if depth > 0 or not part.strip():
            out.append(part)
            continue
        # pieces: (is_text, string); an inserted link is never searched again
        pieces = [(True, part)]
        for gid, rx in pats:
            if gid in seen:
                continue
            for i, (is_text, txt) in enumerate(pieces):
                m = rx.search(txt) if is_text else None
                if m:
                    seen.add(gid)
                    link = (f'<a class="gl" href="{to_root}glossary.html#{gid}">{m.group(0)}'
                            f'<span class="sr-only"> (glossary)</span></a>')
                    pieces[i:i + 1] = [(True, txt[:m.start()]), (False, link), (True, txt[m.end():])]
                    break
        out.append("".join(t for _, t in pieces))
    return "".join(out)


RING = '<circle cx="12" cy="12" r="10.5" fill="rgb(255 255 255 / 0.06)" stroke="#e8ecff" stroke-width="1.5"/>'
GLYPH = {   # the pad's face buttons as the console draws them: a white ring, the symbol in its colour
    "✕": ("cross", '<path d="M8 8l8 8M16 8l-8 8" stroke="#9db7ff" stroke-width="1.7" stroke-linecap="round"/>'),
    "○": ("circle", '<circle cx="12" cy="12" r="4.6" fill="none" stroke="#ff8aa6" stroke-width="1.7"/>'),
    "△": ("triangle", '<path d="M12 7.2 16.6 15.3H7.4Z" fill="none" stroke="#74e0bb" stroke-width="1.6" stroke-linejoin="round"/>'),
    "□": ("square", '<rect x="7.5" y="7.5" width="9" height="9" rx="1" fill="none" stroke="#f3a0dc" stroke-width="1.6"/>'),
}
PILL_RE = re.compile(r"(?<![\w.-])(START|SELECT|L1|R1|L2|R2)(?![\w.-])")
SKIP_TAGS = ("code", "pre", "script", "style", "title", "svg", "a")


def glyph(ch):
    name, inner = GLYPH[ch]
    return (f'<span class="g g-btn" role="img" aria-label="{name}"><svg viewBox="0 0 24 24" '
            f'aria-hidden="true">{RING}{inner}</svg></span>')


def buttonize(content):
    """Every pad button named in running text looks like it does on the console: the face buttons
    as rings with their symbol, START, SELECT and the shoulder buttons as pills. Text inside code,
    links and the head is left alone."""
    content = re.sub(r"<strong>(START|SELECT|L1|R1|L2|R2)</strong>", r"\1", content)
    out, depth = [], 0
    for part in re.split(r"(<[^>]+>)", content):
        if part.startswith("<"):
            m = re.match(r"<(/?)([a-zA-Z0-9]+)", part)
            if m and m.group(2).lower() in SKIP_TAGS and not part.endswith("/>"):
                depth += -1 if m.group(1) else 1
            out.append(part)
            continue
        if depth > 0 or not part:
            out.append(part)
            continue
        part = "".join(glyph(c) if c in GLYPH else c for c in part)
        part = PILL_RE.sub(r'<span class="g-key">\1</span>', part)
        out.append(part)
    return "".join(out)


def main():
    version = (ROOT / "version.txt").read_text(encoding="utf-8").strip()
    if not re.fullmatch(r"\d+\.\d+\.\d+([-+][\w.]+)?", version):
        sys.exit(f"version.txt does not hold a version: {version!r}")
    shell = (SITE / "page.html").read_text(encoding="utf-8")

    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.mkdir()
    (OUT / "docs").mkdir()

    # static files
    shutil.copy2(SITE / "style.css", OUT / "style.css")
    shutil.copy2(SITE / "CNAME", OUT / "CNAME")
    shutil.copytree(SITE / "img", OUT / "img")
    shutil.copytree(ROOT / "docs" / "images", OUT / "docs" / "images")
    shutil.copy2(ROOT / "docs" / "images" / "icon.png", OUT / "icon.png")
    (OUT / ".nojekyll").write_text("", encoding="utf-8")

    pages = []

    # home
    home = (SITE / "home.html").read_text(encoding="utf-8")
    pages.append(("index.html", page(
        shell, title="PSX RiPPER: rip your PS1 discs on a PS3", current="home", to_root="",
        body=glossarize(home, ""), version=version, main_class="home",
        description="A PS3 homebrew app that rips your own PlayStation 1 discs to bit-perfect "
                    ".bin/.cue images and verifies them against Redump.")))

    # docs
    for src, name, label, summary in DOCS:
        text = (ROOT / "docs" / src).read_text(encoding="utf-8")
        body = scrollables(rewrite_links(render_md(text), "docs", "docs"))
        title = first_heading(text)
        body = body.replace("</h1>", f'</h1>\n<p class="summary"><strong>In short:</strong> '
                                     f"{html.escape(summary, quote=False)}</p>", 1)
        body = glossarize(f'<article class="prose">\n{body}\n</article>', "../")
        body += (f'\n<p class="edit"><a href="{REPO}/blob/main/docs/{src}">'
                 f"docs/{src} on GitHub</a></p>")
        pages.append((f"docs/{name}.html", page(
            shell, title=f"{title} | PSX RiPPER", current=name, to_root="../", body=body,
            version=version, main_class="doc", description=f"PSX RiPPER {label.lower()}.")))

    # changelog
    text = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    body = name_changelog_links(scrollables(rewrite_links(render_md(text), "", "")))
    body = body.replace("</h1>", '</h1>\n<p class="summary"><strong>In short:</strong> what changed '
                        "in each version of the app, newest first.</p>", 1)
    body = glossarize(f'<article class="prose changelog">\n{body}\n</article>', "")
    pages.append(("changelog.html", page(
        shell, title="Changelog | PSX RiPPER", current="changelog", to_root="", body=body,
        version=version, main_class="doc", description="What changed in each PSX RiPPER release.")))

    # 404
    pages.append(("404.html", page(
        shell, title="Not found | PSX RiPPER", current="", to_root="/", version=version,
        main_class="doc", description="Page not found.",
        body='<article class="prose"><h1>Disc not found</h1><p>No page has this address. '
             'Go to the <a href="/">home page</a> or the <a href="/docs/user-guide.html">user '
             "guide</a>.</p></article>")))

    # glossary
    entries = "\n".join(
        f'<dt id="{gid}">{html.escape(term)}</dt>\n<dd>{html.escape(text)}</dd>'
        for gid, term, _, text in sorted(GLOSSARY, key=lambda g: g[1].lower()))
    pages.append(("glossary.html", page(
        shell, title="Glossary | PSX RiPPER", current="glossary", to_root="", version=version,
        main_class="doc", description="The words and abbreviations used on the PSX RiPPER site.",
        body='<article class="prose">\n<h1>Glossary</h1>\n<p class="summary"><strong>In short:'
             "</strong> what the technical words and abbreviations on this site mean. On every "
             "page, the first use of each of these words links here.</p>\n"
             f'<dl class="glossary">\n{entries}\n</dl>\n</article>')))

    # accessibility statement
    pages.append(("accessibility.html", page(
        shell, title="Accessibility statement | PSX RiPPER", current="accessibility", to_root="",
        version=version, main_class="doc",
        description="How accessible the PSX RiPPER website is, what is not yet, and how to report "
                    "a problem.",
        body=(SITE / "accessibility.html").read_text(encoding="utf-8"))))

    for rel, content in pages:
        if "{{" in content:
            sys.exit(f"{rel}: a placeholder was not filled: "
                     f"{re.search(r'{{[^}]*}}', content).group(0)}")
        (OUT / rel).write_text(mark_external(buttonize(content)), encoding="utf-8")
        print(f"_site/{rel}")


if __name__ == "__main__":
    main()
