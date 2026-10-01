#!/usr/bin/env python3
"""Assemble docs/*.html from the body fragments in tools/doc_pages/, so every page shares one
shell and one navigation list. Run from anywhere:  python3 tools/build_docs.py"""
import os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "docs")
PAGES_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "doc_pages")

NAV = [
    ("index.html",           "Overview"),
    ("getting-started.html", "Getting Started"),
    ("api.html",             "API Reference"),
    ("porting.html",         "Porting"),
    ("chips.html",           "Chip Drivers"),
    ("format.html",          "On-Flash Format"),
    ("performance.html",     "Performance"),
    ("testing.html",         "Testing"),
    ("upgrading.html",       "Upgrading"),
]

SHELL = """<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title} &middot; nandlog</title>
<meta name="description" content="{desc}">
<link rel="stylesheet" href="style.css">
<link rel="icon" href="data:image/svg+xml,<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 100 100'><text y='.9em' font-size='90'>&#128190;</text></svg>">
</head>
<body>
<a class="skip" href="#content">Skip to content</a>
<header class="topbar">
  <a class="brand" href="index.html">nandlog</a>
  <span class="ver">v1.2</span>
  <input type="checkbox" id="navtoggle" hidden>
  <label for="navtoggle" class="navbtn" aria-label="Menu">&#9776;</label>
  <nav class="sidenav">{nav}</nav>
</header>
<div class="layout">
  <nav class="sidenav desktop">{nav}</nav>
  <main id="content">
{body}
    <footer>
      <p>nandlog is MIT-licensed. <a href="https://github.com/hedgetechllc/nandlog">Source on GitHub</a>.</p>
      <p class="muted">Design rationale, trade-offs and field results are covered in the whitepaper
         <em>(arXiv link to follow)</em>.</p>
    </footer>
  </main>
</div>
</body>
</html>
"""

def nav_html(current):
    items = []
    for href, label in NAV:
        cls = ' class="current" aria-current="page"' if href == current else ''
        items.append(f'<a href="{href}"{cls}>{label}</a>')
    return "\n    " + "\n    ".join(items) + "\n  "

def build():
    os.makedirs(OUT, exist_ok=True)
    built = []
    for href, label in NAV:
        src = os.path.join(PAGES_DIR, href)
        if not os.path.exists(src):
            print(f"  MISSING fragment: {href}")
            continue
        raw = open(src).read()
        m = re.match(r"<!--\s*title:\s*(.*?)\s*\|\s*desc:\s*(.*?)\s*-->\s*", raw, re.S)
        title, desc = (m.group(1), m.group(2)) if m else (label, "nandlog documentation")
        body = raw[m.end():] if m else raw
        html = SHELL.format(title=title, desc=desc, nav=nav_html(href), body=body.rstrip() + "\n")
        open(os.path.join(OUT, href), "w").write(html)
        built.append(href)
    open(os.path.join(OUT, ".nojekyll"), "w").write("")
    print(f"  built {len(built)} pages: {', '.join(built)}")

if __name__ == "__main__":
    build()
