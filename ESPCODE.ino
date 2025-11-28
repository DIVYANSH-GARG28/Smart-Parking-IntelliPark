#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Stepper.h>
#include "time.h"

// -------------------- CONFIG --------------------
const char* WIFI_SSID = "Divyansh Garg";
const char* WIFI_PASS = "divyansh";

// Owner credentials (change before deploy)
const char* OWNER_USER = "admin";
const char* OWNER_PASS = "1234";

// NTP (Asia/Kolkata)
const char* NTP_SERVER = "pool.ntp.org";
const long  GMT_OFFSET_SEC = 19800;     // IST +5:30 = 19800 seconds
const int   DAYLIGHT_OFFSET_SEC = 0;

WebServer server(80);

// Pins & hardware
#define IR_ENTRY_PIN 18
#define IR_EXIT_PIN 19
#define GREEN_LED_PIN 5
#define RED_LED_PIN 4

#define IN1 32
#define IN2 33
#define IN3 25
#define IN4 26
#define STEPS_PER_REV 2048

LiquidCrystal_I2C lcd(0x27, 16, 2);
Stepper gateStepper(STEPS_PER_REV, IN1, IN3, IN2, IN4);

// -------------------- PARKING STRUCTS --------------------
const int MAX_SLOTS = 4;
const int MAX_BLOCKS_PER_SLOT = 8;
const int MAX_LOGS = 300;

struct ResBlock {
  unsigned long start; 
  unsigned long end;
  String code;
  String plate;
  bool paid;
};

struct Slot {
  bool occupied;
  String plate;
  unsigned long entry_ts;
  unsigned long cur_end_ts;
  ResBlock blocks[MAX_BLOCKS_PER_SLOT];
  int blockCount;
};

Slot slots[MAX_SLOTS];
String eventLog[MAX_LOGS];
int eventCount = 0;

// OCR and gate state
bool ocrApproved = false;
unsigned long ocrTimestamp = 0;
String lastPlate = "";

// debounce flags
volatile int flagEntry = 0;
volatile int flagExit = 0;

String ownerSessionToken = "";

unsigned long lastCleanup = 0;
unsigned long lastLCD = 0;

// -------------------- UTILITIES --------------------
unsigned long now_ts() {
  time_t raw;
  time(&raw);
  if (raw < 1600000000) {
    // fallback to millis if time not set
    return (unsigned long)(millis() / 1000ULL);
  }
  return (unsigned long)raw;
}

void pushEvent(const String &s) {
  String entry = String(now_ts()) + " : " + s;
  if (eventCount < MAX_LOGS) {
    eventLog[eventCount++] = entry;
  } else {
    // shift left
    for (int i = 1; i < MAX_LOGS; ++i) eventLog[i-1] = eventLog[i];
    eventLog[MAX_LOGS-1] = entry;
  }
  Serial.println("EVENT: " + entry);
}

String ipString() { return WiFi.localIP().toString(); }

String gen_booking_code() {
  int r = random(100000, 999999);
  char buf[8]; sprintf(buf, "%06d", r);
  return String(buf);
}

void resetSlot(Slot &s) {
  s.occupied = false;
  s.plate = "";
  s.entry_ts = 0;
  s.cur_end_ts = 0;
  s.blockCount = 0;
  for (int i = 0; i < MAX_BLOCKS_PER_SLOT; ++i) {
    s.blocks[i].start = 0;
    s.blocks[i].end = 0;
    s.blocks[i].code = "";
    s.blocks[i].plate = "";
    s.blocks[i].paid = false;
  }
}

bool overlap(unsigned long a1, unsigned long a2, unsigned long b1, unsigned long b2) {
  return (a1 < b2) && (b1 < a2);
}

bool slotHasOverlap(int idx, unsigned long start, unsigned long end) {
  if (idx < 0 || idx >= MAX_SLOTS) return true;
  // check blocks
  for (int b = 0; b < slots[idx].blockCount; ++b) {
    if (overlap(start, end, slots[idx].blocks[b].start, slots[idx].blocks[b].end)) return true;
  }
  // occupied check
  if (slots[idx].occupied && overlap(start, end, slots[idx].entry_ts, slots[idx].cur_end_ts)) return true;
  return false;
}

int findSlotForReservation(unsigned long start, unsigned long end) {
  int chosen = -1;
  int minBlocks = 999;
  for (int i = 0; i < MAX_SLOTS; ++i) {
    if (!slotHasOverlap(i, start, end)) {
      if (slots[i].blockCount < minBlocks) {
        minBlocks = slots[i].blockCount;
        chosen = i;
      }
    }
  }
  return chosen;
}

bool addBlockToSlot(int idx, unsigned long start, unsigned long end, const String &plate, const String &code) {
  if (idx < 0 || idx >= MAX_SLOTS) return false;
  if (slots[idx].blockCount >= MAX_BLOCKS_PER_SLOT) return false;
  int b = slots[idx].blockCount++;
  slots[idx].blocks[b].start = start;
  slots[idx].blocks[b].end = end;
  slots[idx].blocks[b].code = code;
  slots[idx].blocks[b].plate = plate;
  slots[idx].blocks[b].paid = false;
  return true;
}

int findReservationByCode(const String &code) {
  for (int i = 0; i < MAX_SLOTS; ++i) {
    for (int b = 0; b < slots[i].blockCount; ++b) {
      if (slots[i].blocks[b].code == code) return i;
    }
  }
  return -1;
}

