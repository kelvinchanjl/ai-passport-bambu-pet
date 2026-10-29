#!/usr/bin/env python3
"""v4.8 配网页 Wi-Fi 管理界面的行为测试:headless Chrome + 注入假 fetch(没有 google-chrome 就跳过)。
覆盖页面分流(configured / ?mode=wifi / ?mode=reauth)、列表渲染(当前连着+信号、已保存)、删除按钮置灰(只剩 1 个)、
满 5 个禁用添加、迁移横幅、删除确认弹层与请求体、添加流程(口令太短不发请求 / 502 文案原样显示 / 成功回列表 / 断网留在等待页)、
密码显示/隐藏,以及 SSID 载荷 <img onerror> 一路都只当文字。"""
import ast, json, os, re, shutil, subprocess, sys, tempfile

R = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
src = open(os.path.join(R, "art/setup/portal.html"), encoding="utf-8").read()
ga = open(os.path.join(R, "tools/pet/gen_assets.py"), encoding="utf-8").read()
fn = next(n for n in ast.parse(ga).body if isinstance(n, ast.FunctionDef) and n.name == "portal_multi")
ns = {"re": re}; exec(ast.get_source_segment(ga, fn), ns)
html = ns["portal_multi"](src)
chrome = shutil.which("google-chrome") or shutil.which("chromium")
if not chrome:
    print("SKIP: no chrome"); sys.exit(0)

PAY = "<img src=x onerror=document.title='PWNED'>"
HARNESS = """<script>
window.__calls=[];window.__M={};
const resp=(ok,body,st)=>Promise.resolve({ok:ok,status:st||(ok?200:500),json:()=>typeof body=='string'?Promise.reject(new Error('x')):Promise.resolve(body),text:()=>Promise.resolve(typeof body=='string'?body:JSON.stringify(body))});
window.fetch=(u,o)=>{__calls.push([u,o&&o.body]);const h=__M[u.split('?')[0]];return h?h(o):resp(true,{})};
const sleep=ms=>new Promise(r=>setTimeout(r,ms)),q=s=>document.querySelector(s),vis=id=>document.getElementById(id).style.display!='none';
const txt=id=>document.getElementById(id).textContent;
addEventListener('load',()=>setTimeout(async()=>{let out;try{out=await (async()=>{%s})()}catch(e){out={error:String(e&&e.stack||e)}}
 const p=document.createElement('pre');p.id='__out';p.textContent=JSON.stringify(out);document.body.append(p)},50));
</script>"""

def run(body, query="", pre="", size=(390, 844)):
    page = html.replace("<body>", "<body>" + HARNESS % body + (("<script>%s</script>" % pre) if pre else ""), 1)
    d = tempfile.mkdtemp(); f = os.path.join(d, "p.html"); open(f, "w", encoding="utf-8").write(page)
    # 无头 Chrome 的窗口最窄 500 px,所以把页面放进指定宽高的 iframe:iframe 里的 innerWidth 就是 390 / 320,
    # 外层页面等内层产出结果后把 #__out 抄出来(需要 --allow-file-access-from-files)。
    w = os.path.join(d, "w.html")
    open(w, "w", encoding="utf-8").write(
        '<!doctype html><body style="margin:0"><iframe id=f src="file://%s%s" style="border:0;width:%dpx;height:%dpx"></iframe><script>'
        'const t=setInterval(()=>{const o=document.getElementById("f").contentDocument.getElementById("__out");'
        'if(o){clearInterval(t);const p=document.createElement("pre");p.id="__out";p.textContent=o.textContent;p.setAttribute("data-imgs",document.getElementById("f").contentDocument.querySelectorAll("img").length);document.body.append(p);'
        'document.title=document.getElementById("f").contentDocument.title}},100)</script>' % (f, query, size[0], size[1]))
    f = w
    out = subprocess.run([chrome, "--headless=new", "--no-sandbox", "--allow-file-access-from-files", "--disable-gpu", "--virtual-time-budget=40000", "--window-size=%d,%d" % size, "--dump-dom", "file://" + f],
                         capture_output=True, text=True, timeout=120).stdout
    m = re.search(r'<pre id="__out"[^>]*>(.*?)</pre>', out, re.S)
    assert m, "页面没有产出结果:" + out[-500:]
    import html as H
    r = json.loads(H.unescape(m.group(1)))
    assert "error" not in r, r["error"]
    tm = re.search(r"<title>(.*?)</title>", out, re.S)
    assert "PWNED" not in (tm.group(1) if tm else ""), "onerror 被执行了"
    assert 'data-imgs="0"' in out, "出现了 img 元素"
    return r

