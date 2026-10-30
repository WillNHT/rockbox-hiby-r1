#!/usr/bin/env python3
"""Compile a skin written as HTML and CSS into the .wps language.

    python3 themes/wpsc.py themes/src/SnappyV2.html > SnappyV2.wps

A strict subset, translated tag for tag - nothing here emulates CSS, it
only names what the skin engine already does:

  <div id=..>       a viewport: %Vl(id,...), or %V(...) with no id. Its
                    text is skin code and goes in as it is. Boxes are flat
                    and drawn in document order, so later is on top.
  left top width    %V's x, y, w, h. right/bottom with a width/height
  height right      is the engine's negative x/y; with left/top it is its
  bottom            negative w/h.
  font-family: fN   the Nth @font-face, counted from 2 as %Fl does.
  background:       transparent: %Vt(0).
  color: #rrggbb    %Vf.
  opacity: 0..1     %Vo(id,n), set from the default viewport.
  transform:        translate(x, y): %Vx(id,x,y), from the default
                    viewport.
  transition:       opacity|transform ms [linear|ease-out|ease-in-out]:
                    %Va(o|x,ms,ease) in the viewport.
  animation:        name ms steps(n): one %an(n, ms/n) frame per step,
                    each showing the box with the transform and visibility
                    its @keyframes give at that step.
  when="..."        what shows the box, as %Vd(id) enables. Terms joined
                    by "and" nest, "or" gives each alternative its own
                    enable. A term is %tag (true), !%tag (false),
                    %tag[k] (the tag's kth case, as in %?tag<1|2|..>), or
                    %tag op value, op one of = != < > <= >= (%?if).
  <img id src>      %xl(id,src,x,y,frames), x y frames from attributes.
  <skin statusbar="none">   %wd.
  <!-- -->          # comment lines, kept where they are.

Selectors are #id, .class and inline style="", applied in that order.
"""
import re
import sys
import textwrap

TAG = re.compile(r"<!--(.*?)-->|<(/?)(skin|style|div|img)\b((?:[^>\"']|\"[^\"]*\"|'[^']*')*)>",
                 re.S)
ATTR = re.compile(r"([\w-]+)(?:=(\"[^\"]*\"|'[^']*'|[^\s>]+))?")
EASE = {"linear": "linear", "ease-out": "out", "ease-in-out": "inout"}


def fail(msg):
    sys.exit("wpsc: " + msg)


def attrs(s):
    return {k: (v or "").strip("\"'") for k, v in ATTR.findall(s)}


def decls(s):
    out = {}
    for d in s.split(";"):
        if d.strip():
            k, _, v = d.partition(":")
            out[k.strip()] = v.strip()
    return out


def blocks(css):
    """(prelude, body) pairs at the top level, bodies with their braces
    matched, so @keyframes keeps its inner rules."""
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)
    out, i = [], 0
    while True:
        j = css.find("{", i)
        if j < 0:
            return out
        depth, k = 1, j + 1
        while depth:
            if k >= len(css):
                fail("unbalanced { in <style>")
            depth += {"{": 1, "}": -1}.get(css[k], 0)
            k += 1
        out.append((css[i:j].strip(), css[j + 1:k - 1]))
        i = k


def px(v):
    return int(v.strip().removesuffix("px"))


def term(t, inner):
    """One condition around `inner`."""
    t = t.strip()
    m = re.fullmatch(r"(%\w+(?:\([^)]*\))?)\s*(!=|<=|>=|==|=|<|>)\s*(.+)", t)
    if m:
        op = "=" if m[2] == "==" else m[2]
        return "%%?if(%s,%s,%s)<%s>" % (m[1], op, m[3].strip(), inner)
    m = re.fullmatch(r"%(\w+)\[(\d+)\]", t)
    if m:
        k = int(m[2])
        return "%%?%s<%s%s|>" % (m[1], "|" * (k - 1), inner)
    m = re.fullmatch(r"(!?)%(\w+)", t)
    if m:
        return "%%?%s<%s>" % (m[2], "|" + inner if m[1] else inner + "|")
    fail("cannot read condition %r" % t)


def nest(terms, inner):
    for t in reversed(terms):
        inner = term(t, inner)
    return inner


def translate(v):
    m = re.fullmatch(r"translate\(\s*(-?\d+)(?:px)?\s*,\s*(-?\d+)(?:px)?\s*\)",
                     v.strip())
    if not m:
        fail("only translate(x, y) is supported, not %r" % v)
    return int(m[1]), int(m[2])


