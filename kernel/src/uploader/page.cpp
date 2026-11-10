// The upload server's page (spec/oxos_internal/31_upload_server.md §4): the disk (file system and size), its drives
// (FATX) or top-level folders (exFAT) and the current folder, uploads of files and whole folders (one PUT per file, with
// its path inside the folder), a Launch button for each .xbe, and a Rename and Delete button for each file and folder
// (Delete: a folder only when it is empty, the server refuses the others), and a Create Folder button in the ".." row.
// Boot-only data.
#include <xk/uploader.h>

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

static const char UploadpPage[] = R"PAGE(<!doctype html>
<html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>OXOS File Manager</title>
<style>
body{font:15px system-ui,sans-serif;margin:0;background:#111;color:#ddd}
header{background:#1b1b1b;padding:12px 16px;border-bottom:1px solid #333}
h1{font-size:18px;margin:0;color:#8f6}
main{padding:8px 16px;max-width:900px}
button{background:#2a2a2a;color:#8f6;border:1px solid #5a3;border-radius:4px;padding:5px 10px;cursor:pointer}
button:hover{border-color:#8f6}
.top{display:grid;grid-template-columns:max-content 1fr;gap:10px 12px;align-items:center;margin:8px 0}
.bar{display:flex;gap:6px;flex-wrap:wrap;align-items:center}
.label{font-weight:bold}
#path{font-family:monospace;color:#8f6}
hr{border:0;border-top:1px solid #333;margin:12px 0}
.hint{color:#999;margin:8px 0}
#msg{margin:8px 0;color:#fd6;min-height:1.2em}
table{width:100%;border-collapse:collapse}
td{padding:5px 6px;border-bottom:1px solid #262626}
td.size{text-align:right;color:#999;width:110px}
td button{margin-right:6px}
a{color:#5b3;cursor:pointer}
.file{color:#bdb}
</style></head><body>
<header><h1 id="title">OXOS File Manager</h1></header>
<main>
<div class="top">
<span class="label" id="drivesLabel">Drives:</span><div class="bar" id="drives"></div>
<span class="label">Click to upload:</span><div class="bar">
<button onclick="$('files').click()">Single File</button>
<button onclick="$('folder').click()">Entire Folder</button>
<input type="file" id="files" multiple hidden onchange="upload(this.files)">
<input type="file" id="folder" webkitdirectory hidden onchange="upload(this.files)">
</div>
<span class="label">Current Path:</span><span id="path"></span>
</div>
<div id="msg"></div>
<hr>
<div class="hint">[ Click links below to navigate and Click "Launch" to run XBE ]</div>
<table id="list"></table>
</main>
<script>
let cwd = '/E', exfat = false;
const $ = id => document.getElementById(id);
const q = p => encodeURIComponent(p);
const say = text => $('msg').textContent = text;
const join = (folder, name) => (folder === '/' ? '' : folder) + '/' + name;
// A path as the page shows it: a FATX disk has partitions, so /E/Apps reads E:\\Apps (/E reads E:\\); an exFAT disk has
// one volume whose top-level folders stand for the drives, so its paths stay as they are.
const show = path => exfat || !/^\/[^\/]/.test(path) ? path :
  path[1] + ':\\' + path.slice(3).replace(/\//g, '\\');

function size(n) {
  return n < 1024 ? n + ' B' : n < 1048576 ? (n / 1024).toFixed(1) + ' KB' : (n / 1048576).toFixed(1) + ' MB';
}

// A disk's size as its maker states it (decimal units): "8.0 GB", "2.0 TB".
function diskSize(n) {
  return n >= 1e12 ? (n / 1e12).toFixed(1) + ' TB' : (n / 1e9).toFixed(1) + ' GB';
}

async function list(path) {
  const r = await fetch('/api/list?path=' + q(path));
  if (!r.ok) { say('Cannot open ' + show(path) + ': ' + await r.text()); return; }
  cwd = path;
  $('path').textContent = show(path);
  const entries = (await r.json()).sort((a, b) => b.dir - a.dir || a.name.localeCompare(b.name));
  const table = $('list');
  table.innerHTML = '';
  if (path !== '/') {
    const up = path.lastIndexOf('/') > 0 ? path.slice(0, path.lastIndexOf('/')) : '/';
    row(table, '..', true, 0, () => list(up), null, null);
    const b = document.createElement('button');
    b.textContent = 'Create Folder';
    b.onclick = createFolder;
    table.rows[0].cells[2].append(b);
  }
  for (const e of entries) {
    const full = join(path, e.name);
    row(table, e.name, e.dir, e.size, e.dir ? () => list(full) : null,
        !e.dir && /\.xbe$/i.test(e.name) ? full : null, path === '/' ? null : full);
  }
}

// One entry: its name (a link for a folder), its size, a Delete button (not for "..", not for a drive) and, for an
// .xbe, a Launch button.
function row(table, name, dir, bytes, open, xbe, full) {
  const tr = table.insertRow(), cell = tr.insertCell(), sizeCell = tr.insertCell(), action = tr.insertCell();
  if (open) {
    const link = document.createElement('a');
    link.textContent = name + (dir ? '/' : '');
    link.onclick = open;
    cell.append(link);
  } else {
    cell.className = 'file';
    cell.textContent = name;
  }
  sizeCell.className = 'size';
  sizeCell.textContent = dir ? '' : size(bytes);
  if (full) {
    for (const [label, act] of [['Rename', () => rename(full, name)], ['Delete', () => remove(full, dir)]]) {
      const b = document.createElement('button');
      b.textContent = label;
      b.onclick = act;
      action.append(b);
    }
  }
  if (xbe) {
    const b = document.createElement('button');
    b.textContent = 'Launch';
    b.onclick = () => launch(xbe);
    action.append(b);
  }
}

async function upload(fileList) {
  const files = [...fileList];
  if (!files.length) return;
  if (cwd === '/') { say(exfat ? 'Open a folder first' : 'Open a drive first'); return; }
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

const badName = name => /[\/\\:*?"<>|]/.test(name) || name === '.' || name === '..';

async function createFolder() {
  const name = prompt('New folder in ' + show(cwd) + ':');
  if (!name) return;
  if (badName(name)) { say('Not a folder name: ' + name); return; }
  const r = await fetch('/api/folder?path=' + q(join(cwd, name)), {method: 'PUT'});
  say(r.ok ? 'Created ' + show(join(cwd, name)) : 'Create folder failed: ' + await r.text());
  if (r.ok) list(cwd);
}

async function rename(path, old) {
  const name = prompt('Rename ' + old + ' to:', old);
  if (!name || name === old) return;
  if (badName(name)) { say('Not a name: ' + name); return; }
  const r = await fetch('/api/rename?path=' + q(path) + '&name=' + q(name), {method: 'POST'});
  say(r.ok ? 'Renamed ' + old + ' to ' + name : 'Rename failed: ' + await r.text());
  if (r.ok) list(cwd);
}

async function remove(path, dir) {
  if (!confirm('Delete ' + show(path) + '?')) return;
  const r = await fetch('/api/file?path=' + q(path), {method: 'DELETE'});
  say(r.ok ? 'Deleted ' + show(path) : 'Delete failed: ' + await r.text());
  if (r.ok) list(cwd);
}

async function launch(path) {
  say('Launching ' + show(path) + ' ...');
  const r = await fetch('/api/launch?path=' + q(path), {method: 'POST'});
  say(r.ok ? 'Launching ' + show(path) + ': [ Starting XBE ]' : '[ Launch failed: ' + await r.text() + ' ]' );
  }

// The header and the drive (FATX) or folder (exFAT) buttons: the file system and size, then one button for each
// top-level drive or folder that exists.
async function start() {
  const r = await fetch('/api/disk');
  if (r.ok) {
    const disk = await r.json();
    exfat = disk.fs === 'exFAT';
    $('title').textContent = 'OXOS File Manager [' + disk.fs + ', ' + diskSize(disk.bytes) + ']';
    $('drivesLabel').textContent = exfat ? 'Folders:' : 'Drives:';
  }
  const top = await fetch('/api/list?path=' + q('/'));
  for (const e of top.ok ? await top.json() : []) {
    const b = document.createElement('button');
    b.textContent = exfat ? '/' + e.name : e.name + ':';
    b.onclick = () => list('/' + e.name);
    $('drives').append(b);
  }
  list(cwd);
}
start();
</script>
</body></html>
)PAGE";

const char *UploadPage(ULONG *Length) {
    *Length = sizeof(UploadpPage) - 1;
    return UploadpPage;
}
