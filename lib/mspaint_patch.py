#!/usr/bin/env python3
"""Patch the Windows 10 Paint files so that they run under Wine.

Only the Python standard library is used.

Commands:
  gather WINDOWS_DIR SRC_DIR [--fonts default|all|none]
      Copy the needed files from a Windows directory (C:\\Windows of an
      installed system or of an extracted image) into SRC_DIR.
  build SRC_DIR OUT_DIR
      Make the patched files from the files in SRC_DIR:
        OUT_DIR/app/       mspaint.exe, mfc42u.dll, DismApi.dll
        OUT_DIR/system32/  uiribbon.dll, uiribbonres.dll, fms.dll
        OUT_DIR/mspaint.png  (application icon, if one is found)
  fontreg FONT_DIR OUT.reg
      Write a .reg file that registers the fonts in FONT_DIR with Wine.

See the README for the reason of each patch.
"""
import os
import re
import shutil
import struct
import sys
import zlib

RES_DIR = 2           # IMAGE_DIRECTORY_ENTRY_RESOURCE
IMPORT_DIR = 1        # IMAGE_DIRECTORY_ENTRY_IMPORT
DELAY_IMPORT_DIR = 13  # IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT
RT_MANIFEST = 24


class PatchError(Exception):
    pass


# ---------------------------------------------------------------- PE file --

class PE:
    """Minimal PE32+ reader: sections, imports, delay imports, resources."""

    def __init__(self, data):
        self.data = data
        if data[:2] != b"MZ":
            raise PatchError("not a PE file")
        self.pe_off = struct.unpack_from("<I", data, 0x3C)[0]
        if data[self.pe_off:self.pe_off + 4] != b"PE\0\0":
            raise PatchError("not a PE file")
        fh = self.pe_off + 4
        self.nsec = struct.unpack_from("<H", data, fh + 2)[0]
        opt_size = struct.unpack_from("<H", data, fh + 16)[0]
        self.opt = fh + 20
        self.magic = struct.unpack_from("<H", data, self.opt)[0]
        if self.magic not in (0x10B, 0x20B):
            raise PatchError("unknown PE optional header")
        self.dd_off = self.opt + (112 if self.magic == 0x20B else 96)
        self.sections = []
        sec_tab = self.opt + opt_size
        for i in range(self.nsec):
            name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", data, sec_tab + 40 * i)
            self.sections.append((name.rstrip(b"\0"), va, vsize, rptr, rsize))

    def directory(self, index):
        return struct.unpack_from("<II", self.data, self.dd_off + 8 * index)

    def rva_to_off(self, rva):
        for _, va, vsize, rptr, rsize in self.sections:
            if va <= rva < va + max(vsize, rsize):
                if rva - va >= rsize:
                    return None
                return rptr + rva - va
        return None

    def off_to_rva(self, off):
        for _, va, vsize, rptr, rsize in self.sections:
            if rptr <= off < rptr + rsize:
                return va + off - rptr
        return None

    def section_of_rva(self, rva):
        for name, va, vsize, _, rsize in self.sections:
            if va <= rva < va + max(vsize, rsize):
                return name
        return None

    def get(self, rva, size):
        off = self.rva_to_off(rva)
        if off is None:
            return b"\0" * size
        blob = self.data[off:off + size]
        return blob + b"\0" * (size - len(blob))

    def cstring_at(self, rva):
        off = self.rva_to_off(rva)
        end = self.data.index(b"\0", off)
        return off, self.data[off:end]

    def import_names(self):
        """Yield (kind, file offset, dll name) for normal and delay imports."""
        rva, size = self.directory(IMPORT_DIR)
        if rva:
            pos = rva
            while True:
                name_rva = struct.unpack_from("<I", self.get(pos, 20), 12)[0]
                if not name_rva:
                    break
                off, name = self.cstring_at(name_rva)
                yield "import", off, name
                pos += 20
        rva, size = self.directory(DELAY_IMPORT_DIR)
        if rva:
            pos = rva
            while True:
                attrs, name_rva = struct.unpack_from("<II", self.get(pos, 32))
                if not name_rva:
                    break
                off, name = self.cstring_at(name_rva)
                yield "delay import", off, name
                pos += 32

    def resources(self):
        """Return {type: {name: {lang: (blob, codepage)}}}. Keys are ints or strs."""
        base, size = self.directory(RES_DIR)
        tree = {}
        if not base:
            return tree

        def entries(dir_rva):
            hdr = self.get(dir_rva, 16)
            n_named, n_ids = struct.unpack_from("<HH", hdr, 12)
            for i in range(n_named + n_ids):
                name, target = struct.unpack("<II", self.get(dir_rva + 16 + 8 * i, 8))
                if name & 0x80000000:
                    s_rva = base + (name & 0x7FFFFFFF)
                    length = struct.unpack("<H", self.get(s_rva, 2))[0]
                    key = self.get(s_rva + 2, 2 * length).decode("utf-16-le")
                else:
                    key = name
                yield key, target

        for tkey, t in entries(base):
            if not t & 0x80000000:
                continue
            for nkey, n in entries(base + (t & 0x7FFFFFFF)):
                if not n & 0x80000000:
                    continue
                for lkey, l in entries(base + (n & 0x7FFFFFFF)):
                    if l & 0x80000000:
                        continue
                    d_rva, d_size, codepage = struct.unpack("<III", self.get(base + l, 12))
                    tree.setdefault(tkey, {}).setdefault(nkey, {})[lkey] = (self.get(d_rva, d_size), codepage)
        return tree


