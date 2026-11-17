#include "webfiles.h"

#include <WebServer.h>
#include <WiFi.h>

#include "board.h"
#include "net.h"
#include "safewrite.h"
#include "settings.h"
#include "ui.h"

namespace webfiles {
namespace {

constexpr char kDefaultSsid[] = "t5-epaper";
// 既定のパスワード。ソースを公開しているので秘密にはならない。あえて、見るからに秘密でない
// 値にしてある。他人につながれたくない場合は、設定アプリの「WiFi 転送」で変える。
constexpr char kDefaultPassword[] = "12345678";
constexpr char kTempName[] = ".upload.tmp";  // 受信中の一時ファイル名
constexpr size_t kMaxPathLength = 200;

WebServer g_server(80);
bool g_running = false;
bool g_accessPoint = false;
uint32_t g_requests = 0;

// WiFi 用 QR コードの中で特別な意味を持つ文字を「\」で打ち消す。
String qrEscape(const String &s)
{
    String out;
    for (size_t i = 0; i < s.length(); ++i) {
        const char c = s[i];
        if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') {
            out += '\\';
        }
        out += c;
    }
    return out;
}

// アップロードの途中経過
SafeWriter g_uploadWriter;
String g_uploadTemp;
String g_uploadFinal;
bool g_uploadFailed = false;

// ブラウザに返す画面。スマホでも PC でも使えるようにする。
// ファイル名は textContent で入れ、HTML として解釈させない。
const char kPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="ja"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>t5-epaper ファイル</title>
<style>
*{box-sizing:border-box}
body{font-family:system-ui,sans-serif;max-width:860px;margin:0 auto;padding:12px 14px 40px;color:#111;background:#fff}
header{display:flex;align-items:center;gap:8px;flex-wrap:wrap}
h1{font-size:19px;margin:8px 0;flex:1}
#storage{font-size:13px;color:#555}
#path{font-size:15px;margin:10px 0;word-break:break-all}
#path a,.name.dir{color:#06c;cursor:pointer}
button{font-size:15px;padding:9px 14px;border:1px solid #666;border-radius:8px;background:#f4f4f4;color:#111;cursor:pointer}
button.primary{background:#06c;border-color:#06c;color:#fff}
button.danger{border-color:#b00;color:#b00;background:#fff}
#drop{margin:10px 0;padding:14px;border:2px dashed #999;border-radius:10px;text-align:center;color:#555}
#drop.over{background:#eef;border-color:#06c}
#progress{display:none;margin:10px 0;align-items:center;gap:8px}
#progress.show{display:flex}
#barwrap{flex:1;height:22px;border:1px solid #666;border-radius:6px;overflow:hidden;position:relative;background:#eee}
#bar{height:100%;width:0;background:#06c}
#bartext{position:absolute;inset:0;text-align:center;font-size:13px;line-height:20px;color:#111;mix-blend-mode:difference;filter:invert(1)}
#msg{min-height:1.3em;color:#b00;margin:6px 0;font-size:14px}
.row{display:flex;align-items:center;gap:10px;padding:13px 4px;border-bottom:1px solid #ddd;cursor:pointer}
.row:hover{background:#f6f6f6}
.name{flex:1;word-break:break-all}
.size{white-space:nowrap;color:#555;font-size:13px}
.overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.5);align-items:center;justify-content:center;padding:16px;z-index:10}
.overlay.show{display:flex}
.dialog{background:#fff;border-radius:12px;padding:18px;width:100%;max-width:380px;display:flex;flex-direction:column;gap:10px}
.dialog h2{font-size:16px;margin:0 0 4px;word-break:break-all}
.dialog input{font-size:16px;padding:9px;border:1px solid #666;border-radius:8px;width:100%}
#preview .dialog{max-width:860px;max-height:92vh}
#pbody{overflow:auto;text-align:center}
#pbody img{max-width:100%}
#pbody pre{text-align:left;white-space:pre-wrap;word-break:break-all;font-size:13px;margin:0}
@media (prefers-color-scheme:dark){body{background:#111;color:#eee}button{background:#222;color:#eee;border-color:#888}
button.danger{background:#111;color:#f77;border-color:#f77}.row{border-color:#333}.row:hover{background:#1c1c1c}
.size,#storage,#drop{color:#aaa}#path a,.name.dir{color:#6af}#drop.over{background:#223}#msg{color:#f77}
.dialog{background:#1b1b1b;color:#eee}.dialog input{background:#111;color:#eee}#barwrap{background:#222}}
</style></head><body>
<header><h1>t5-epaper ファイル</h1><span id="storage"></span></header>
<div id="path"></div>
<div style="display:flex;gap:8px;flex-wrap:wrap">
<button class="primary" id="pick">アップロード</button>
<button id="mkdir">新しいフォルダ</button>
<input type="file" id="files" multiple hidden>
</div>
<div id="drop">ファイルをここにドロップしてもアップロードできます</div>
<div id="progress"><div id="barwrap"><div id="bar"></div><div id="bartext">0%</div></div><button id="cancel">✕ 中止</button></div>
<div id="msg"></div>
<div id="list"></div>

<div class="overlay" id="menu"><div class="dialog">
<h2 id="mtitle"></h2>
<button id="mopen">開く</button>
<button id="mpreview">プレビュー</button>
<button id="mdownload">ダウンロード</button>
<button id="mrename">名前の変更</button>
<button class="danger" id="mdelete">削除</button>
<button id="mclose">閉じる</button>
</div></div>

<div class="overlay" id="ask"><div class="dialog">
<h2 id="atitle"></h2>
<input id="ainput" autocomplete="off">
<button class="primary" id="aok">決定</button>
<button id="acancel">キャンセル</button>
</div></div>

<div class="overlay" id="preview"><div class="dialog">
<h2 id="ptitle"></h2>
<div id="pbody"></div>
<button id="pclose">閉じる</button>
</div></div>

<script>
let cur='/',sel=null,xhr=null,stopped=false;
const $=id=>document.getElementById(id),enc=encodeURIComponent;
const join=(d,n)=>d==='/'?'/'+n:d+'/'+n;
const msg=t=>{$('msg').textContent=t||''};
const show=(id,on)=>$(id).classList.toggle('show',on);
const fmt=n=>n>=1073741824?(n/1073741824).toFixed(1)+' GB':n>=1048576?(n/1048576).toFixed(1)+' MB':n>=1024?Math.round(n/1024)+' KB':n+' B';
const ext=n=>(n.includes('.')?n.split('.').pop():'').toLowerCase();
const isImage=n=>['png','jpg','jpeg','gif','bmp'].includes(ext(n));
const isText=n=>['txt','log','csv','json','md','ini','config','cfg','xml','html','htm','js','css','yaml','yml'].includes(ext(n));
function el(tag,text,cls){const e=document.createElement(tag);if(text!==undefined)e.textContent=text;if(cls)e.className=cls;return e}
async function post(url){try{const r=await fetch(url,{method:'POST'});if(!r.ok)msg(await r.text());return r.ok}catch(e){msg('本体に接続できません');return false}}
async function storage(){try{const j=await(await fetch('/api/storage')).json();
  $('storage').textContent='空き '+fmt(j.total-j.used)+' / 全体 '+fmt(j.total)}catch(e){}}
async function load(p){
  let r;try{r=await fetch('/api/list?path='+enc(p))}catch(e){msg('本体に接続できません');return}
  if(!r.ok){msg('フォルダを開けません: '+p);return}
  const j=await r.json();cur=j.path;
  const path=$('path');path.textContent='';
  const root=el('a','SD カード');root.onclick=()=>load('/');path.append(root);
  let acc='';for(const part of cur.split('/').filter(Boolean)){acc+='/'+part;const here=acc;
    path.append(' / ');const a=el('a',part);a.onclick=()=>load(here);path.append(a)}
  const list=$('list');list.textContent='';
  j.entries.sort((a,b)=>a.dir!==b.dir?(a.dir?-1:1):a.name.localeCompare(b.name,'ja'));
  if(!j.entries.length)list.append(el('div','このフォルダは空です','size'));
  for(const e of j.entries){
    const row=el('div',undefined,'row');
    row.append(el('span',(e.dir?'📁 ':'')+e.name,'name'+(e.dir?' dir':'')),el('span',e.dir?'':fmt(e.size),'size'));
    row.onclick=()=>openMenu(e);list.append(row)}
}
function openMenu(e){sel=e;$('mtitle').textContent=e.name;
  $('mopen').hidden=!e.dir;$('mdownload').hidden=e.dir;
  $('mpreview').hidden=e.dir||!(isImage(e.name)||isText(e.name));show('menu',true)}
$('mclose').onclick=()=>show('menu',false);
$('menu').onclick=e=>{if(e.target===$('menu'))show('menu',false)};
$('mopen').onclick=()=>{show('menu',false);load(join(cur,sel.name))};
$('mdownload').onclick=()=>{show('menu',false);location.href='/api/download?path='+enc(join(cur,sel.name))};
$('mdelete').onclick=async()=>{show('menu',false);
  if(!confirm((sel.dir?'フォルダ':'ファイル')+'「'+sel.name+'」を削除しますか？\n削除すると元に戻せません。'))return;
  msg('');await post('/api/delete?path='+enc(join(cur,sel.name)));load(cur);storage()};
$('mpreview').onclick=async()=>{show('menu',false);
  const url='/api/view?path='+enc(join(cur,sel.name)),body=$('pbody');body.textContent='';$('ptitle').textContent=sel.name;
  if(isImage(sel.name)){const i=el('img');i.src=url;body.append(i)}
  else{try{const t=await(await fetch(url)).text();body.append(el('pre',t.length>200000?t.slice(0,200000)+'\n…（以下省略）':t))}catch(e){body.textContent='読み込めません'}}
  show('preview',true)};
$('pclose').onclick=()=>{show('preview',false);$('pbody').textContent=''};
function ask(title,value,done){$('atitle').textContent=title;$('ainput').value=value;show('ask',true);$('ainput').focus();$('ainput').select();
  $('aok').onclick=()=>{const v=$('ainput').value.trim();show('ask',false);if(v)done(v)}}
$('acancel').onclick=()=>show('ask',false);
$('ainput').onkeydown=e=>{if(e.key==='Enter')$('aok').click()};
$('mrename').onclick=()=>{show('menu',false);const old=sel.name;ask('名前の変更',old,async v=>{
  if(v===old)return;msg('');await post('/api/rename?path='+enc(join(cur,old))+'&to='+enc(v));load(cur)})};
$('mkdir').onclick=()=>ask('新しいフォルダの名前','',async v=>{msg('');await post('/api/mkdir?path='+enc(join(cur,v)));load(cur)});
function sendOne(f,i,n){return new Promise(res=>{
  const fd=new FormData();fd.append('file',f,f.name);xhr=new XMLHttpRequest();
  xhr.open('POST','/api/upload?path='+enc(cur));
  xhr.upload.onprogress=e=>{if(!e.lengthComputable)return;const p=Math.round(e.loaded*100/e.total);
    $('bar').style.width=p+'%';$('bartext').textContent=(n>1?i+' / '+n+'　':'')+p+'%'};
  xhr.onload=()=>res(xhr.status===200?'':(xhr.responseText||'保存できませんでした'));
  xhr.onerror=()=>res('通信に失敗しました');xhr.onabort=()=>res('中止しました');
  xhr.send(fd)})}
async function send(files){
  if(xhr)return;msg('');stopped=false;show('progress',true);let i=0;
  for(const f of files){i++;$('bar').style.width='0';$('bartext').textContent='0%';
    const err=await sendOne(f,i,files.length);
    if(err){msg(f.name+': '+err);break}if(stopped)break}
  xhr=null;show('progress',false);$('files').value='';load(cur);storage()}
$('cancel').onclick=()=>{stopped=true;if(xhr)xhr.abort()};
$('pick').onclick=()=>$('files').click();
$('files').onchange=()=>{if($('files').files.length)send([...$('files').files])};
const drop=$('drop');
document.ondragover=e=>{e.preventDefault();drop.classList.add('over')};
document.ondragleave=e=>{if(!e.relatedTarget)drop.classList.remove('over')};
document.ondrop=e=>{e.preventDefault();drop.classList.remove('over');if(e.dataTransfer.files.length)send([...e.dataTransfer.files])};
load('/');storage();
</script></body></html>
)HTML";

// ---- 補助 -------------------------------------------------------------------

// 要求を受けたことを記録する。通信があるあいだはスリープしない。
// 接続の制限は WiFi のパスワードで行うので、ブラウザ側の認証はない。
void noteRequest()
{
    ++g_requests;
    ui::noteActivity();
}

void sendText(int code, const char *text)
{
    g_server.send(code, "text/plain; charset=utf-8", text);
}

// プレビュー用の種類。HTML などもそのまま文字として見せ、ブラウザに実行させない。
const char *viewMimeType(const String &path)
{
    String ext = path.substring(path.lastIndexOf('.') + 1);
    ext.toLowerCase();
    if (ext == "png") return "image/png";
    if (ext == "jpg" || ext == "jpeg") return "image/jpeg";
    if (ext == "gif") return "image/gif";
    if (ext == "bmp") return "image/bmp";
    return "text/plain; charset=utf-8";
}

// ファイル名として使えるか（フォルダの区切りや制御文字を含まない）。
bool validName(const String &name)
{
    if (name.length() == 0 || name.length() > 120 || name.startsWith(".")) {
        return false;
    }
    for (size_t i = 0; i < name.length(); ++i) {
        const uint8_t c = static_cast<uint8_t>(name[i]);
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') {
            return false;
        }
    }
    return true;
}

// SD カード上の絶対パスとして安全か。上の階層へ抜ける指定などを弾く。
bool validPath(const String &path)
{
    if (path.length() == 0 || path.length() > kMaxPathLength || path[0] != '/') {
        return false;
    }
    if (path.indexOf("//") >= 0 || path.indexOf('\\') >= 0) {
        return false;
    }
    if (path.indexOf("/../") >= 0 || path.endsWith("/..") || path.indexOf("/./") >= 0 ||
        path.endsWith("/.")) {
        return false;
    }
    for (size_t i = 0; i < path.length(); ++i) {
        if (static_cast<uint8_t>(path[i]) < 0x20) {
            return false;
        }
    }
    return path == "/" || !path.endsWith("/");
}

// 要求の path 引数を取り出す。不正なら 400 を返して false。
bool requestPath(String &path)
{
    path = g_server.arg("path");
    if (!validPath(path)) {
        g_server.send(400, "text/plain; charset=utf-8", "パスが正しくありません");
        return false;
    }
    if (!board::sdReady()) {
        g_server.send(503, "text/plain; charset=utf-8", "SD カードがありません");
        return false;
    }
    return true;
}

String joinPath(const String &dir, const String &name)
{
    return dir == "/" ? "/" + name : dir + "/" + name;
}

String jsonEscape(const String &s)
{
    String out;
    out.reserve(s.length() + 8);
    for (size_t i = 0; i < s.length(); ++i) {
        const char c = s[i];
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (static_cast<uint8_t>(c) < 0x20) {
            char buf[8];
            snprintf(buf, sizeof(buf), "\\u%04x", c);
            out += buf;
        } else {
            out += c;
        }
    }
    return out;
}

String urlEncode(const String &s)
{
    String out;
    for (size_t i = 0; i < s.length(); ++i) {
        const uint8_t c = static_cast<uint8_t>(s[i]);
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            char buf[4];
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

// ---- 各要求の処理 -----------------------------------------------------------

void handlePage()
{
    noteRequest();
    g_server.send_P(200, "text/html; charset=utf-8", kPage);
}

void handleList()
{
    String path;
    noteRequest();
    if (!requestPath(path)) {
        return;
    }
    File dir = SD.open(path);
    if (!dir || !dir.isDirectory()) {
        g_server.send(404, "text/plain; charset=utf-8", "フォルダが見つかりません");
        return;
    }

    // 件数が多いフォルダでもメモリを使い切らないよう、少しずつ送る。
    g_server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    g_server.send(200, "application/json; charset=utf-8", "");
    String chunk = "{\"path\":\"" + jsonEscape(path) + "\",\"entries\":[";
    bool first = true;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
        const String name = f.name();
        if (!name.startsWith(".")) {  // 隠しファイルと、受信中の一時ファイルは見せない
            if (!first) {
                chunk += ',';
            }
            first = false;
            chunk += "{\"name\":\"" + jsonEscape(name) + "\",\"dir\":";
            chunk += f.isDirectory() ? "true" : "false";
            chunk += ",\"size\":" + String(static_cast<uint32_t>(f.size())) + "}";
        }
        f.close();
        if (chunk.length() > 1200) {
            g_server.sendContent(chunk);
            chunk = "";
        }
    }
    dir.close();
    chunk += "]}";
    g_server.sendContent(chunk);
    g_server.sendContent("");  // 送信の終わり
}

void handleDownload()
{
    String path;
    noteRequest();
    if (!requestPath(path)) {
        return;
    }
    File file = SD.open(path);
    if (!file || file.isDirectory()) {
        g_server.send(404, "text/plain; charset=utf-8", "ファイルが見つかりません");
        return;
    }
    const String name = path.substring(path.lastIndexOf('/') + 1);
    g_server.sendHeader("Content-Disposition",
                        "attachment; filename*=UTF-8''" + urlEncode(name));
    g_server.streamFile(file, "application/octet-stream");
    file.close();
}

// アップロードの本体を受け取るたびに呼ばれる。
void handleUploadData()
{
    HTTPUpload &upload = g_server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        g_uploadFailed = true;
        g_uploadTemp = "";
        ui::noteActivity();
        const String dir = g_server.arg("path");
        // ブラウザによってはパス付きで名前を送るので、ファイル名の部分だけを使う。
        String name = upload.filename;
        name = name.substring(std::max(name.lastIndexOf('/'), name.lastIndexOf('\\')) + 1);
        if (!validPath(dir) || !validName(name) || !validPath(joinPath(dir, name)) ||
            !board::sdReady()) {
            return;
        }
        g_uploadFinal = joinPath(dir, name);
        g_uploadTemp = joinPath(dir, kTempName);
        g_uploadFailed = !g_uploadWriter.begin(g_uploadTemp);
        if (g_uploadFailed) {
            Serial.printf("[web] upload: cannot create %s\n", g_uploadTemp.c_str());
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        ui::noteActivity();
        if (!g_uploadFailed && !g_uploadWriter.write(upload.buf, upload.currentSize)) {
            g_uploadFailed = true;  // 容量不足や、やり直しても書けなかった場合
            Serial.printf("[web] upload: write failed at %u\n",
                          static_cast<unsigned>(upload.totalSize));
        }
    } else if (upload.status == UPLOAD_FILE_END || upload.status == UPLOAD_FILE_ABORTED) {
        if (upload.status == UPLOAD_FILE_ABORTED) {
            g_uploadFailed = true;
            g_uploadWriter.abort();
        } else if (!g_uploadFailed && !g_uploadWriter.finish()) {
            g_uploadFailed = true;
        }
        if (g_uploadTemp.length() == 0) {
            return;
        }
        // 最後まで受け取れたときだけ、同名の既存ファイルと置き換える。
        if (!g_uploadFailed) {
            if (SD.exists(g_uploadFinal)) {
                SD.remove(g_uploadFinal);
            }
            g_uploadFailed = !SD.rename(g_uploadTemp, g_uploadFinal);
        }
        if (g_uploadFailed) {
            g_uploadWriter.abort();
            SD.remove(g_uploadTemp);
        }
    }
}

// アップロードの受信が終わった後に呼ばれる。
void handleUploadDone()
{
    noteRequest();
    if (g_uploadFailed) {
        g_server.send(500, "text/plain; charset=utf-8", "保存できませんでした");
    } else {
        g_server.send(200, "text/plain; charset=utf-8", "OK");
    }
}

void handleDelete()
{
    String path;
    noteRequest();
    if (!requestPath(path)) {
        return;
    }
    if (path == "/") {
        g_server.send(400, "text/plain; charset=utf-8", "ここは削除できません");
        return;
    }
    File target = SD.open(path);
    if (!target) {
        g_server.send(404, "text/plain; charset=utf-8", "見つかりません");
        return;
    }
    const bool isDir = target.isDirectory();
    target.close();
    // フォルダは空のときだけ削除できる（中身ごと消すことはしない）。
    const bool ok = isDir ? SD.rmdir(path) : SD.remove(path);
    if (ok) {
        g_server.send(200, "text/plain; charset=utf-8", "OK");
    } else {
        g_server.send(409, "text/plain; charset=utf-8",
                      isDir ? "フォルダが空でないため削除できません" : "削除できませんでした");
    }
}

// ダウンロードと違い、ブラウザの中で表示させる（プレビュー用）。
void handleView()
{
    String path;
    noteRequest();
    if (!requestPath(path)) {
        return;
    }
    File file = SD.open(path);
    if (!file || file.isDirectory()) {
        sendText(404, "ファイルが見つかりません");
        return;
    }
    g_server.sendHeader("Content-Disposition", "inline");
    g_server.sendHeader("X-Content-Type-Options", "nosniff");
    g_server.streamFile(file, viewMimeType(path));
    file.close();
}

// 名前の変更。同じフォルダの中で、名前だけを変える。
void handleRename()
{
    String path;
    noteRequest();
    if (!requestPath(path)) {
        return;
    }
    const String newName = g_server.arg("to");
    if (path == "/" || !validName(newName)) {
        sendText(400, "その名前は使えません");
        return;
    }
    const String target = joinPath(path.substring(0, std::max(1, path.lastIndexOf('/'))), newName);
    const String normalized = target.startsWith("//") ? target.substring(1) : target;
    if (!validPath(normalized) || !SD.exists(path)) {
        sendText(400, "名前を変更できません");
        return;
    }
    if (SD.exists(normalized)) {
        sendText(409, "同じ名前のファイルかフォルダがすでにあります");
        return;
    }
    if (!SD.rename(path, normalized)) {
        sendText(500, "名前を変更できませんでした");
        return;
    }
    sendText(200, "OK");
}

void handleStorage()
{
    noteRequest();
    if (!board::sdReady()) {
        sendText(503, "SD カードがありません");
        return;
    }
    char json[80];
    snprintf(json, sizeof(json), "{\"total\":%llu,\"used\":%llu}",
             static_cast<unsigned long long>(SD.totalBytes()),
             static_cast<unsigned long long>(SD.usedBytes()));
    g_server.send(200, "application/json", json);
}

void handleMkdir()
{
    String path;
    noteRequest();
    if (!requestPath(path)) {
        return;
    }
    if (!validName(path.substring(path.lastIndexOf('/') + 1))) {
        sendText(400, "その名前は使えません");
        return;
    }
    if (SD.exists(path) || !SD.mkdir(path)) {
        g_server.send(409, "text/plain; charset=utf-8", "フォルダを作成できませんでした");
        return;
    }
    g_server.send(200, "text/plain; charset=utf-8", "OK");
}

void registerHandlers()
{
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    g_server.on("/", HTTP_GET, handlePage);
    g_server.on("/api/list", HTTP_GET, handleList);
    g_server.on("/api/download", HTTP_GET, handleDownload);
    g_server.on("/api/upload", HTTP_POST, handleUploadDone, handleUploadData);
    g_server.on("/api/delete", HTTP_POST, handleDelete);
    g_server.on("/api/mkdir", HTTP_POST, handleMkdir);
    g_server.on("/api/view", HTTP_GET, handleView);
    g_server.on("/api/rename", HTTP_POST, handleRename);
    g_server.on("/api/storage", HTTP_GET, handleStorage);
    g_server.onNotFound([]() { g_server.send(404, "text/plain; charset=utf-8", "Not found"); });
}

}  // namespace

bool start(bool accessPoint)
{
    if (g_running) {
        return true;
    }
    if (!board::sdReady()) {
        return false;
    }
    g_accessPoint = accessPoint;
    if (accessPoint) {
        // ふだんの WiFi 接続を止め、本体をパスワード付き (WPA2) のアクセスポイントにする。
        net::suspend();
        WiFi.mode(WIFI_AP);
        if (!WiFi.softAP(ssid().c_str(), password().c_str())) {
            net::resume();
            return false;
        }
    }
    registerHandlers();
    g_server.begin();
    g_requests = 0;
    g_running = true;
    Serial.printf("[web] access point %s, %s\n", ssid().c_str(), url().c_str());
    return true;
}

void stop()
{
    if (!g_running) {
        return;
    }
    g_server.stop();
    g_uploadWriter.abort();
    g_running = false;
    if (g_accessPoint) {
        WiFi.softAPdisconnect(true);
        net::resume();
    }
}

bool running() { return g_running; }

void tick()
{
    if (g_running) {
        g_server.handleClient();
    }
}

String ssid()
{
    const String custom = settings::transferSsid();
    return custom.length() ? custom : String(kDefaultSsid);
}

String url()
{
    return "http://" + (g_accessPoint ? WiFi.softAPIP() : WiFi.localIP()).toString() + "/";
}

String password()
{
    const String custom = settings::transferPassword();
    return custom.length() ? custom : defaultPassword();
}

String defaultSsid() { return kDefaultSsid; }

String defaultPassword() { return kDefaultPassword; }

String wifiQrText()
{
    return "WIFI:T:WPA;S:" + qrEscape(ssid()) + ";P:" + qrEscape(password()) + ";;";
}

int clientCount() { return g_running && g_accessPoint ? WiFi.softAPgetStationNum() : 0; }

uint32_t requestCount() { return g_requests; }

}  // namespace webfiles