int findBlockIndexByCode(int slotIdx, const String &code) {
  if (slotIdx < 0 || slotIdx >= MAX_SLOTS) return -1;
  for (int b = 0; b < slots[slotIdx].blockCount; ++b) {
    if (slots[slotIdx].blocks[b].code == code) return b;
  }
  return -1;
}

int findReservationByPlate(const String &plate) {
  for (int i = 0; i < MAX_SLOTS; ++i) {
    for (int b = 0; b < slots[i].blockCount; ++b) {
      if (slots[i].blocks[b].plate.equalsIgnoreCase(plate)) return i;
    }
  }
  return -1;
}

int findOccupiedByPlate(const String &plate) {
  for (int i = 0; i < MAX_SLOTS; ++i) {
    if (slots[i].occupied && slots[i].plate.equalsIgnoreCase(plate)) return i;
  }
  return -1;
}

// cleanup expired blocks and no-show auto-cancel (30% rule)
void cleanupExpiredAndNoShow() {
  unsigned long now = now_ts();
  for (int i = 0; i < MAX_SLOTS; ++i) {
    int writePos = 0;
    for (int b = 0; b < slots[i].blockCount; ++b) {
      ResBlock &rb = slots[i].blocks[b];
      // expired -> remove
      if (rb.end <= now) {
        pushEvent("Booking expired slot " + String(i+1) + " plate " + rb.plate + " code " + rb.code);
        continue;
      }
      // no-show: if now > start + 30% duration AND not occupied by plate -> cancel (if now >= start)
      unsigned long duration = (rb.end > rb.start) ? (rb.end - rb.start) : 0;
      unsigned long grace = (unsigned long)((double)duration * 0.30);
      if (duration > 0 && now > (rb.start + grace) && now >= rb.start) {
        bool occupiedBySame = (slots[i].occupied && slots[i].plate.equalsIgnoreCase(rb.plate));
        if (!occupiedBySame) {
          pushEvent("No-show cancelled slot " + String(i+1) + " plate " + rb.plate + " code " + rb.code);
          continue;
        }
      }
      // keep block
      if (writePos != b) slots[i].blocks[writePos] = slots[i].blocks[b];
      writePos++;
    }
    slots[i].blockCount = writePos;
  }
}

// -------------------- LCD & GATE --------------------
void updateLCD() {
  lcd.clear();
  lcd.setCursor(0,0);
  lcd.print("IP:");
  String ip = ipString();
  if (ip.length() > 10) ip = ip.substring(ip.length()-10);
  lcd.print(ip);
  lcd.setCursor(0,1);
  int avail = 0;
  unsigned long now = now_ts();
  for (int i = 0; i < MAX_SLOTS; ++i) {
    bool busyNow = slots[i].occupied && slots[i].cur_end_ts > now;
    for (int b = 0; b < slots[i].blockCount; ++b) {
      if (slots[i].blocks[b].start <= now && now < slots[i].blocks[b].end) { busyNow = true; break; }
    }
    if (!busyNow) avail++;
  }
  lcd.print("Free:");
  lcd.print(avail);
  lcd.print("/");
  lcd.print(MAX_SLOTS);
}

void openGateAction() {
  digitalWrite(GREEN_LED_PIN, HIGH);
  digitalWrite(RED_LED_PIN, LOW);
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Gate Opening");
  if (lastPlate.length()) { lcd.setCursor(0,1); lcd.print(lastPlate.substring(0, min((size_t)12, lastPlate.length()))); }
  gateStepper.setSpeed(10);
  // open
  for (int i = 0; i < 512; ++i) gateStepper.step(1);
  delay(600);
  // close
  for (int i = 0; i < 512; ++i) gateStepper.step(-1);
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, HIGH);
  updateLCD();
}

// -------------------- FILE UPLOAD (preview) --------------------
File uploadFile;
void handlePreviewUpload() {
  // This only called after upload completed handler; we send a response here.
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "text/plain", "PREVIEW_UPLOADED");
}

void handlePreviewUploadStart() {
  // provide placeholder - actual handling will be in upload handler
  server.send(200, "text/plain", "OK");
}

// called by server during upload process
void handleFileUpload() {
  HTTPUpload& upload = server.upload();
  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.startsWith("/")) filename = "/" + filename;
    // Write to LittleFS
    if (LittleFS.exists(filename)) LittleFS.remove(filename);
    uploadFile = LittleFS.open(filename, "w");
    if (!uploadFile) {
      Serial.println("Failed to open file for writing: " + filename);
    } else {
      Serial.println("Start upload: " + filename);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      Serial.println("Upload finished");
      pushEvent("Preview image uploaded");
    }
  }
}

// Serve preview.png (if exists in LittleFS) or a generated SVG placeholder
void handlePreview() {
  const char *path = "/preview.png";
  if (LittleFS.exists(path)) {
    File f = LittleFS.open(path, "r");
    server.streamFile(f, "image/png");
    f.close();
    return;
  }
  // return an SVG placeholder that displays lastPlate if available
  String svg = "<svg xmlns='http://www.w3.org/2000/svg' width='640' height='360'>";
  svg += "<rect width='100%' height='100%' fill='#1b2630'/>";
  svg += "<text x='50%' y='45%' fill='#9fb6c6' font-family='Arial' font-size='28' text-anchor='middle'>No preview available</text>";
  if (lastPlate.length() > 0) {
    svg += "<text x='50%' y='65%' fill='#06b6d4' font-family='Arial' font-size='22' text-anchor='middle'>Plate: " + lastPlate + "</text>";
  }
  svg += "</svg>";
  server.send(200, "image/svg+xml", svg);
}