# ------------------------------------------------------- resource merging --
# Wine does not load .mui and .mun files. The resources of those files are
# merged into the language-neutral PE as a new .rsrc section.

def _sort_keys(d):
    named = sorted(k for k in d if isinstance(k, str))
    ids = sorted(k for k in d if not isinstance(k, str))
    return named, ids


def _build_rsrc(tree, base_rva):
    def dir_size(d):
        return 16 + 8 * len(d)

    type_named, type_ids = _sort_keys(tree)
    type_keys = type_named + type_ids
    off = dir_size(tree)
    type_dir_off, name_dir_off, data_entry_off = {}, {}, {}
    for tk in type_keys:
        type_dir_off[tk] = off
        off += dir_size(tree[tk])
    for tk in type_keys:
        nn, ni = _sort_keys(tree[tk])
        for nk in nn + ni:
            name_dir_off[(tk, nk)] = off
            off += dir_size(tree[tk][nk])
    for tk in type_keys:
        nn, ni = _sort_keys(tree[tk])
        for nk in nn + ni:
            for lk in sorted(tree[tk][nk]):
                data_entry_off[(tk, nk, lk)] = off
                off += 16
    strings_start = off
    strings = bytearray()
    string_off = {}

    def add_string(s):
        nonlocal off
        if s not in string_off:
            string_off[s] = off
            enc = s.encode("utf-16-le")
            strings.extend(struct.pack("<H", len(s)) + enc)
            off += 2 + len(enc)
            if off % 2:
                strings.append(0)
                off += 1

    for tk in type_named:
        add_string(tk)
    for tk in type_keys:
        for nk in _sort_keys(tree[tk])[0]:
            add_string(nk)
    blobs_start = (off + 7) & ~7
    off = blobs_start
    blobs = bytearray()
    blob_off = {}
    for tk in type_keys:
        nn, ni = _sort_keys(tree[tk])
        for nk in nn + ni:
            for lk in sorted(tree[tk][nk]):
                blob, _ = tree[tk][nk][lk]
                blob_off[(tk, nk, lk)] = off
                blobs.extend(blob)
                off += len(blob)
                pad = (-off) & 7
                blobs.extend(b"\0" * pad)
                off += pad

    out = bytearray()

    def emit_dir(d, child_off):
        named, ids = _sort_keys(d)
        out.extend(struct.pack("<IIHHHH", 0, 0, 0, 0, len(named), len(ids)))
        for k in named + ids:
            name_val = (0x80000000 | string_off[k]) if isinstance(k, str) else k
            out.extend(struct.pack("<II", name_val, child_off(k)))

    emit_dir(tree, lambda tk: 0x80000000 | type_dir_off[tk])
    for tk in type_keys:
        emit_dir(tree[tk], lambda nk, tk=tk: 0x80000000 | name_dir_off[(tk, nk)])
    for tk in type_keys:
        nn, ni = _sort_keys(tree[tk])
        for nk in nn + ni:
            emit_dir(tree[tk][nk], lambda lk, tk=tk, nk=nk: data_entry_off[(tk, nk, lk)])
    for tk in type_keys:
        nn, ni = _sort_keys(tree[tk])
        for nk in nn + ni:
            for lk in sorted(tree[tk][nk]):
                blob, cp = tree[tk][nk][lk]
                out.extend(struct.pack("<IIII", base_rva + blob_off[(tk, nk, lk)], len(blob), cp, 0))
    assert len(out) == strings_start
    out.extend(strings)
    out.extend(b"\0" * (blobs_start - len(out)))
    out.extend(blobs)
    return bytes(out)