class Skin:
    def __init__(self, src):
        self.head = []          # lines before the enables
        self.enables = []
        self.body = []          # viewports and their comments
        self.fonts = {}
        self.rules = {}         # selector -> decls
        self.frames = {}        # @keyframes name -> [(pct, decls)]
        self.seen_div = False
        self.parse(src)

    def comment(self, text, into):
        lines = textwrap.dedent(text.strip("\n")).split("\n")
        into += ["#" + (" " + ln if ln.strip() else "") for ln in lines]

    def parse(self, src):
        pos, open_div = 0, None
        for m in TAG.finditer(src):
            between = src[pos:m.start()]
            pos = m.end()
            if open_div is not None:
                text += between
                if m[1] is not None:    # a comment inside a box
                    lines = []
                    self.comment(m[1], lines)
                    text += "\n".join(lines)
                elif m[3] == "div" and m[2] == "/":
                    self.box(open_div, text)
                    open_div = None
                else:
                    fail("<%s> inside a <div>" % m[3])
                continue
            if m[1] is not None:
                self.comment(m[1], self.body if self.seen_div else self.head)
            elif m[2]:
                continue        # </skin>, </style>
            elif m[3] == "skin":
                if attrs(m[4]).get("statusbar") == "none":
                    self.head.append("%wd")
            elif m[3] == "style":
                end = src.index("</style>", pos)
                self.style(src[pos:end])
                pos = end
            elif m[3] == "img":
                a = attrs(m[4])
                self.head.append("%%xl(%s,%s%s)" % (a["id"], a["src"], "".join(
                    "," + a[k] for k in ("x", "y", "frames") if k in a)))
            elif m[3] == "div":
                self.seen_div = True
                open_div, text = attrs(m[4]), ""
        if open_div is not None:
            fail("unclosed <div>")

    def style(self, css):
        for pre, body in blocks(css):
            if pre == "@font-face":
                d = decls(body)
                name = d["font-family"]
                n = len(self.fonts) + 2
                self.fonts[name] = n
                url = re.fullmatch(r"url\((.*)\)", d["src"])[1].strip("\"'")
                self.head.append("%%Fl(%d,%s)" % (n, url))
            elif pre.startswith("@keyframes"):
                self.frames[pre.split()[1]] = sorted(
                    (float(p.strip().rstrip("%")), decls(b))
                    for sel, b in blocks(body) for p in sel.split(","))
            else:
                for sel in pre.split(","):
                    self.rules.setdefault(sel.strip(), {}).update(decls(body))

    def computed(self, a):
        d = {}
        for c in a.get("class", "").split():
            d.update(self.rules.get("." + c, {}))
        if "id" in a:
            d.update(self.rules.get("#" + a["id"], {}))
        d.update(decls(a.get("style", "")))
        return d

    def box(self, a, text):
        d = self.computed(a)
        vid = a.get("id")

        def axis(lo, hi, size):
            if lo in d:
                return px(d[lo]), (-px(d[hi]) if hi in d else px(d[size]))
            if hi in d and size in d:
                return -(px(d[hi]) + px(d[size])), -px(d[hi])
            fail("%s: needs %s, or %s and %s" % (vid, lo, hi, size))
        x, w = axis("left", "right", "width")
        y, h = axis("top", "bottom", "height")
        font = d.get("font-family")
        font = str(self.fonts[font]) if font else "-"
        geo = "%d,%d,%d,%d,%s" % (x, y, w, h, font)
        self.body.append("%%Vl(%s,%s)" % (vid, geo) if vid else "%%V(%s)" % geo)

        bg = d.get("background")
        if bg == "transparent":
            self.body.append("%Vt(0)")
        elif bg:
            fail("%s: background %r - only transparent" % (vid, bg))
        if "color" in d:
            self.body.append("%%Vf(%s)" % d["color"].lstrip("#").upper())
        for t in filter(None, d.get("transition", "").split(",")):
            prop, ms, *ease = t.split()
            self.body.append("%%Va(%s,%d%s)" % (
                {"opacity": "o", "transform": "x"}[prop],
                int(ms.removesuffix("ms")),
                "," + EASE[ease[0]] if ease else ""))
        content = textwrap.dedent(text).strip("\n")
        if content:
            self.body.append(content)

        if not vid:
            if "when" in a or "animation" in d:
                fail("a box with when= or animation needs an id")
            return
        show = "%%Vd(%s)" % vid
        if "opacity" in d:
            show += "%%Vo(%s,%d)" % (vid, round(float(d["opacity"]) * 100))
        if "transform" in d:
            show += "%%Vx(%s,%d,%d)" % ((vid,) + translate(d["transform"]))
        alts = [[t for t in alt.split(" and ")]
                for alt in a["when"].split(" or ")] if "when" in a else [[]]
        if "animation" not in d:
            self.enables += [nest(alt, show) for alt in alts]
            return
        name, ms, steps = d["animation"].split()[:3]
        n = int(re.fullmatch(r"steps\((\d+)\)", steps)[1])
        ms = int(ms.removesuffix("ms"))
        frames = self.frames[name]
        for i in range(n):
            at = {}
            for pct, fd in frames:
                if pct <= i * 100 / n + 1e-3:
                    at.update(fd)
            if at.get("visibility") == "hidden":
                continue
            s = "%%Vd(%s)" % vid
            if "transform" in at:
                s += "%%Vx(%s,%d,%d)" % ((vid,) + translate(at["transform"]))
            tick = "%%an(%d,%d) = %d" % (n, ms // n, i + 1)
            self.enables += [nest(alt + [tick], s) for alt in alts]

    def wps(self):
        return "\n".join(self.head + self.enables + self.body) + "\n"


def compile_file(path):
    with open(path, encoding="utf-8") as f:
        return Skin(f.read()).wps()


if __name__ == "__main__":
    sys.stdout.write(compile_file(sys.argv[1]))
