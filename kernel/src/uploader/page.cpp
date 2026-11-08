// The upload server's page (spec/oxos_internal/31_upload_server.md §4): the drives and the current folder, uploads of
// files and whole folders (one PUT per file, with its path inside the folder), and a Launch button for each .xbe.
// Boot-only data.
#include <xk/uploader.h>

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

static const char UploadpPage[] = R"PAGE(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>OXOS upload</title>
<style>
body{font:15px system-ui,sans-serif;margin:0;background:#111;color:#ddd}
header{background:#1b1b1b;padding:12px 16px;border-bottom:1px solid #333}
h1{font-size:18px;margin:0;color:#8f6}
main{padding:8px 16px;max-width:900px}
button{background:#2a2a2a;color:#ddd;border:1px solid #444;border-radius:4px;padding:5px 10px;cursor:pointer}
button:hover{border-color:#8f6}
.launch{border-color:#5a3;color:#8f6}
.bar{display:flex;gap:6px;flex-wrap:wrap;align-items:center;margin:8px 0}
#path{font-family:monospace;color:#fff}
#msg{margin:8px 0;color:#fd6;min-height:1.2em}
table{width:100%;border-collapse:collapse}
td{padding:5px 6px;border-bottom:1px solid #262626}
td.size{text-align:right;color:#999;width:110px}
a{color:#8cf;cursor:pointer}
</style></head><body>
<header><h1>OXOS upload</h1></header>
<main>
<div class="bar" id="drives"></div>
<div class="bar">Folder: <span id="path"></span></div>
<div class="bar">
<button onclick="$('files').click()">Upload files</button>
<button onclick="$('folder').click()">Upload a folder</button>
<input type="file" id="files" multiple hidden onchange="upload(this.files)">
<input type="file" id="folder" webkitdirectory hidden onchange="upload(this.files)">
</div>
<div id="msg"></div>
<table id="list"></table>
</main>
<script>
let cwd = '/E';
const $ = id => document.getElementById(id);
const q = p => encodeURIComponent(p);
const say = text => $('msg').textContent = text;
const join = (folder, name) => (folder === '/' ? '' : folder) + '/' + name;

function size(n) {
  return n < 1024 ? n + ' B' : n < 1048576 ? (n / 1024).toFixed(1) + ' KB' : (n / 1048576).toFixed(1) + ' MB';
}

async function list(path) {
  const r = await fetch('/api/list?path=' + q(path));
  if (!r.ok) { say('Cannot open ' + path + ': ' + await r.text()); return; }
  cwd = path;
  $('path').textContent = path;
  const entries = (await r.json()).sort((a, b) => b.dir - a.dir || a.name.localeCompare(b.name));
  const table = $('list');
  table.innerHTML = '';
  if (path !== '/') {
    const up = path.lastIndexOf('/') > 0 ? path.slice(0, path.lastIndexOf('/')) : '/';
    row(table, '..', true, 0, () => list(up), null);
  }
  for (const e of entries) {
    const full = join(path, e.name);
    row(table, e.name, e.dir, e.size, e.dir ? () => list(full) : null,
        !e.dir && /\.xbe$/i.test(e.name) ? full : null);
  }
}

function row(table, name, dir, bytes, open, xbe) {
  const tr = table.insertRow(), cell = tr.insertCell(), sizeCell = tr.insertCell(), action = tr.insertCell();
  if (open) {
    const link = document.createElement('a');
    link.textContent = name + (dir ? '/' : '');
    link.onclick = open;
    cell.append(link);
  } else {
    cell.textContent = name;
  }
  sizeCell.className = 'size';
  sizeCell.textContent = dir ? '' : size(bytes);
  if (xbe) {
    const b = document.createElement('button');
    b.className = 'launch';
    b.textContent = 'Launch';
    b.onclick = () => launch(xbe);
    action.append(b);
  }
}

async function upload(fileList) {
  const files = [...fileList];
  if (!files.length) return;
  if (cwd === '/') { say('Open a drive first'); return; }
  let done = 0, bytes = 0;
  for (const f of files) {
    const path = f.webkitRelativePath || f.name;
    say('Uploading ' + (done + 1) + ' of ' + files.length + ': ' + path + ' (' + size(f.size) + ')');
    const r = await fetch('/api/file?path=' + q(join(cwd, path)), {method: 'PUT', body: f});
    if (!r.ok) { say('Uploading ' + path + ' failed: ' + await r.text()); return; }
    done++;
    bytes += f.size;
  }
  say('Uploaded ' + done + (done === 1 ? ' file, ' : ' files, ') + size(bytes));
  $('files').value = $('folder').value = '';
  list(cwd);
}

async function launch(path) {
  say('Launching ' + path + ' ...');
  const r = await fetch('/api/launch?path=' + q(path), {method: 'POST'});
  say(r.ok ? 'Launching ' + path + ': the console is starting it.' : 'Launch failed: ' + await r.text());
}

for (const d of ['C', 'E', 'F', 'G', 'X', 'Y', 'Z']) {
  const b = document.createElement('button');
  b.textContent = d + ':';
  b.onclick = () => list('/' + d);
  $('drives').append(b);
}
list(cwd);
</script>
</body></html>
)PAGE";

const char *UploadPage(ULONG *Length) {
    *Length = sizeof(UploadpPage) - 1;
    return UploadpPage;
}