def _align(v, a):
    return (v + a - 1) // a * a


def merge_resources(base, *sources):
    """Return base with the union of its resources and those of sources.

    Later sources override earlier ones. The MUI resource type is dropped.
    The manifest (RT_MANIFEST) of base is kept: the .mun file has a manifest
    that only contains "placeholder". Without the real manifest, Wine 10 and
    later give Paint comctl32 version 5, and Paint crashes at startup.
    """
    tree = {}
    for data in (base,) + sources:
        for tk, names in PE(data).resources().items():
            if tk == "MUI" or (tk == RT_MANIFEST and data is not base):
                continue
            for nk, langs in names.items():
                tree.setdefault(tk, {}).setdefault(nk, {}).update(langs)
    img = bytearray(base)
    pe = PE(base)
    if pe.magic != 0x20B:
        raise PatchError("only 64-bit (PE32+) files are supported")
    fh = pe.pe_off + 4
    opt = pe.opt
    sec_align = struct.unpack_from("<I", img, opt + 32)[0]
    file_align = struct.unpack_from("<I", img, opt + 36)[0]
    size_of_headers = struct.unpack_from("<I", img, opt + 60)[0]
    opt_size = struct.unpack_from("<H", img, fh + 16)[0]
    sec_tab = opt + opt_size
    last = sec_tab + 40 * (pe.nsec - 1)
    last_vsize, last_va = struct.unpack_from("<II", img, last + 8)
    new_hdr = sec_tab + 40 * pe.nsec
    if new_hdr + 40 > size_of_headers:
        raise PatchError("no room for a new section header")
    new_va = _align(last_va + last_vsize, sec_align)
    payload = _build_rsrc(tree, new_va)
    raw_ptr = _align(len(img), file_align)
    raw_size = _align(len(payload), file_align)
    img.extend(b"\0" * (raw_ptr - len(img)))
    img.extend(payload)
    img.extend(b"\0" * (raw_size - len(payload)))
    struct.pack_into("<8sIIIIIIHHI", img, new_hdr, b".rsrc", len(payload), new_va, raw_size, raw_ptr,
                     0, 0, 0, 0, 0x40000040)
    struct.pack_into("<H", img, fh + 2, pe.nsec + 1)
    struct.pack_into("<I", img, opt + 56, _align(new_va + len(payload), sec_align))  # SizeOfImage
    struct.pack_into("<I", img, opt + 64, 0)                                         # CheckSum
    struct.pack_into("<II", img, opt + 112 + 8 * RES_DIR, new_va, len(payload))
    struct.pack_into("<II", img, opt + 112 + 8 * 4, 0, 0)                            # drop Authenticode
    return bytes(img)


