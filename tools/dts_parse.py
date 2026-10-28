#!/usr/bin/env python3
"""Minimal parser for dtc -O dts output (enough for vendor Qualcomm DTs)."""
import re
import sys


class Node:
    __slots__ = ("name", "label", "props", "children", "parent")

    def __init__(self, name, label=None, parent=None):
        self.name = name
        self.label = label
        self.props = {}
        self.children = []
        self.parent = parent

    def path(self):
        if self.parent is None:
            return "/"
        p = self.parent.path()
        return (p if p != "/" else "") + "/" + self.name

    def child(self, name):
        for c in self.children:
            if c.name == name:
                return c
        return None

    def walk(self):
        yield self
        for c in self.children:
            yield from c.walk()


def _split_statements(text):
    """Yield ('node', name) / ('prop', raw) for the body of a dts file."""
    i, n = 0, len(text)
    quoted = False
    angle = 0
    brack = 0
    buf_start = None
    while i < n:
        ch = text[i]
        if quoted:
            if ch == "\\":
                i += 2
                continue
            if ch == '"':
                quoted = False
            i += 1
            continue
        if ch == '"':
            quoted = True
            i += 1
            continue
        if ch == "<":
            angle += 1
        elif ch == ">":
            angle -= 1
        elif ch == "[":
            brack += 1
        elif ch == "]":
            brack -= 1
        elif not angle and not brack:
            if ch == "{":
                stmt = text[buf_start:i].strip()
                yield ("node", stmt)
                # skip to matching close brace handled by caller recursion
                depth = 1
                j = i + 1
                q = False
                a = b = 0
                while j < n and depth:
                    c = text[j]
                    if q:
                        if c == "\\":
                            j += 2
                            continue
                        if c == '"':
                            q = False
                    elif c == '"':
                        q = True
                    elif c == "<":
                        a += 1
                    elif c == ">":
                        a -= 1
                    elif c == "[":
                        b += 1
                    elif c == "]":
                        b -= 1
                    elif not a and not b:
                        if c == "{":
                            depth += 1
                        elif c == "}":
                            depth -= 1
                    j += 1
                yield ("body", text[i + 1:j - 1])
                i = j
                buf_start = None
                continue
            if ch == ";" and buf_start is not None:
                yield ("prop", text[buf_start:i].strip())
                buf_start = None
                i += 1
                continue
            if buf_start is None:
                if not ch.isspace() and ch != ";":
                    buf_start = i
                i += 1
                continue
            i += 1
            continue
        i += 1


def _clean(txt):
    txt = re.sub(r"/\*.*?\*/", " ", txt, flags=re.S)
    return txt


def parse(path):
    text = _clean(open(path, encoding="utf-8", errors="replace").read())
    # drop directives and the outer / { ... }
    text = re.sub(r"^/dts-v1/;.*$", "", text, flags=re.M)
    text = re.sub(r"^/plugin/;.*$", "", text, flags=re.M)
    text = re.sub(r"^/memreserve/.*$", "", text, flags=re.M)
    root = Node("/")
    # first node statement is "/ {" -> its body becomes root body
    gen = _split_statements(text)
    for kind, body in [(k, b) for k, b in gen if k == "body"]:
        _fill(root, body)
        break
    return root


def _fill(node, body):
    pending = None
    for kind, item in _split_statements(body):
        if kind == "node":
            lbl = None
            m = re.match(r"^([A-Za-z0-9_]+):\s*(.*)$", item, re.S)
            if m:
                lbl, item = m.group(1), m.group(2).strip()
            pending = Node(item.strip(), lbl, node)
            node.children.append(pending)
        elif kind == "body":
            if pending is not None:
                _fill(pending, item)
                pending = None
        elif kind == "prop":
            if "=" in item:
                k, v = item.split("=", 1)
                node.props[k.strip()] = v.strip()
            else:
                node.props[item.strip()] = None


def cells(v):
    """Return integer cells from a <...> property, or []."""
    if not v:
        return []
    out = []
    for m in re.finditer(r"<([^>]*)>", v):
        for tok in m.group(1).split():
            tok = tok.strip()
            if tok.startswith("0x") or tok.startswith("0X"):
                try:
                    out.append(int(tok, 16))
                except ValueError:
                    pass
            elif re.fullmatch(r"-?\d+", tok):
                out.append(int(tok))
    return out


def bytes_(v):
    """Return the byte values of [...] arrays in a property."""
    if not v:
        return []
    out = []
    for m in re.finditer(r"\[([^\]]*)\]", v):
        for tok in m.group(1).split():
            tok = tok.strip()
            if re.fullmatch(r"[0-9a-fA-F]{2}", tok):
                out.append(int(tok, 16))
    return out


def strings(v):
    if not v:
        return []
    out = []
    for s in re.findall(r'"((?:[^"\\]|\\.)*)"', v):
        out.extend(s.encode().decode("unicode_escape").split("\0"))
    return out


def dump(node, indent=0, out=None):
    out = out if out is not None else sys.stdout
    pad = "\t" * indent
    lbl = f"{node.label}: " if node.label else ""
    out.write(f"{pad}{lbl}{node.name} {{\n")
    for k, v in node.props.items():
        out.write(f"{pad}\t{k}" + ("" if v is None else f" = {v}") + ";\n")
    for c in node.children:
        dump(c, indent + 1, out)
    out.write(f"{pad}}};\n")