def lst(items, **k):
    return "__M['/api/wifi_list']=()=>resp(true,%s);" % json.dumps(dict({"configured": True, "max": 5, "migrated": False, "items": items}, **k))
HOME = [{"ssid": "Home-5G", "connected": True, "rssi": -48}, {"ssid": PAY, "connected": False}]

# 1) configured=true → 入口页;点「管理 Wi-Fi」→ 列表
r = run("""await sleep(100);const a={home:vis('v-home'),hp:txt('hp'),first:vis('v-first')};q('#hm').click();await sleep(50);
 a.list=vis('v-list');a.title=txt('lt');a.rows=[...document.querySelectorAll('.rw .nm b')].map(e=>e.textContent);a.tags=[...document.querySelectorAll('.rw .tag')].map(e=>e.className+':'+e.textContent);
 a.sg=document.querySelector('.sg').className;a.dl=[...document.querySelectorAll('.dl')].map(e=>e.disabled);a.add=q('#ladd').disabled;a.cap=txt('lcap');a.banner=vis('lbn')&&q('#lbn').style.display!='none';
 a.calls=__calls.map(c=>c[0]);return a""", pre=lst(HOME))
assert r["home"] and r["hp"] == "线线已经设置好啦" and not r["first"] and r["list"], r
assert r["title"] == "已保存的 Wi-Fi (2/5)" and r["rows"] == ["Home-5G", PAY], r
assert r["tags"] == ["tag on:当前连着", "tag:已保存"] and r["sg"] == "sg l3", r
assert r["dl"] == [False, False] and r["add"] is False and r["cap"] == "" and r["banner"] is False, r
assert r["calls"] == ["/api/wifi_list"], r          # 只调了 wifi_list,没有 /api/scan
print("1 入口页 → 列表 ok:不扫描、SSID 载荷当文字、信号条 l3")

# 2) 只剩 1 个:删除置灰 + 常驻说明;满 5 个:添加禁用 + 说明;信号分级
r = run("q('#hm').click();await sleep(50);return {dl:[...document.querySelectorAll('.dl')].map(e=>e.disabled),cap:txt('lcap'),add:q('#ladd').disabled}", pre=lst([{"ssid": "A", "connected": False}]))
assert r["dl"] == [True] and "至少留 1 个 Wi-Fi" in r["cap"] and r["add"] is False, r
five = [{"ssid": "N%d" % i, "connected": i == 0, **({"rssi": -80} if i == 0 else {})} for i in range(5)]
r = run("q('#hm').click();await sleep(50);return {add:q('#ladd').disabled,cap:txt('lcap'),sg:q('.sg').className,t:txt('lt')}", pre=lst(five))
assert r["add"] is True and r["cap"] == "最多保存 5 个，先删掉一个再添加" and r["sg"] == "sg l1" and r["t"].endswith("(5/5)"), r
r = run("q('#hm').click();await sleep(50);return q('.sg').className", pre=lst([{"ssid": "A", "connected": True, "rssi": -68}, {"ssid": "B", "connected": False}]))
assert r == "sg l2", r
print("2 置灰/满额/信号分级 ok")

# 3) 未配置 → 首次 3 步 + 扫描;?mode=reauth → 只剩登录卡片;?mode=wifi → 直接列表
r = run("await sleep(100);return {first:vis('v-first'),hp:txt('hp'),opts:[...document.querySelectorAll('#ssid option')].map(e=>e.textContent),wifi:[...document.querySelectorAll('.wifi')].map(e=>e.style.display)}",
        pre=lst([], configured=False) + "__M['/api/scan']=()=>resp(true,[{ssid:%s,rssi:-40}]);" % json.dumps(PAY))
assert r["first"] and r["hp"] == "三步把我连上你的打印机" and r["opts"] == [PAY] and r["wifi"] == ["", ""], r
r = run("await sleep(100);return {first:vis('v-first'),hp:txt('hp'),wifi:[...document.querySelectorAll('.wifi')].map(e=>e.style.display),lan:q('#lanbox').style.display,n2:txt('n2'),calls:__calls.map(c=>c[0])}", query="?mode=reauth", pre=lst(HOME))
assert r["first"] and r["hp"] == "重新登录一下拓竹账号就好" and r["wifi"] == ["none", "none"] and r["lan"] == "none" and r["n2"] == "✓" and r["calls"] == [], r
r = run("await sleep(100);return {list:vis('v-list'),home:vis('v-home'),hp:txt('hp')}", query="?mode=wifi", pre=lst(HOME))
assert r["list"] and not r["home"] and r["hp"] == "管理工牌的 Wi-Fi", r
# wifi_list 失败(旧固件/出错)→ 退回首次流程
r = run("await sleep(100);return {first:vis('v-first')}", pre="__M['/api/wifi_list']=()=>resp(false,'x',404);")
assert r["first"], r
print("3 页面分流 ok(configured / reauth / mode=wifi / 接口失败回退)")

