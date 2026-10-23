#!/usr/bin/env python3
"""pdf_text.py - pull the text out of a PDF with the standard library only (no pdftotext here).

Used to check a quoted number or sentence against a paper's own text before citing it
(references/근거기반-설명-작성법.md, section 3).

  python3 tools/pdf_text.py paper.pdf                 # raw text
  python3 tools/pdf_text.py paper.pdf --despace       # PDFs that place one glyph at a time:
                                                      # 3 spaces = word gap, 1 space = none
  python3 tools/pdf_text.py paper.pdf --find "order of magnitude" [--width 300]

Reads FlateDecode content streams and the strings of their Tj/TJ operators.  Fonts with
custom encodings come out garbled; ligatures show as escapes (\\fi etc.).  Good enough to
find a sentence, not to reproduce the layout.  Keep downloaded PDFs in a fresh directory
and run this script from elsewhere (python -I).
"""
import re
import sys
import zlib


def extract(data):
    out = []
    for m in re.finditer(rb'stream\r?\n', data):
        start = m.end()
        end = data.find(b'endstream', start)
        if end < 0:
            continue
        raw = data[start:end]
        try:
            dec = zlib.decompress(raw)
        except Exception:
            try:
                dec = zlib.decompressobj().decompress(raw)
            except Exception:
                continue
        if b'TJ' not in dec and b'Tj' not in dec:
            continue
        txt = []
        for tm in re.finditer(rb'\[(.*?)\]\s*TJ|\((.*?)(?<!\\)\)\s*Tj', dec, re.S):
            if tm.group(1) is not None:
                s = b''
                for part, num in re.findall(rb'\(((?:\\.|[^\\)])*)\)|(-?\d+\.?\d*)', tm.group(1)):
                    if part:
                        s += part
                    elif num and float(num) < -200:
                        s += b' '
                txt.append(s)
            else:
                txt.append(tm.group(2))
            txt.append(b' ')
        out.append(b''.join(txt))
    t = b'\n'.join(out).decode('latin-1')
    return t.replace('\\(', '(').replace('\\)', ')')


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    t = extract(open(argv[1], 'rb').read())
    if '--despace' in argv:
        t = t.replace('   ', '\x00').replace(' ', '').replace('\x00', ' ')
    t = re.sub(r'[ \t]+', ' ', t)
    if '--find' in argv:
        key = argv[argv.index('--find') + 1]
        width = int(argv[argv.index('--width') + 1]) if '--width' in argv else 250
        flat = re.sub(r'\s+', ' ', t)
        hits = [m.start() for m in re.finditer(re.escape(key), flat)]
        print('%d hit(s) for %r' % (len(hits), key))
        for h in hits:
            print('>>', flat[max(0, h - width):h + len(key) + width])
            print()
        return 0 if hits else 1
    print(t)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
