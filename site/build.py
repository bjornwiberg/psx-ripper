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

# The docs pages in nav order: source file, page name, short nav label.
DOCS = [
    ("user-guide.md", "user-guide", "User guide"),
    ("configuration.md", "configuration", "Configuration"),
    ("mcp.md", "mcp", "MCP"),
    ("faq.md", "faq", "FAQ"),
    ("how-it-works.md", "how-it-works", "How it works"),
]

# The top navigation: page name, label, path from the site root.
NAV = [("home", "Home", "")] + [(n, label, f"docs/{n}.html") for _, n, label in DOCS] + [
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
    doc_pages = {posixpath.join("docs", src): f"docs/{name}.html" for src, name, _ in DOCS}
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


def page(shell, *, title, current, to_root, body, version, description, main_class):
    out = shell
    for key, val in {
        "title": html.escape(title),
        "description": html.escape(description, quote=True),
        "root": to_root or "./",
        "nav": nav_html(current, to_root),
        "main_class": main_class,
        "body": body,
    }.items():
        out = out.replace("{{" + key + "}}", val)
    # last, so the version also reaches the body and the shell's footer
    return out.replace("{{version}}", html.escape(version))


EXT_A = re.compile(r'<a ((?:(?!>).)*?)href="(https?://[^"]+|mailto:[^"]+)"((?:(?!>).)*)>')


def mark_external(content):
    """External links open in a new tab and get the small arrow (class ext); buttons and mail links
    keep their look but still open in a new tab when they leave the site."""
    def fix(m):
        before, url, after = m.group(1), m.group(2), m.group(3)
        attrs = before + after
        if url.startswith("https://psxripper.bjrn.nu") or "target=" in attrs:
            return m.group(0)
        if url.startswith("mailto:") or "btn-dl" in attrs or "/download/" in url:   # a download stays in this tab
            return m.group(0)
        extra = ' target="_blank" rel="noopener"'
        if "class=" in attrs:
            if "btn" not in attrs:
                before = re.sub(r'class="', 'class="ext ', before) if 'class="' in before else before
                after = re.sub(r'class="', 'class="ext ', after) if 'class="' in after else after
        else:
            extra += ' class="ext"'
        return f'<a {before}href="{url}"{after}{extra}>'
    return EXT_A.sub(fix, content)


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
        body=home, version=version, main_class="home",
        description="A PS3 homebrew app that rips your own PlayStation 1 discs to bit-perfect "
                    ".bin/.cue images and verifies them against Redump.")))

    # docs
    for src, name, label in DOCS:
        text = (ROOT / "docs" / src).read_text(encoding="utf-8")
        body = rewrite_links(render_md(text), "docs", "docs")
        title = first_heading(text)
        body = f'<article class="prose">\n{body}\n</article>'
        body += (f'\n<p class="edit"><a href="{REPO}/blob/main/docs/{src}">'
                 f"docs/{src} on GitHub</a></p>")
        pages.append((f"docs/{name}.html", page(
            shell, title=f"{title} | PSX RiPPER", current=name, to_root="../", body=body,
            version=version, main_class="doc", description=f"PSX RiPPER {label.lower()}.")))

    # changelog
    text = (ROOT / "CHANGELOG.md").read_text(encoding="utf-8")
    body = rewrite_links(render_md(text), "", "")
    body = f'<article class="prose changelog">\n{body}\n</article>'
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

    for rel, content in pages:
        if "{{" in content:
            sys.exit(f"{rel}: a placeholder was not filled: "
                     f"{re.search(r'{{[^}]*}}', content).group(0)}")
        (OUT / rel).write_text(mark_external(content), encoding="utf-8")
        print(f"_site/{rel}")


if __name__ == "__main__":
    main()