# 4) 迁移横幅
r = run("q('#hm').click();await sleep(50);const a={on:q('#lbn').style.display!='none',txt:q('#lbn span').textContent};q('#lbx').click();a.off=q('#lbn').style.display=='none';return a", pre=lst(HOME, migrated=True))
assert r == {"on": True, "txt": "之前保存的 Wi-Fi 已自动加入", "off": True}, r
print("4 迁移横幅 ok")

# 5) 删除:确认弹层 + 请求体 + 刷新列表 + toast;409 文案原样显示
r = run("""q('#hm').click();await sleep(50);document.querySelectorAll('.dl')[1].click();await sleep(20);
 const a={ov:q('#ov').style.display,h3:q('#ov h3').textContent,ss:q('#ov .ss').textContent,p:q('#ov p:not(.ss)').textContent};
 __M['/api/wifi_list']=()=>resp(true,{configured:true,max:5,migrated:false,items:[{ssid:'Home-5G',connected:true,rssi:-50}]});
 q('#ov .dng').click();await sleep(100);a.after=txt('lt');a.toast=q('.ts').textContent;a.post=__calls.filter(c=>c[0]=='/api/wifi_del').map(c=>c[1]);a.ov2=q('#ov').style.display;
return a""", pre=lst(HOME))
assert r["ov"] == "flex" and r["h3"] == "删除这个 Wi-Fi？" and r["ss"] == PAY and r["p"] == "删除后，工牌不会再自动连它。", r
assert r["after"] == "已保存的 Wi-Fi (1/5)" and r["toast"] == "已删除「" + PAY + "」" and r["post"] == [json.dumps({"ssid": PAY}, separators=(",", ":"), ensure_ascii=False)] and r["ov2"] == "none", r
# 当前连着的那个:弹层换标题和说明
r = run("q('#hm').click();await sleep(50);q('.dl').click();await sleep(20);return {h3:q('#ov h3').textContent,p:q('#ov p:not(.ss)').textContent,ss:q('#ov .ss').textContent}", pre=lst(HOME))
assert r["h3"] == "删除当前连着的 Wi-Fi？" and "会自动换到其他已保存的 Wi-Fi" in r["p"] and r["ss"] == "Home-5G", r
r = run("""q('#hm').click();await sleep(50);__M['/api/wifi_del']=()=>resp(false,'至少留 1 个 Wi-Fi，工牌要靠它上网；想换网络，先添加新的再删旧的',409);document.querySelectorAll('.dl')[0].click();q('#ov .dng').click();await sleep(100);
 return {er:q('#ler').textContent}""", pre=lst(HOME))
assert "至少留 1 个 Wi-Fi" in r["er"], r
print("5 删除流程 ok")

# 6) 添加:搜索结果格式、口令太短不发请求、502 文案原样显示、成功回列表、密码显示/隐藏
SCAN = "__M['/api/scan']=()=>resp(true,[{ssid:'Studio',rssi:-50},{ssid:'Home-5G',rssi:-70},{ssid:%s,rssi:-80}]);" % json.dumps(PAY)
r = run("""q('#hm').click();await sleep(50);q('#ladd').click();await sleep(100);
 const a={add:vis('v-add'),opts:[...document.querySelectorAll('#wssid option')].map(e=>e.textContent),vals:[...document.querySelectorAll('#wssid option')].map(e=>e.value),dis:q('#wssid').disabled,save:q('#asave').disabled};
 const s=q('#wssid');s.value='Studio';s.onchange();a.save2=q('#asave').disabled;
 q('#wpw').value='123';q('#asave').click();await sleep(20);a.short=q('#aer').textContent;a.nopost=!__calls.some(c=>c[0]=='/api/wifi_add');a.still=vis('v-add');
 q('#wshow').click();a.type=q('#wpw').type;a.btn=txt('wshow');q('#wshow').click();a.type2=q('#wpw').type;
 __M['/api/wifi_add']=()=>resp(false,'密码不对，再核对一下（区分大小写）',502);
 q('#wpw').value='12345678';q('#asave').click();await sleep(20);a.wait=vis('v-wait');a.wh=txt('wh');await sleep(100);a.back=vis('v-add');a.err=q('#aer').textContent;
 __M['/api/wifi_add']=()=>resp(true,{});__M['/api/wifi_list']=()=>resp(true,{configured:true,max:5,migrated:false,items:[{ssid:'Home-5G',connected:false},{ssid:'Studio',connected:true,rssi:-50}]});
 q('#asave').click();await sleep(100);a.list=vis('v-list');a.title=txt('lt');a.toast=q('.ts').textContent;a.body=__calls.filter(c=>c[0]=='/api/wifi_add').map(c=>c[1]);return a""", pre=lst(HOME) + SCAN)