// -------------------- HTML PAGES (Owner / User) --------------------
// For readability we store large HTML in raw strings.
// Owner UI: enterprise look, validated login (server-side), upload preview form, events list, slot grid.
String ownerHTML() {
  String s = R"rawliteral(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Owner - Smart Parking (Enterprise)</title>
<link rel="icon" href="data:,">
<style>
:root{--bg:#0b1220;--card:#071022;--muted:#9fb6c6;--accent:#06b6d4;--ok:#10b981;--warn:#f59e0b;--bad:#ef4444;--glass:rgba(255,255,255,0.03)}
*{box-sizing:border-box}
body{font-family:Inter,system-ui,Arial,sans-serif;background:linear-gradient(180deg,#061323,#041422);color:#e6eef6;margin:0;padding:20px;}
.container{max-width:1200px;margin:0 auto;}
.header{display:flex;justify-content:space-between;align-items:center;gap:12px}
.title{font-size:20px;font-weight:800}
.right{display:flex;gap:10px;align-items:center}
.card{background:linear-gradient(180deg,#071022,#081425);padding:14px;border-radius:10px;box-shadow:0 10px 30px rgba(0,0,0,0.6)}
.controls{display:flex;gap:8px;flex-wrap:wrap}
.input,button,select{padding:10px;border-radius:8px;border:none;background:#0f1f2a;color:#e6eef6}
.btn{background:var(--accent);color:#022;cursor:pointer;font-weight:700}
.statrow{display:flex;gap:10px;margin-top:12px}
.stat{flex:1;padding:12px;border-radius:8px;background:#0a1a24;text-align:center}
.stat h2{margin:0;font-size:20px}
.grid{display:grid;grid-template-columns:2fr 420px;gap:12px;margin-top:12px}
@media(max-width:980px){ .grid{grid-template-columns:1fr} .header{flex-direction:column;align-items:flex-start} }
.slot-grid{display:flex;gap:8px;flex-wrap:wrap}
.slot{padding:10px;border-radius:8px;min-width:88px;text-align:center;background:#072033;border-left:6px solid transparent}
.slot.free{border-left-color:var(--ok)}
.slot.reserved{border-left-color:var(--warn)}
.slot.occupied{border-left-color:var(--bad)}
.table{margin-top:12px}
.table table{width:100%;border-collapse:collapse;color:#cbe7ee}
.table th,.table td{padding:8px;border-bottom:1px solid rgba(255,255,255,0.03);font-size:13px}
.log{max-height:300px;overflow:auto;background:#071622;padding:8px;border-radius:8px;font-size:12px;color:var(--muted)}
.small{font-size:13px;color:var(--muted)}
.login-modal{position:fixed;inset:0;display:flex;align-items:center;justify-content:center;background:rgba(0,0,0,0.6)}
.login-card{width:420px;padding:16px;border-radius:10px;background:#071022}
.preview{width:100%;border-radius:8px;display:block}
.upload-row{display:flex;gap:8px;align-items:center}
.note{font-size:12px;color:var(--muted)}
</style>
</head>
<body>
<div class="container">
  <div class="header">
    <div>
      <div class="title">Smart Parking — Owner Dashboard</div>
      <div class="small">Enterprise UI — Manage slots, upload camera preview, watch events</div>
    </div>
    <div class="right">
      <div id="ip" class="small">IP: --</div>
      <button class="btn" id="logoutBtn">Logout</button>
    </div>
  </div>

  <div class="card statrow">
    <div class="stat"><h2 id="total">0</h2><div class="small">Total Slots</div></div>
    <div class="stat"><h2 id="available">0</h2><div class="small">Available Now</div></div>
    <div class="stat"><h2 id="reserved">0</h2><div class="small">Active Reservations</div></div>
    <div class="stat"><h2 id="occupied">0</h2><div class="small">Occupied</div></div>
  </div>

  <div class="grid">
    <div>
      <div class="card">
        <div style="display:flex;justify-content:space-between;align-items:center">
          <div style="font-weight:700">Slots & Live Bookings</div>
          <div class="small" id="nowTime"></div>
        </div>

        <div class="slot-grid" id="slotIcons" style="margin-top:10px"></div>

        <div class="table card" style="margin-top:12px;padding:8px;">
          <table><thead><tr><th>Slot</th><th>Plate</th><th>Entry</th><th>End</th><th>Blocks</th><th>Occ</th></tr></thead>
          <tbody id="slotsTable"></tbody></table>
        </div>
      </div>

      <div class="card" style="margin-top:12px">
        <div style="display:flex;justify-content:space-between;align-items:center">
          <div style="font-weight:700">Create Reservation</div>
          <div class="small">Server time used for validation</div>
        </div>
        <div style="margin-top:10px" class="controls">
          <input id="plate" class="input" placeholder="Plate e.g. MH12AB1234">
          <input id="start_dt" class="input" type="datetime-local">
          <input id="end_dt" class="input" type="datetime-local">
          <button class="btn" onclick="createBooking()">Reserve</button>
        </div>
        <div class="note" style="margin-top:8px">Tip: Use user page for duration-based booking. Reservations auto-cancel on no-show after 30% grace period.</div>
      </div>
    </div>

    <div>
      <div class="card">
        <div style="font-weight:700">Camera / Preview</div>
        <img id="previewImg" class="preview" src="/preview.png" alt="preview">
        <div style="margin-top:8px" class="upload-row">
          <input id="filePicker" type="file" accept="image/*">
          <button class="btn" onclick="uploadPreview()">Upload</button>
        </div>
        <div class="note" style="margin-top:6px">Upload preview.png (max size depends on LittleFS). Preview will update automatically.</div>
      </div>

      <div class="card" style="margin-top:12px">
        <div style="font-weight:700">Event Log</div>
        <div id="log" class="log"></div>
      </div>
    </div>
  </div>
</div>

<!-- Login Modal -->
<div id="loginModal" class="login-modal" style="display:none">
  <div class="login-card">
    <div style="font-weight:700;margin-bottom:8px">Owner Login</div>
    <input id="inUser" class="input" placeholder="Username" value="admin">
    <input id="inPass" class="input" placeholder="Password" type="password" value="1234">
    <div style="display:flex;gap:8px;margin-top:8px">
      <button class="btn" onclick="doLogin()">Login</button>
      <button class="input" onclick="closeLogin()">Cancel</button>
    </div>
    <div id="loginErr" style="color:#f88;margin-top:8px;display:none">Invalid credentials</div>
  </div>
</div>

<script>
const OWNER_USER = "admin"; // JS-level hint only
let OWNER_TOKEN = null;

function secToDate(ts){ if(!ts||ts==0) return '--'; return new Date(ts*1000).toLocaleString(); }

async function loadData(){
  try {
    const res = await fetch('/data');
    const j = await res.json();
    document.getElementById('ip').innerText = "IP: " + window.location.hostname;
    document.getElementById('total').innerText = j.total;
    document.getElementById('available').innerText = j.available;
    document.getElementById('reserved').innerText = j.reserved;
    document.getElementById('occupied').innerText = j.occupied;
    document.getElementById('nowTime').innerText = j.now ? new Date(j.now*1000).toLocaleString() : '';

    // slot icons
    let icons = '';
    j.slots.forEach(s => {
      let cls='slot';
      if (s.occupied) cls += ' occupied';
      else if (s.blocks && s.blocks.length>0) cls += ' reserved';
      else cls += ' free';
      icons += `<div class="${cls}">S${s.index}<br><small>${s.plate || '--'}</small></div>`;
    });
    document.getElementById('slotIcons').innerHTML = icons;

    // table
    let rows = '';
    j.slots.forEach(s => {
      let blocksHtml = '--';
      if (s.blocks && s.blocks.length>0) {
        blocksHtml = '';
        s.blocks.forEach(b => {
          blocksHtml += `<div style="font-size:12px">${new Date(b.start*1000).toLocaleString()} → ${new Date(b.end*1000).toLocaleString()}<br><b>${b.plate}</b> (${b.code})</div>`;
        });
      }
      rows += `<tr>
        <td>${s.index}</td>
        <td>${s.plate || '--'}</td>
        <td>${s.entry? new Date(s.entry*1000).toLocaleString() : '--'}</td>
        <td>${s.cur_end? new Date(s.cur_end*1000).toLocaleString() : '--'}</td>
        <td>${blocksHtml}</td>
        <td>${s.occupied}</td>
      </tr>`;
    });
    document.getElementById('slotsTable').innerHTML = rows;

    // events
    let logHtml = '';
    (j.events || []).slice().reverse().forEach(e => logHtml += `<div>${e}</div>`);
    document.getElementById('log').innerHTML = logHtml;
  } catch (err) {
    console.error(err);
  }
}

async function createBooking(){
  const plate = document.getElementById('plate').value.trim().toUpperCase();
  const start = document.getElementById('start_dt').value;
  const end = document.getElementById('end_dt').value;
  if(!plate || !start || !end){ alert('Fill plate, start and end datetimes'); return; }
  const s = Math.floor(new Date(start).getTime()/1000);
  const e = Math.floor(new Date(end).getTime()/1000);
  if (e <= s){ alert('End must be after start'); return; }
  const form = new URLSearchParams();
  form.append('plate', plate);
  form.append('start', String(s));
  form.append('end', String(e));
  const r = await fetch('/book', { method:'POST', body: form });
  const txt = await r.text();
  alert(txt);
  await loadData();
}

function showLogin(){
  document.getElementById('loginModal').style.display = 'flex';
}
function closeLogin(){
  document.getElementById('loginModal').style.display = 'none';
}
async function doLogin(){
  const u = document.getElementById('inUser').value.trim();
  const p = document.getElementById('inPass').value.trim();
  const form = new URLSearchParams();
  form.append('user', u); form.append('pass', p);
  const r = await fetch('/owner_login',{method:'POST', body: form});
  const txt = await r.text();
  if(r.status === 200) {
    OWNER_TOKEN = txt; // token returned
    localStorage.setItem('ownerToken', OWNER_TOKEN);
    closeLogin();
    loadData();
  } else {
    document.getElementById('loginErr').style.display = 'block';
  }
}

document.getElementById('logoutBtn').addEventListener('click', async ()=>{
  localStorage.removeItem('ownerToken');
  OWNER_TOKEN = null;
  showLogin();
});

// preview upload
async function uploadPreview(){
  const fileInput = document.getElementById('filePicker');
  if (!fileInput.files || fileInput.files.length==0){ alert('Choose an image'); return; }
  const f = fileInput.files[0];
  const form = new FormData();
  form.append('file', f, 'preview.png');
  const r = await fetch('/upload_preview', { method:'POST', body: form });
  const txt = await r.text();
  if(r.status === 200) {
    alert('Preview uploaded');
    // reload preview
    document.getElementById('previewImg').src = '/preview.png?rnd=' + Date.now();
  } else {
    alert('Upload failed: ' + txt);
  }
}

window.addEventListener('load', ()=>{
  if (!localStorage.getItem('ownerToken')) {
    showLogin();
  } else {
    OWNER_TOKEN = localStorage.getItem('ownerToken');
  }
  loadData();
  setInterval(loadData, 2500);
  // refresh preview occasionally
  setInterval(()=> {
    const img = document.getElementById('previewImg');
    if (img) img.src = '/preview.png?rnd=' + Date.now();
  }, 5000);
});
</script>
</body>
</html>
)rawliteral";
  return s;
}

// User HTML - clean booking flow, duration-based, arrival-by calculation
String userHTML() {
  String s = R"rawliteral(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>User - Smart Parking</title>
<link rel="icon" href="data:,">
<style>
body{font-family:Inter,system-ui,Arial,sans-serif;background:#061223;color:#e6eef6;margin:0;padding:22px}
.container{max-width:560px;margin:0 auto}
.card{background:linear-gradient(180deg,#071026,#071829);padding:16px;border-radius:10px}
.h{font-weight:700;font-size:20px;margin-bottom:8px}
.input,button{width:100%;padding:12px;border-radius:8px;border:none;background:#0f1f2a;color:#e6eef6;margin-top:8px}
.btn{background:#06b6d4;color:#022;font-weight:700;cursor:pointer}
.small{font-size:13px;color:#9fb6c6}
.result{margin-top:12px;padding:12px;border-radius:8px;background:#071a26}
</style>
</head>
<body>
<div class="container">
  <div class="card">
    <div class="h">Book Your Parking Slot</div>
    <label class="small">Vehicle Plate</label>
    <input id="plate" class="input" placeholder="MH12AB1234">

    <label class="small">Start Date & Time</label>
    <input id="start_dt" class="input" type="datetime-local">

    <label class="small">Duration (hours)</label>
    <input id="duration" class="input" type="number" min="1" value="2">

    <button class="btn" onclick="processBooking()">Calculate & Book</button>

    <div id="result" class="result" style="display:none"></div>

    <div class="small" style="margin-top:10px">You will receive a booking code. Arrive before the "arrive by" time (start + 30% of duration) to keep the slot.</div>
  </div>
</div>

<script>
function toEpoch(dt){ return Math.floor(new Date(dt).getTime()/1000) }
async function processBooking(){
  const plate = document.getElementById('plate').value.trim().toUpperCase();
  const startVal = document.getElementById('start_dt').value;
  const hours = parseFloat(document.getElementById('duration').value);
  if(!plate || !startVal || !hours || hours<=0){ alert('Fill fields'); return; }
  const startEpoch = toEpoch(startVal);
  const endEpoch = startEpoch + Math.round(hours * 3600);
  const arrivalBefore = startEpoch + Math.round(hours * 0.30 * 3600);
  const resDiv = document.getElementById('result');
  resDiv.style.display = 'block';
  resDiv.innerHTML = `<b>Plate:</b> ${plate}<br><b>Start:</b> ${new Date(startEpoch*1000).toLocaleString()}<br><b>End:</b> ${new Date(endEpoch*1000).toLocaleString()}<br><b>Arrive by:</b> ${new Date(arrivalBefore*1000).toLocaleString()}<br>Booking...`;
  const form = new URLSearchParams();
  form.append('plate', plate); form.append('start', String(startEpoch)); form.append('end', String(endEpoch));
  const r = await fetch('/user_book', { method:'POST', body: form });
  const txt = await r.text();
  resDiv.innerHTML += `<br><b>Server:</b> ${txt}`;
}
</script>
</body>
</html>
)rawliteral";
  return s;
}

// -------------------- HTTP HANDLERS --------------------

// /data -> JSON used by owner dashboard
void handleData() {
  cleanupExpiredAndNoShow();
  unsigned long now = now_ts();

  int occupied = 0, reserved = 0;
  for (int i = 0; i < MAX_SLOTS; ++i) {
    if (slots[i].occupied && slots[i].cur_end_ts > now) occupied++;
    for (int b = 0; b < slots[i].blockCount; ++b) {
      if (slots[i].blocks[b].end > now) reserved++;
    }
  }
  int available = MAX_SLOTS - occupied;

  // build JSON carefully and compactly
  String json = "{";
  json += "\"total\":" + String(MAX_SLOTS) + ",";
  json += "\"available\":" + String(available) + ",";
  json += "\"reserved\":" + String(reserved) + ",";
  json += "\"occupied\":" + String(occupied) + ",";
  json += "\"now\":" + String(now) + ",";
  json += "\"slots\":[";

  for (int i = 0; i < MAX_SLOTS; ++i) {
    if (i) json += ",";
    String plateEsc = slots[i].plate; plateEsc.replace("\"","'");
    json += "{";
    json += "\"index\":" + String(i+1) + ",";
    json += String("\"occupied\":") + (slots[i].occupied ? "true" : "false") + ",";
    json += "\"plate\":\"" + plateEsc + "\",";
    json += "\"entry\":" + String(slots[i].entry_ts) + ",";
    json += "\"cur_end\":" + String(slots[i].cur_end_ts) + ",";
    json += "\"blocks\":[";
    for (int b = 0; b < slots[i].blockCount; ++b) {
      if (b) json += ",";
      ResBlock &rb = slots[i].blocks[b];
      String plateB = rb.plate; plateB.replace("\"","'");
      json += "{";
      json += "\"start\":" + String(rb.start) + ",";
      json += "\"end\":" + String(rb.end) + ",";
      json += "\"code\":\"" + rb.code + "\",";
      json += "\"plate\":\"" + plateB + "\",";
      json += String("\"paid\":") + (rb.paid ? "true" : "false");
      json += "}";
    }
    json += "]";
    json += "}";
  }

  json += "],\"events\":[";
  for (int i = 0; i < eventCount; ++i) {
    if (i) json += ",";
    String e = eventLog[i]; e.replace("\"","'");
    json += "\"" + e + "\"";
  }
  json += "]}";

  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

// /owner - owner UI page
void handleOwnerPage() {
  server.send(200, "text/html", ownerHTML());
}

// /user - user UI page
void handleUserPage() {
  server.send(200, "text/html", userHTML());
}

// /ocr?plate=XYZ -> accept OCR plate (approve)
void handleOCR() {
  if (!server.hasArg("plate")) { server.send(400, "text/plain", "Missing plate"); return; }
  String plate = server.arg("plate");
  plate.trim(); plate.toUpperCase();
  if (plate.length() < 3) { ocrApproved = false; server.send(400, "text/plain", "INVALID_PLATE"); return; }
  lastPlate = plate;
  ocrApproved = true;
  ocrTimestamp = millis();
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Plate OK:"); lcd.setCursor(0,1); lcd.print(plate.substring(0, min((size_t)12, plate.length())));
  pushEvent("OCR approved: " + plate);
  server.send(200, "text/plain", "OCR_OK");
}

// /gate?status=OK&plate=...&duration=hours -> walk-in or manual gate open
void handleGate() {
  if (!server.hasArg("status")) { server.send(400, "text/plain", "Missing status"); return; }
  String status = server.arg("status"); status.toUpperCase();
  if (status != "OK") { server.send(200, "text/plain", "INVALID"); return; }
  if (server.hasArg("plate")) { lastPlate = server.arg("plate"); lastPlate.trim(); lastPlate.toUpperCase(); }
  ocrApproved = true; ocrTimestamp = millis();
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Gate OK:"); lcd.setCursor(0,1); lcd.print(lastPlate.substring(0, min((size_t)12,lastPlate.length())));
  pushEvent("Gate OK plate=" + lastPlate);

  if (server.hasArg("duration")) {
    float hrs = atof(server.arg("duration").c_str());
    if (hrs <= 0) { server.send(400, "text/plain", "Bad duration"); return; }
    unsigned long now = now_ts();
    unsigned long reqEnd = now + (unsigned long)(hrs * 3600.0);
    int chosen = -1;
    for (int i = 0; i < MAX_SLOTS; ++i) {
      if (slots[i].occupied && slots[i].cur_end_ts > now) continue;
      if (slotHasOverlap(i, now, reqEnd)) continue;
      chosen = i; break;
    }
    if (chosen == -1) { server.send(200, "text/plain", "NO_SLOT_FOR_DURATION"); return; }
    slots[chosen].occupied = true;
    slots[chosen].plate = lastPlate;
    slots[chosen].entry_ts = now;
    slots[chosen].cur_end_ts = reqEnd;
    pushEvent("Walk-in allocated slot " + String(chosen+1) + " plate " + lastPlate);
    openGateAction();
    server.send(200, "text/plain", "OCCUPIED_SLOT_" + String(chosen+1));
    return;
  }

  server.send(200, "text/plain", "OK");
}

// /book (owner) POST: plate,start,end
void handleBook() {
  if (!server.hasArg("plate") || !server.hasArg("start") || !server.hasArg("end")) { server.send(400, "text/plain", "Missing plate/start/end"); return; }
  String plate = server.arg("plate"); plate.trim(); plate.toUpperCase();
  unsigned long start_ts = (unsigned long) strtoul(server.arg("start").c_str(), NULL, 10);
  unsigned long end_ts = (unsigned long) strtoul(server.arg("end").c_str(), NULL, 10);
  unsigned long now = now_ts();
  if (start_ts < now) { server.send(400, "text/plain", "Start must be >= now"); return; }
  if (end_ts <= start_ts) { server.send(400, "text/plain", "End must be after start"); return; }

  int sidx = findSlotForReservation(start_ts, end_ts);
  if (sidx == -1) { server.send(400, "text/plain", "No slot available for requested window"); return; }
  String code = gen_booking_code();
  if (!addBlockToSlot(sidx, start_ts, end_ts, plate, code)) { server.send(500, "text/plain", "Failed to add booking"); return; }

  double hours = (double)(end_ts - start_ts) / 3600.0;
  double amount = 0.0;
  if (hours <= 3.0) amount = 30.0;
  else amount = 30.0 + 10.0 * ceil(hours - 3.0);

  char buf[128];
  sprintf(buf, "RESERVED,%d,%s,%.2f", sidx+1, code.c_str(), amount);
  pushEvent("Reserved slot " + String(sidx+1) + " plate " + plate + " code " + code);
  server.send(200, "text/plain", String(buf));
}

// /user_book POST (public booking)
void handleUserBook() {
  if (!server.hasArg("plate") || !server.hasArg("start") || !server.hasArg("end")) { server.send(400, "text/plain", "Missing plate/start/end"); return; }
  String plate = server.arg("plate"); plate.trim(); plate.toUpperCase();
  unsigned long start_ts = (unsigned long) strtoul(server.arg("start").c_str(), NULL, 10);
  unsigned long end_ts = (unsigned long) strtoul(server.arg("end").c_str(), NULL, 10);
  unsigned long now = now_ts();
  if (start_ts < now) { server.send(400, "text/plain", "Start must be >= now"); return; }
  if (end_ts <= start_ts) { server.send(400, "text/plain", "End must be after start"); return; }

  int sidx = findSlotForReservation(start_ts, end_ts);
  if (sidx == -1) { server.send(400, "text/plain", "No slot available for requested window"); return; }
  String code = gen_booking_code();
  if (!addBlockToSlot(sidx, start_ts, end_ts, plate, code)) { server.send(500, "text/plain", "Failed to add booking"); return; }

  double hours = (double)(end_ts - start_ts) / 3600.0;
  double amount = 0.0;
  if (hours <= 3.0) amount = 30.0;
  else amount = 30.0 + 10.0 * ceil(hours - 3.0);

  char buf[128];
  sprintf(buf, "RESERVED,%d,%s,%.2f", sidx+1, code.c_str(), amount);
  pushEvent("User reserved slot " + String(sidx+1) + " plate " + plate + " code " + code);
  server.send(200, "text/plain", String(buf));
}

// /cancel?code=... or /cancel?plate=...
void handleCancel() {
  if (server.hasArg("code")) {
    String code = server.arg("code");
    int sidx = findReservationByCode(code);
    if (sidx == -1) { server.send(400, "text/plain", "Booking not found"); return; }
    int bi = findBlockIndexByCode(sidx, code);
    if (bi == -1) { server.send(400, "text/plain", "Booking not found"); return; }
    pushEvent("Cancelled booking code " + code + " slot " + String(sidx+1));
    for (int j = bi + 1; j < slots[sidx].blockCount; ++j) slots[sidx].blocks[j-1] = slots[sidx].blocks[j];
    slots[sidx].blockCount--;
    server.send(200, "text/plain", "CANCELLED");
    return;
  } else if (server.hasArg("plate")) {
    String plate = server.arg("plate");
    for (int i = 0; i < MAX_SLOTS; ++i) {
      for (int b = 0; b < slots[i].blockCount; ++b) {
        if (slots[i].blocks[b].plate.equalsIgnoreCase(plate)) {
          if (slots[i].occupied && slots[i].plate.equalsIgnoreCase(plate)) {
            server.send(400, "text/plain", "Currently occupied - cannot cancel");
            return;
          }
          pushEvent("Cancelled booking plate " + plate);
          for (int j = b + 1; j < slots[i].blockCount; ++j) slots[i].blocks[j-1] = slots[i].blocks[j];
          slots[i].blockCount--;
          server.send(200, "text/plain", "CANCELLED");
          return;
        }
      }
    }
    server.send(400, "text/plain", "Not found");
    return;
  }
  server.send(400, "text/plain", "Missing code or plate");
}

// /pay?code= -> mark paid
void handlePay() {
  if (!server.hasArg("code")) { server.send(400, "text/plain", "Missing code"); return; }
  String code = server.arg("code");
  int sidx = findReservationByCode(code);
  if (sidx == -1) { server.send(400, "text/plain", "Booking not found"); return; }
  int bi = findBlockIndexByCode(sidx, code);
  if (bi == -1) { server.send(400, "text/plain", "Booking not found"); return; }
  slots[sidx].blocks[bi].paid = true;
  pushEvent("Paid booking code " + code);
  server.send(200, "text/plain", "PAID");
}

// /preview.png handled by handlePreview() above

// /upload_preview accepts file upload via field name "file"
void handleUploadPreviewEndpoint() {
  // response handled after file upload via handleFileUpload and handlePreviewUpload
  handlePreviewUpload();
}

// /owner_login POST -> verify credentials (server-side) and return token
void handleOwnerLogin() {
  if (!server.hasArg("user") || !server.hasArg("pass")) { server.send(400, "text/plain", "Missing"); return; }
  String u = server.arg("user");
  String p = server.arg("pass");
  if (u == OWNER_USER && p == OWNER_PASS) {
    // generate a simple token
    ownerSessionToken = gen_booking_code() + gen_booking_code();
    pushEvent("Owner logged in");
    server.send(200, "text/plain", ownerSessionToken);
  } else {
    server.send(401, "text/plain", "INVALID");
  }
}

// root
void handleRoot() {
  String r = "<html><body><h3>Smart Parking</h3><a href='/owner'>Owner</a><br><a href='/user'>User</a></body></html>";
  server.send(200, "text/html", r);
}

// -------------------- SETUP & LOOP --------------------
void init_time() {
  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);
}

void setup() {
  Serial.begin(115200);
  randomSeed(analogRead(0));

  // prepare LittleFS
  if (!LittleFS.begin(true)) {
    Serial.println("LittleFS mount failed");
  } else {
    Serial.println("LittleFS mounted");
  }

  // reset slots
  for (int i = 0; i < MAX_SLOTS; ++i) resetSlot(slots[i]);
  eventCount = 0;

  // pins
  pinMode(IR_ENTRY_PIN, INPUT_PULLUP);
  pinMode(IR_EXIT_PIN, INPUT_PULLUP);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, HIGH);

  gateStepper.setSpeed(10);

  lcd.init();
  lcd.backlight();
  lcd.clear(); lcd.setCursor(0,0); lcd.print("Connecting WiFi...");

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("Connecting WiFi");
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries < 120) { delay(250); Serial.print("."); tries++; }
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("\nWiFi failed");
    lcd.clear(); lcd.setCursor(0,0); lcd.print("WiFi Failed");
    pushEvent("WiFi Failed");
  } else {
    Serial.println("\nWiFi connected");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    lcd.clear(); lcd.setCursor(0,0); lcd.print("IP:");
    lcd.setCursor(4,0); lcd.print(WiFi.localIP().toString().substring(0,10));
    pushEvent("WiFi connected: " + WiFi.localIP().toString());
    delay(600);
  }

  init_time();

  // HTTP routes
  server.on("/", handleRoot);
  server.on("/owner", handleOwnerPage);
  server.on("/user", handleUserPage);
  server.on("/data", handleData);
  server.on("/book", HTTP_POST, handleBook);
  server.on("/user_book", HTTP_POST, handleUserBook);
  server.on("/cancel", handleCancel);
  server.on("/pay", handlePay);
  server.on("/ocr", handleOCR);
  server.on("/gate", handleGate);
  server.on("/preview.png", HTTP_GET, handlePreview);

  // upload_preview - file upload via multipart form (field name: file)
  server.on("/upload_preview", HTTP_POST, handleUploadPreviewEndpoint, handleFileUpload);

  // owner login
  server.on("/owner_login", HTTP_POST, handleOwnerLogin);

  // start webserver
  server.begin();
  Serial.println("HTTP server started");
  updateLCD();
  pushEvent("Server started");
}

void loop() {
  server.handleClient();

  unsigned long now = millis();

  // periodic cleanup & lcd
  if (now - lastCleanup > 3000) {
    cleanupExpiredAndNoShow();
    lastCleanup = now;
  }
  if (now - lastLCD > 2000) {
    updateLCD();
    lastLCD = now;
  }

  // OCR timeout
  if (ocrApproved && (millis() - ocrTimestamp > 7000)) ocrApproved = false;

  // ENTRY logic (debounce)
  static uint32_t lastEntryChange = 0;
  int entryState = digitalRead(IR_ENTRY_PIN);
  if (entryState == LOW && flagEntry == 0 && (millis() - lastEntryChange) > 200) {
    flagEntry = 1; lastEntryChange = millis();
    Serial.println("IR_ENTRY triggered");
    if (!ocrApproved) {
      lcd.clear(); lcd.setCursor(0,0); lcd.print("ENTRY BLOCKED"); lcd.setCursor(0,1); lcd.print("No Plate");
      pushEvent("Entry blocked - no OCR");
      delay(900); updateLCD();
    } else {
      String plate = lastPlate;
      int occ = findOccupiedByPlate(plate);
      if (occ != -1) {
        lcd.clear(); lcd.setCursor(0,0); lcd.print("Already Parked"); lcd.setCursor(0,1); lcd.print(plate.substring(0,min((size_t)12,plate.length())));
        pushEvent("Already parked: " + plate);
        delay(900); updateLCD();
      } else {
        // try to match reservation for current time
        unsigned long nowSec = now_ts();
        bool found = false;
        for (int i = 0; i < MAX_SLOTS && !found; ++i) {
          for (int b = 0; b < slots[i].blockCount; ++b) {
            ResBlock &rb = slots[i].blocks[b];
            if (rb.plate.equalsIgnoreCase(plate) && nowSec >= rb.start && nowSec <= rb.end) {
              slots[i].occupied = true;
              slots[i].plate = plate;
              slots[i].entry_ts = nowSec;
              slots[i].cur_end_ts = rb.end;
              // remove block
              for (int j = b + 1; j < slots[i].blockCount; ++j) slots[i].blocks[j-1] = slots[i].blocks[j];
              slots[i].blockCount--;
              pushEvent("Reserved -> occupied slot " + String(i+1) + " plate " + plate);
              lastPlate = plate;
              openGateAction();
              found = true;
              break;
            }
          }
        }
        if (!found) {
          // walk-in default 1 hour
          unsigned long reqEnd = now_ts() + 3600;
          int chosen = -1;
          for (int i = 0; i < MAX_SLOTS; ++i) {
            if (slots[i].occupied && slots[i].cur_end_ts > now_ts()) continue;
            if (slotHasOverlap(i, now_ts(), reqEnd)) continue;
            chosen = i; break;
          }
          if (chosen == -1) {
            lcd.clear(); lcd.setCursor(0,0); lcd.print("PARK FULL"); pushEvent("Park full for walk-in " + plate);
            delay(900); updateLCD();
          } else {
            slots[chosen].occupied = true;
            slots[chosen].plate = plate;
            slots[chosen].entry_ts = now_ts();
            slots[chosen].cur_end_ts = reqEnd;
            pushEvent("Walk-in -> slot " + String(chosen+1) + " plate " + plate);
            lastPlate = plate;
            openGateAction();
          }
        }
      }
    }
    ocrApproved = false;
  }
  if (entryState == HIGH) flagEntry = 0;

  // EXIT logic (debounce)
  static uint32_t lastExitChange = 0;
  int exitState = digitalRead(IR_EXIT_PIN);
  if (exitState == LOW && flagExit == 0 && (millis() - lastExitChange) > 200) {
    flagExit = 1; lastExitChange = millis();
    Serial.println("IR_EXIT triggered");
    bool freed = false;
    if (lastPlate.length()) {
      int idx = findOccupiedByPlate(lastPlate);
      if (idx != -1) {
        pushEvent("Exit - freed slot " + String(idx+1) + " plate " + slots[idx].plate);
        resetSlot(slots[idx]);
        freed = true;
      }
    }
    if (!freed) {
      for (int i = 0; i < MAX_SLOTS; ++i) {
        if (slots[i].occupied) {
          pushEvent("Exit - freed slot " + String(i+1) + " plate " + slots[i].plate);
          resetSlot(slots[i]);
          freed = true;
          break;
        }
      }
    }
    lcd.clear(); lcd.setCursor(0,0); lcd.print("Exit Detected");
    gateStepper.setSpeed(10);
    for (int i = 0; i < 512; ++i) gateStepper.step(1);
    delay(500);
    for (int i = 0; i < 512; ++i) gateStepper.step(-1);
    updateLCD();
  }
  if (exitState == HIGH) flagExit = 0;

  delay(10);
}