# ---------------------------------------------------------- import rename --

def rename_import(data, old, new):
    """Rename an imported DLL (normal or delay import). new must not be longer."""
    old_b, new_b = old.encode(), new.encode()
    if len(new_b) > len(old_b):
        raise PatchError(f"{new} is longer than {old}")
    img = bytearray(data)
    done = 0
    for kind, off, name in PE(data).import_names():
        if name.lower() == old_b.lower():
            img[off:off + len(old_b)] = new_b + b"\0" * (len(old_b) - len(new_b))
            done += 1
    if not done:
        raise PatchError(f"{old} is not imported")
    return bytes(img)


def imports_of(data):
    return [name.decode() for _, _, name in PE(data).import_names()]


# ---------------------------------------------------- mspaint.exe patches --
# Each patch is found by a byte pattern ("??" = any byte), so that other
# builds of mspaint.exe with the same code also work. A pattern must match
# exactly once. "at" is the position of the patched bytes in the pattern.

CODE_PATCHES = [
    {
        "name": "GDI+ version 3 -> 2 (Wine's GdiplusStartup rejects version 3, so every save failed)",
        "section": b".text",
        # Builds before 10.0.19041.4046 always start GDI+ with version 2; they do not need this.
        "optional": True,
        # lea rcx,[rip+x]; test al,al; je +x; mov dword [rbp+x],3; mov [rbp+x],rdi
        "pattern": "48 8d 0d ?? ?? ?? ?? 84 c0 74 ?? c7 45 ?? 03 00 00 00 48 89 7d ?? 48 89 7d ??",
        "at": 14, "old": "03", "new": "02",
    },
    {
        "name": "default folder routine always resolves a known folder",
        "section": b".text",
        # call [rip+x]; nop5; test ebx,ebx; je +0xbe -> 6 x nop
        "pattern": "48 ff 15 ?? ?? ?? ?? 0f 1f 44 00 00 85 db 0f 84 ?? ?? 00 00 48 83 64 24 ?? 00 4c 8d 4c 24 ?? 45 33 c0",
        "at": 14, "old": None, "new": "90 90 90 90 90 90",
    },
    {
        "name": "skip the Windows Information Protection block in Save As (Wine stubs)",
        "section": b".text",
        # js err; mov [rsp+0x88],r14; >> mov rcx,[rsp+0x88]; test rcx,rcx; je; ...
        # The block ends after "call PSGetPropertyDescriptionListFromString" and
        # "call IFileSaveDialog::SetCollectedProperties" (vtable +0xe8), each with its js.
        "pattern": "0f 88 ?? ?? 00 00 4c 89 b4 24 88 00 00 00 48 8b 8c 24 88 00 00 00 48 85 c9 74 ??",
        "at": 14, "old": "48 8b 8c 24 88 00 00 00", "new": "jmp-wip",
    },
]

FOLDERID_PICTURES = bytes.fromhex("3081e2331e4e7646835a98395c3bc3bb")
FOLDERID_DOWNLOADS = bytes.fromhex("90e24d373f126545916439c4925e467b")


def _compile(pattern):
    parts = []
    for tok in pattern.split():
        parts.append(b"." if tok == "??" else re.escape(bytes.fromhex(tok)))
    return re.compile(b"".join(parts), re.S)


def _find_once(data, pe, pattern, section):
    hits = [m.start() for m in _compile(pattern).finditer(data)
            if pe.section_of_rva(pe.off_to_rva(m.start()) or 0) == section]
    return hits


def _wip_jump(data, pe, start):
    """Return the jmp that skips the WIP block that starts at file offset start.

    The block ends with: mov rax,[rax+0xe8]; call [rip+x]; mov rcx,[rsp+x]; test eax,eax; js x.
    The jump goes to the instruction after that js.
    """
    end_pat = _compile("48 8b 80 e8 00 00 00 ff 15 ?? ?? ?? ?? 48 8b 8c 24 ?? ?? 00 00 85 c0 0f 88 ?? ?? 00 00")
    m = end_pat.search(data, start, start + 0x200)
    if not m:
        raise PatchError("end of the WIP block not found")
    target = m.end()
    rel = target - (start + 5)
    return b"\xe9" + struct.pack("<i", rel) + b"\x90" * 3