assert r["add"] and r["opts"] == ["请选择网络", "Studio · 信号强", "Home-5G · 信号中 · 已保存", PAY + " · 信号弱 · 已保存"] and r["vals"][2] == "Home-5G" and r["dis"] is False and r["save"] is True and r["save2"] is False, r
assert r["short"] == "!密码至少 8 位；没有密码的网络请留空" and r["nopost"] and r["still"], r
assert r["type"] == "text" and r["btn"] == "隐藏" and r["type2"] == "password", r
assert r["wh"] == "正在连接「Studio」…" and r["back"] and r["err"] == "!密码不对，再核对一下（区分大小写）", r
assert r["list"] and r["title"] == "已保存的 Wi-Fi (2/5)" and r["toast"] == "已保存「Studio」" and len(r["body"]) == 2 and json.loads(r["body"][1]) == {"ssid": "Studio", "password": "12345678"}, r
print("6 添加流程 ok")

# 7) 同名 = 更新密码 toast;断网(fetch 抛 TypeError)留在等待页,16 s 后出现「回到列表看看」
r = run("""q('#hm').click();await sleep(50);q('#ladd').click();await sleep(100);const s=q('#wssid');s.value='Home-5G';s.onchange();q('#wpw').value='newpass99';
 __M['/api/wifi_add']=()=>resp(true,{});q('#asave').click();await sleep(100);return {toast:q('.ts').textContent}""", pre=lst(HOME) + SCAN)
assert r["toast"] == "已更新「Home-5G」的密码", r
r = run("""q('#hm').click();await sleep(50);q('#ladd').click();await sleep(100);const s=q('#wssid');s.value='Studio';s.onchange();q('#wpw').value='12345678';
 __M['/api/wifi_add']=()=>Promise.reject(new TypeError('Failed to fetch'));q('#asave').click();await sleep(200);
 const a={wait:vis('v-wait'),back0:q('#wback').style.display};await sleep(17000);a.back1=q('#wback').style.display;a.still=vis('v-wait');
 __M['/api/wifi_list']=()=>Promise.reject(new TypeError('x'));q('#wback').click();await sleep(50);a.msg=q('#wback').textContent;
 __M['/api/wifi_list']=()=>resp(true,{configured:true,max:5,migrated:false,items:[{ssid:'Home-5G',connected:false},{ssid:'Studio',connected:true,rssi:-50}]});q('#wback').click();await sleep(50);a.list=vis('v-list');a.t=txt('lt');return a""", pre=lst(HOME) + SCAN)
assert r["wait"] and r["back0"] == "none" and r["back1"] == "" and r["still"] and "稍后再点" in r["msg"] and r["list"] and r["t"] == "已保存的 Wi-Fi (2/5)", r
print("7 同名更新 / 断网留等待页 / 回到列表 ok")

# 8) 没搜到网络
r = run("q('#hm').click();await sleep(50);q('#ladd').click();await sleep(100);return {o:[...document.querySelectorAll('#wssid option')].map(e=>e.textContent),er:q('#aer').textContent,dis:q('#wssid').disabled}",
        pre=lst(HOME) + "__M['/api/scan']=()=>resp(true,[]);")
assert r["o"] == ["没找到网络"] and "没搜到 2.4G 网络" in r["er"] and r["dis"] is True, r
print("8 没搜到 ok")

# 9) 首次绑定后的等待页多一句 + 入口页的两条链接
r = run("""await sleep(100);q('#hr').click();const a={re:vis('v-first'),hp:txt('hp')};q('#hf').click();a.hp2=txt('hp');a.wifi=[...document.querySelectorAll('.wifi')].map(e=>e.style.display);
 q('#go').disabled=false;q('#go').click();await sleep(50);a.wait=document.getElementById('app').textContent;return a""", pre=lst(HOME) + "__M['/api/scan']=()=>resp(true,[]);")
