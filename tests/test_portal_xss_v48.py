#!/usr/bin/env python3
"""v4.8: 配网页里的 SSID / 打印机名是外部字符串,必须当文本显示,不能被当 HTML 执行。
1) 静态:源页面和打补丁后的页面里,innerHTML 赋值不许拼 ${...}。
2) 动态(有 google-chrome 才跑,没有就跳过并说明):注入假 fetch,SSID/打印机名用 <img onerror> 载荷,
   dump-dom 后检查:载荷没生成 <img> 元素、没触发 onerror(document.title 没被改)、原文按文字显示。"""
import ast, os, re, shutil, subprocess, sys, tempfile

R = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
src = open(os.path.join(R, "art/setup/portal.html"), encoding="utf-8").read()
ga = open(os.path.join(R, "tools/pet/gen_assets.py"), encoding="utf-8").read()
fn = next(n for n in ast.parse(ga).body if isinstance(n, ast.FunctionDef) and n.name == "portal_multi")
ns = {"re": re}
exec(ast.get_source_segment(ga, fn), ns)
html = ns["portal_multi"](src)

BAD = re.compile(r"innerHTML\s*=[^;]*\$\{")
for name, h in (("source", src), ("patched", html)):
    assert not BAD.search(h), name + ": innerHTML 里拼了 ${...}"
print("static lint ok (source + patched)")

PAY = "<img src=x onerror=document.title='PWNED'>"
chrome = shutil.which("google-chrome") or shutil.which("chromium")
if not chrome:
    print("SKIP dynamic: no chrome"); sys.exit(0)
mock = """<script>
window.fetch=u=>Promise.resolve({ok:true,json:()=>Promise.resolve(
 u.indexOf('/api/scan')>=0?[{ssid:"%(p)s",rssi:-40},{ssid:"A&amp;B",rssi:-50}]:
 u.indexOf('/api/login')>=0?{devices:[{id:"S1",name:"%(p)s",online:true}]}:{})});
addEventListener('load',()=>setTimeout(()=>{const c=document.getElementById('code');c.value='123456';c.dispatchEvent(new Event('input'))},50));
</script>""" % {"p": PAY.replace('"', '\\"')}
page = html.replace("<body>", "<body>" + mock, 1) if "<body>" in html else mock + html
d = tempfile.mkdtemp()
f = os.path.join(d, "p.html"); open(f, "w", encoding="utf-8").write(page)
out = subprocess.run([chrome, "--headless=new", "--no-sandbox", "--disable-gpu", "--virtual-time-budget=4000",
                      "--dump-dom", "file://" + f], capture_output=True, text=True, timeout=60).stdout
assert "PWNED" not in re.search(r"<title>.*?</title>", out, re.S).group(0), "onerror 被执行了"
assert "<img" not in out.split("</header>", 1)[1].split("<script", 1)[0], "载荷变成了 img 元素"
assert "&lt;img src=x onerror=" in out, "载荷应作为文字出现在页面里"
assert "A&amp;amp;B" in out, "SSID 里的 &amp; 应按字面显示(DOM 序列化成 A&amp;amp;B)"
print("dynamic ok: payload shown as text, no <img>, title unchanged")