RIBBON_PATCHES = [
    {
        "name": "user32 ordinal 2714 lookup returns NULL (Wine 10+ has a stub there that raises an exception)",
        "section": b".text",
        # mov edx,0xa9a; mov rcx,rax; >> call [GetProcAddress] -> xor eax,eax + nops.
        # UIRibbon.dll caches the pointer and does not call it when it is NULL, as on Wine 9.
        "pattern": "ba 9a 0a 00 00 48 8b c8 48 ff 15 ?? ?? ?? ??",
        "at": 8, "old": None, "new": "31 c0 90 90 90 90 90",
    },
]


def patch_code(data, patches):
    img = bytearray(data)
    pe = PE(data)
    log = []
    for p in patches:
        hits = _find_once(data, pe, p["pattern"], p["section"])
        if not hits and p.get("optional"):
            log.append(f"{p['name']}: not needed for this build")
            continue
        if len(hits) != 1:
            raise PatchError(f"patch '{p['name']}': pattern found {len(hits)} times, expected 1. "
                             "This build is not supported.")
        at = hits[0] + p["at"]
        if p["old"] is not None:
            old = bytes.fromhex(p["old"])
            if data[at:at + len(old)] != old:
                raise PatchError(f"patch '{p['name']}': unexpected bytes")
        new = _wip_jump(data, pe, at) if p["new"] == "jmp-wip" else bytes.fromhex(p["new"])
        img[at:at + len(new)] = new
        log.append(f"{p['name']}: RVA {pe.off_to_rva(at):#x}")
    return bytes(img), log


def patch_mspaint_code(data, downloads_folder=True):
    data, log = patch_code(data, CODE_PATCHES)
    if downloads_folder:
        n = data.count(FOLDERID_PICTURES)
        if n != 1:
            raise PatchError(f"FOLDERID_Pictures found {n} times, expected 1")
        at = data.index(FOLDERID_PICTURES)
        data = data[:at] + FOLDERID_DOWNLOADS + data[at + 16:]
        log.append(f"default folder Pictures -> Downloads: RVA {PE(data).off_to_rva(at):#x}")
    return data, log


def patch_font_xml(data):
    """Make the text-tool font XML safe for Wine's msxml.

    The XML declares a DTD default for the Language attribute subLangId. For a
    defaulted attribute, Wine's msxml getNamedItem returns S_OK with a NULL node
    and Paint dereferences it, so selecting the Text tool crashes Paint. Every
    <Language> gets an explicit subLangId. The comment and line breaks are
    removed, so the result is not longer than the original. Spaces pad it.
    """
    data = bytearray(data)
    start = data.find(b'<?xml version="1.0"?>\r\n<!DOCTYPE root [')
    if start < 0:
        raise PatchError("font XML not found")
    end = data.find(b"</root>", start) + len(b"</root>")
    xml = bytes(data[start:end])
    dtd = re.search(rb"<!DOCTYPE root \[.*?\]>", xml, re.S).group(0)
    dtd = re.sub(rb"\s+", b" ", dtd)
    body = xml[xml.index(b"<root>"):]
    body = re.sub(rb">\s+<", b"><", body)
    body = re.sub(rb'<Language (primaryLangId="[^"]*")>', rb'<Language \1 subLangId="0">', body)
    body = body.replace(b'" />', b'"/>')
    new = b'<?xml version="1.0"?>' + dtd + body
    if len(new) > len(xml) or re.search(rb'<Language primaryLangId="[^"]*">', new):
        raise PatchError("font XML rewrite failed")
    data[start:end] = new + b" " * (len(xml) - len(new))
    return bytes(data)