assert r["re"] and r["hp"] == "重新登录一下拓竹账号就好" and r["hp2"] == "三步把我连上你的打印机" and r["wifi"] == ["", ""], r
assert "以后想加别的 Wi-Fi，再打开配网页，选「管理 Wi-Fi」。" in r["wait"] and "快好啦！" in r["wait"], r
print("9 入口页链接 / 首次绑定后提示 ok")
# 10) D1:等待页遇到 32 位无空格英文 SSID(和 10 个汉字、含 &<> 的名字)不能横向溢出,390 和 320 宽都测
LONG = "abcdefghijklmnopqrstuvwxyz012345"   # 32 位、没有空格也没有连字符(浏览器没有断行机会)
for w in (390, 320):
    for name in (LONG, "Very-Long-Network-Name-012345678", "家里的网络家里的网络家里的网络家a", "A&B<C>" * 5 + "xx"):
        r = run("""q('#hm').click();await sleep(50);q('#ladd').click();await sleep(100);const s=q('#wssid');s.value=%s;s.onchange();q('#wpw').value='12345678';
 __M['/api/wifi_add']=()=>new Promise(()=>{});q('#asave').click();await sleep(100);
 const h=q('#wh').getBoundingClientRect();return {iw:innerWidth,sw:document.documentElement.scrollWidth,bw:document.body.scrollWidth,wait:vis('v-wait'),hr:h.right,hl:h.left}""" % json.dumps(name),
                pre=lst(HOME) + "__M['/api/scan']=()=>resp(true,[{ssid:%s,rssi:-50}]);" % json.dumps(name), size=(w, 844))
        assert r["wait"] and r["iw"] == w, r
        assert r["sw"] <= r["iw"] and r["bw"] <= r["iw"] and r["hr"] <= r["iw"] and r["hl"] >= 0, ("横向溢出", w, name, r)
print("10 D1 等待页长 SSID 不溢出(390/320)ok")
# 11) D2:列表滚到底后删除失败,提示条要在视口内(出错后 scrollTo(0,0));390 和 320 宽、视口高 500
five_items = [{"ssid": "N%d" % i, "connected": i == 0, **({"rssi": -50} if i == 0 else {})} for i in range(5)]
for w in (390, 320):
    for fail in ("http", "network"):
        r = run("""q('#hm').click();await sleep(50);scrollTo(0,99999);await sleep(20);const y0=scrollY;
 __M['/api/wifi_del']=()=>%s;document.querySelectorAll('.dl')[4].click();q('#ov .dng').click();await sleep(100);
 const b=q('#ler .er').getBoundingClientRect();return {iw:innerWidth,ih:innerHeight,y0:y0,y1:scrollY,top:b.top,bottom:b.bottom,txt:q('#ler .er span').textContent}""" %
                ("resp(false,'至少留 1 个 Wi-Fi，工牌要靠它上网；想换网络，先添加新的再删旧的',409)" if fail == "http" else "Promise.reject(new TypeError('Failed to fetch'))"),
                pre=lst(five_items), size=(w, 500))
        assert r["iw"] == w and r["ih"] == 500 and r["y0"] > 0, ("页面应该先滚下去了", r)
        assert r["top"] >= 0 and r["bottom"] <= r["ih"] and r["y1"] == 0, ("提示条不在视口内", w, fail, r)
        if fail == "network":   # D4:断网类失败的文案
            assert r["txt"] == "没收到工牌的回复，可能已经删除。重新连上工牌热点后刷新页面，再点「管理 Wi-Fi」确认", r
print("11 D2 删除失败提示条在视口内(390/320,http/断网两种)ok")
# 12) D3:setup_portal.c 里 5 条错误文案必须是全角标点(没有半角 , ; ( ))
csrc = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "main", "setup_portal.c"), encoding="utf-8").read()
msgs = dict(re.findall(r'static const char (MSG_\w+)\[\] = "(.*?)";', csrc))
want = {
    "MSG_FULL": "最多保存 5 个，先删掉一个再添加",
    "MSG_LAST": "至少留 1 个 Wi-Fi，工牌要靠它上网；想换网络，先添加新的再删旧的",
    "MSG_SHORT": "密码至少 8 位；没有密码的网络请留空",
    "MSG_BADPW": "密码不对，再核对一下（区分大小写）",
    "MSG_NOAP": "连不上这个 Wi-Fi，靠近路由器再试试（只支持 2.4G）",
}
for k, v in want.items():
    assert msgs.get(k) == v, (k, msgs.get(k))
    assert not re.search(r"[,;()]", msgs[k]), (k, msgs[k])
print("12 D3 setup_portal.c 5 条错误文案全角标点 ok")
print("test_portal_wifi_v48 OK")
