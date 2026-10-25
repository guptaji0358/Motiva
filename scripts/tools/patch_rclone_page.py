"""Rebrands the sign-in result page that the bundled rclone shows in the browser.

rclone (MIT) serves its OAuth result page from a Go text template compiled into
rclone.exe. This script swaps that template for a Motiva-branded one of EXACTLY the
same byte length (the binary stores string lengths in its code), so nothing else
in the executable changes. Idempotent. Usage:  python -I patch_rclone_page.py rclone.exe [...]
"""
import sys

OLD_HEAD = b'<!DOCTYPE html>\n<html lang="en">\n<head>\n<meta charset="utf-8">\n<title>{{ if .OK }}Success!'
MY_HEAD = b'<!DOCTYPE html><meta charset=utf-8><title>Motiva</title>'
OLD_LEN = 583

PAGE = (
    '<!DOCTYPE html><meta charset=utf-8><title>Motiva</title>'
    "<link rel=icon href=\"data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 9 9'%3E"
    "%3Crect width='9' height='9' rx='2' fill='%2316a34a'/%3E%3Cpath d='M2 7V2l2.5 3L7 2v5' "
    "fill='none' stroke='%23fff'/%3E%3C/svg%3E\">"
    '<body style="margin:0;height:100vh;display:grid;place-items:center;background:#0f1115;color:#ddd;'
    'font:17px sans-serif;text-align:center"><div>'
    '<b style="font-size:44px;color:#22c55e">Motiva</b>'
    '<p>{{if .OK}}Signed in. Return to Motiva.'
    '{{else}}Sign-in failed: {{.Name}}<br>{{.Description}}{{end}}'
).encode()


def main(paths):
    page = PAGE
    if len(page) > OLD_LEN:
        sys.exit("new page is %d bytes, limit %d" % (len(page), OLD_LEN))
    page = page + b' ' * (OLD_LEN - len(page))
    for p in paths:
        data = open(p, 'rb').read()
        i = data.find(OLD_HEAD)
        if i < 0:
            i = data.find(MY_HEAD)  # re-patch an earlier Motiva version
        if i < 0:
            print(p, ': template not found')
            continue
        out = data[:i] + page + data[i + OLD_LEN:]
        assert len(out) == len(data)
        open(p, 'wb').write(out)
        print(p, ': patched')


main(sys.argv[1:])