# ------------------------------------------------------------------ icon --

def _png_chunk(kind, payload):
    return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", zlib.crc32(kind + payload))


def _dib_to_png(dib):
    size, w, h, planes, bpp = struct.unpack_from("<IiiHH", dib)
    h //= 2  # XOR mask + AND mask
    if bpp != 32 or w <= 0 or h <= 0:
        return None
    px = dib[size:size + w * h * 4]
    rows = []
    for y in range(h - 1, -1, -1):
        row = px[y * w * 4:(y + 1) * w * 4]
        rgba = bytearray(len(row))
        rgba[0::4], rgba[1::4], rgba[2::4], rgba[3::4] = row[2::4], row[1::4], row[0::4], row[3::4]
        rows.append(b"\0" + bytes(rgba))
    return (b"\x89PNG\r\n\x1a\n" + _png_chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
            + _png_chunk(b"IDAT", zlib.compress(b"".join(rows), 9)) + _png_chunk(b"IEND", b""))


def extract_icon_png(data):
    res = PE(data).resources()
    groups, icons = res.get(14, {}), res.get(3, {})
    if not groups:
        return None
    named, ids = _sort_keys(groups)
    group = next(iter(groups[(ids + named)[0]].values()))[0]
    count = struct.unpack_from("<H", group, 4)[0]
    best = None
    for i in range(count):
        w, h, _, _, _, bpp, size, icon_id = struct.unpack_from("<BBBBHHIH", group, 6 + 14 * i)
        w = w or 256
        if icon_id in icons and (best is None or (w, bpp) > best[:2]):
            best = (w, bpp, icon_id)
    if not best:
        return None
    blob = next(iter(icons[best[2]].values()))[0]
    return blob if blob.startswith(b"\x89PNG") else _dib_to_png(blob)


# ----------------------------------------------------------------- fonts --

DEFAULT_FONTS = [
    "arial", "arialbd", "arialbi", "ariali", "ariblk", "calibri", "calibrib", "calibrii", "calibriz",
    "cambria", "cambriab", "cambriai", "cambriaz", "candara", "candarab", "candarai", "candaraz",
    "comic", "comicbd", "comici", "comicz", "consola", "consolab", "consolai", "consolaz",
    "constan", "constanb", "constani", "constanz", "corbel", "corbelb", "corbeli", "corbelz",
    "cour", "courbd", "courbi", "couri", "framd", "framdit", "gabriola", "georgia", "georgiab",
    "georgiai", "georgiaz", "impact", "lucon", "l_10646", "micross", "pala", "palab", "palabi",
    "palai", "segoepr", "segoeprb", "segoesc", "segoescb", "segoeui", "segoeuib", "segoeuii",
    "segoeuil", "segoeuisl", "segoeuiz", "seguisb", "seguili", "seguisbi", "seguisli", "seguiemj",
    "seguisym", "sylfaen", "symbol", "tahoma", "tahomabd", "times", "timesbd", "timesbi", "timesi",
    "trebuc", "trebucbd", "trebucbi", "trebucit", "verdana", "verdanab", "verdanai", "verdanaz",
    "webdings", "wingding", "marlett", "javatext", "mvboli", "holomdl2", "segmdl2",
]


def _font_full_names(data):
    """Return the full names (name ID 4) of the fonts in a .ttf/.otf/.ttc file."""
    offsets = [0]
    if data[:4] == b"ttcf":
        n = struct.unpack_from(">I", data, 8)[0]
        offsets = list(struct.unpack_from(f">{n}I", data, 12))
    names = []
    for base in offsets:
        num_tables = struct.unpack_from(">H", data, base + 4)[0]
        for i in range(num_tables):
            tag, _, off, length = struct.unpack_from(">4sIII", data, base + 12 + 16 * i)
            if tag != b"name":
                continue
            count, str_off = struct.unpack_from(">HH", data, off + 2)
            found = {}
            for j in range(count):
                plat, enc, lang, nid, ln, so = struct.unpack_from(">HHHHHH", data, off + 6 + 12 * j)
                if nid != 4:
                    continue
                raw = data[off + str_off + so:off + str_off + so + ln]
                if plat == 3 and enc in (0, 1, 10):
                    found[(0 if lang == 0x409 else 1, 0)] = raw.decode("utf-16-be", "replace")
                elif plat == 1 and enc == 0:
                    found[(2, 0)] = raw.decode("mac_roman", "replace")
            if found:
                names.append(found[min(found)])
    return names


def font_reg(font_dir, out_path):
    lines = ["REGEDIT4", "", r"[HKEY_LOCAL_MACHINE\Software\Microsoft\Windows NT\CurrentVersion\Fonts]"]
    for fn in sorted(os.listdir(font_dir)):
        if not fn.lower().endswith((".ttf", ".ttc", ".otf")):
            continue
        try:
            names = _font_full_names(open(os.path.join(font_dir, fn), "rb").read())
        except (struct.error, IndexError):
            continue
        if not names:
            continue
        kind = "OpenType" if fn.lower().endswith(".otf") else "TrueType"
        value = " & ".join(dict.fromkeys(names)) + f" ({kind})"
        lines.append('"%s"="%s"' % (value.replace("\\", "\\\\").replace('"', '\\"'), fn))
    with open(out_path, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\n".join(lines) + "\n")
    return len(lines) - 3


# ---------------------------------------------------------------- gather --

def _ci_path(root, rel):
    """Resolve rel (with / separators) under root without regard to case."""
    cur = root
    for part in rel.split("/"):
        try:
            entries = os.listdir(cur)
        except OSError:
            return None
        match = next((e for e in entries if e.lower() == part.lower()), None)
        if match is None:
            return None
        cur = os.path.join(cur, match)
    return cur


# (file name in SRC_DIR, path under the Windows directory, language-specific)
SOURCE_FILES = [
    ("mspaint.exe", "System32/mspaint.exe", False),
    ("mspaint.exe.mun", "SystemResources/mspaint.exe.mun", False),
    ("mspaint.exe.mui", "System32/{lang}/mspaint.exe.mui", True),
    ("UIRibbon.dll", "System32/UIRibbon.dll", False),
    ("UIRibbon.dll.mui", "System32/{lang}/UIRibbon.dll.mui", True),
    ("UIRibbonRes.dll", "System32/UIRibbonRes.dll", False),
    ("mfc42u.dll", "System32/mfc42u.dll", False),
    ("DismApi.dll", "System32/DismApi.dll", False),
    ("fms.dll", "System32/fms.dll", False),
    ("fms.dll.mui", "System32/{lang}/fms.dll.mui", True),
]


def find_windows_dir(path):
    """Accept a drive root, a Windows directory or an extracted image root."""
    for cand in (path, _ci_path(path, "Windows")):
        if cand and _ci_path(cand, "System32/mspaint.exe"):
            return cand
    return None


def pick_language(win):
    system32 = _ci_path(win, "System32")
    langs = [d for d in os.listdir(system32)
             if re.fullmatch(r"[a-z]{2,3}(-[A-Za-z]{2,4})*(-[A-Za-z0-9]+)?", d, re.I)
             and _ci_path(system32, d + "/mspaint.exe.mui")]
    for pref in ("en-US", "en-GB"):
        for d in langs:
            if d.lower() == pref.lower():
                return d
    return sorted(langs)[0] if langs else None


def gather(path, src_dir, fonts="default"):
    win = find_windows_dir(path)
    if not win:
        raise PatchError(f"no Windows\\System32\\mspaint.exe found under {path}")
    lang = pick_language(win)
    if not lang:
        raise PatchError("no language folder with mspaint.exe.mui found in System32")
    os.makedirs(src_dir, exist_ok=True)
    missing = []
    for dst, rel, _ in SOURCE_FILES:
        p = _ci_path(win, rel.format(lang=lang))
        if not p:
            missing.append(rel.format(lang=lang))
            continue
        shutil.copyfile(p, os.path.join(src_dir, dst))
    if missing:
        raise PatchError("missing files: " + ", ".join(missing))
    copied = 0
    font_src = _ci_path(win, "Fonts")
    if fonts != "none" and font_src:
        fdst = os.path.join(src_dir, "fonts")
        os.makedirs(fdst, exist_ok=True)
        want = set(DEFAULT_FONTS)
        for fn in os.listdir(font_src):
            stem, ext = os.path.splitext(fn.lower())
            if ext in (".ttf", ".ttc", ".otf") and (fonts == "all" or stem in want):
                shutil.copyfile(os.path.join(font_src, fn), os.path.join(fdst, fn.lower()))
                copied += 1
    return lang, copied


# ----------------------------------------------------------------- build --

def _read(src, name):
    with open(os.path.join(src, name), "rb") as f:
        return f.read()


def _write(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def build(src, out, downloads_folder=True):
    # mspaint.exe: feature-staging import -> featstage.dll (our stub + clipboard/text shim),
    # merge .mun + .mui resources, code patches, font XML fix.
    exe = _read(src, "mspaint.exe")
    if "featstage.dll" not in [n.lower() for n in imports_of(exe)]:
        exe = rename_import(exe, "api-ms-win-core-featurestaging-l1-1-0.dll", "featstage.dll")
    exe = merge_resources(exe, _read(src, "mspaint.exe.mun"), _read(src, "mspaint.exe.mui"))
    exe, log = patch_mspaint_code(exe, downloads_folder)
    exe = patch_font_xml(exe)
    _write(os.path.join(out, "app", "mspaint.exe"), exe)

    # mfc42u.dll: Wine 9.0 maps api-ms-win-core-com-l2-1-1 to coml2.dll, where
    # StgCreateDocfileOnILockBytes is a stub. ole32.dll implements it.
    mfc = _read(src, "mfc42u.dll")
    mfc = rename_import(mfc, "api-ms-win-core-com-l2-1-1.dll", "ole32.dll")
    _write(os.path.join(out, "app", "mfc42u.dll"), mfc)
    _write(os.path.join(out, "app", "DismApi.dll"), _read(src, "DismApi.dll"))

    # uiribbon.dll: merge .mui, user32 -> U32SHM.dll (user32 proxy with the missing ordinals).
    rib = merge_resources(_read(src, "UIRibbon.dll"), _read(src, "UIRibbon.dll.mui"))
    rib = rename_import(rib, "USER32.dll", "U32SHM.dll")
    rib, rib_log = patch_code(rib, RIBBON_PATCHES)
    log += rib_log
    _write(os.path.join(out, "system32", "uiribbon.dll"), rib)
    _write(os.path.join(out, "system32", "uiribbonres.dll"), _read(src, "UIRibbonRes.dll"))

    # fms.dll (font list in the ribbon): merge .mui.
    _write(os.path.join(out, "system32", "fms.dll"),
           merge_resources(_read(src, "fms.dll"), _read(src, "fms.dll.mui")))

    icon = extract_icon_png(exe)
    if icon:
        _write(os.path.join(out, "mspaint.png"), icon)
    return log


def main(argv):
    if len(argv) >= 3 and argv[0] == "gather":
        fonts = "default"
        if "--fonts" in argv:
            fonts = argv[argv.index("--fonts") + 1]
        lang, n = gather(argv[1], argv[2], fonts)
        print(f"copied the Paint files ({lang}) and {n} fonts")
    elif len(argv) >= 3 and argv[0] == "build":
        for line in build(argv[1], argv[2], "--pictures" not in argv):
            print("  " + line)
    elif len(argv) == 3 and argv[0] == "fontreg":
        print(f"registered {font_reg(argv[1], argv[2])} font files")
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    try:
        main(sys.argv[1:])
    except PatchError as e:
        sys.exit(f"error: {e}")
