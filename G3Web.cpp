// Interfaccia web per ThinkNode G3: stato e configurazione del repeater.
//
// - Il Wi-Fi si imposta dalla pagina stessa: senza rete salvata il G3 crea il
//   Wi-Fi "G3-Setup" (pagina su http://192.168.4.1/). SSID e password restano in flash.
// - Reset: pulsante Reload premuto 5 secondi = cancella il Wi-Fi e riparte in setup;
//   se la rete salvata non risponde per 45 s si riapre da sola la rete di setup.
// - Accesso protetto da utente "admin" e dalla password admin del repeater (HTTP Basic).
// - Solo HTTP: da usare in rete locale, non esporre su internet.
// - I comandi dalla pagina hanno gli stessi permessi dell'amministrazione remota
//   da app; erase, spegnimento, OTA e chiave privata sono bloccati.

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <string.h>
#include "G3Web.h"

#ifndef G3WEB_HOSTNAME
#define G3WEB_HOSTNAME "meshcore-g3"
#endif
#ifndef G3WEB_AP_NAME
#define G3WEB_AP_NAME "G3-Setup"
#endif
#ifndef USER_BTN_PRESSED
#define USER_BTN_PRESSED LOW
#endif
#define G3WEB_RESET_HOLD_MS 5000

namespace {

WebServer web(80);
Preferences g3prefs;

G3WebPasswordFn getPassword = nullptr;
G3WebExecFn execCmd = nullptr;

char saved_ssid[33] = "";
char saved_pwd[65] = "";
bool sta_cfg = false;
bool ap_on = false;
bool mdns_on = false;
uint32_t last_ok = 0;
uint32_t last_check = 0;
uint32_t restart_at = 0;
uint32_t lock_until = 0;
uint8_t fails = 0;

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="it"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>ThinkNode G3</title>
<style>
body{font-family:system-ui,sans-serif;margin:0;background:#101418;color:#e6e9ee}
header{padding:12px 16px;background:#1b2330;font-weight:600}
main{max-width:760px;margin:0 auto;padding:8px 12px}
section{background:#171d26;border-radius:10px;padding:12px 14px;margin:12px 0}
h2{font-size:15px;margin:0 0 8px;color:#9fb3c8}
table{width:100%;border-collapse:collapse}td{padding:3px 0;vertical-align:top}td:first-child{color:#9fb3c8;width:38%}
input,select,button{font:inherit;padding:6px 8px;border-radius:6px;border:1px solid #394456;background:#0f141b;color:inherit}
button{background:#2b6cb0;border-color:#2b6cb0;cursor:pointer}button.s{background:#2a3342;border-color:#394456}
.r{display:flex;gap:6px;margin:6px 0;flex-wrap:wrap}.r input{flex:1;min-width:90px}
.r select{flex:1;min-width:90px}label{display:block;font-size:12px;color:#9fb3c8;margin:8px 0 2px}.g{display:grid;grid-template-columns:1fr 1fr;gap:6px 10px}.g select,.g input{width:100%;box-sizing:border-box}small{display:block;color:#7f93a8;margin-top:6px;font-size:12px}
pre{white-space:pre-wrap;word-break:break-all;background:#0f141b;padding:8px;border-radius:6px;margin:6px 0;font-size:12px}
#m{position:sticky;top:0;padding:6px 16px;display:none}
</style></head><body>
<header>ThinkNode G3 &middot; MeshCore Repeater</header><div id="m"></div><main>
<section><h2>Stato</h2><table id="t"></table></section>
<section><h2>Statistiche</h2><pre id="core"></pre><pre id="rs"></pre><pre id="pk"></pre></section>
<section><h2>Vicini</h2><pre id="nb"></pre></section>
<section><h2>Configurazione</h2>
<div class="r"><input id="f_name" placeholder="Nome nodo"><button onclick="sv('set name ','f_name')">Salva nome</button></div>
<label>Radio</label>
<div class="r"><select id="f_preset" onchange="pre()"></select></div>
<div class="g">
<div><label>Frequenza (MHz)</label><input id="f_freq" type="number" step="0.001" min="150" max="2500"></div>
<div><label>Larghezza di banda (kHz)</label><select id="f_bw"></select></div>
<div><label>Spreading Factor</label><select id="f_sf"></select></div>
<div><label>Coding Rate</label><select id="f_cr"></select></div>
</div>
<label>Dimensione hash del percorso</label>
<div class="r"><select id="f_hm"></select></div>
<div class="r"><button onclick="rad()">Salva radio</button></div>
<small>Tutti i nodi della stessa rete devono usare gli stessi valori. Dopo il salvataggio serve un riavvio.</small>
<label>Potenza TX (dBm)</label>
<div class="r"><input id="f_tx" type="number" min="1" max="22" placeholder="Potenza TX (dBm)"><button onclick="sv('set tx ','f_tx')">Salva TX</button></div>
<label>Posizione</label>
<div class="r"><input id="f_lat" placeholder="Latitudine"><input id="f_lon" placeholder="Longitudine"><button onclick="pos()">Salva posizione</button></div>
<div class="r"><input id="f_pw" type="password" placeholder="Nuova password admin"><button onclick="pw()">Cambia password</button></div>
<div class="r"><button class="s" onclick="run('advert')">Invia advert</button><button class="s" onclick="st()">Sincronizza ora</button><button class="s" onclick="rb()">Riavvia</button></div>
</section>
<section><h2>Wi-Fi</h2>
<p style="margin:0 0 8px;font-size:13px;color:#9fb3c8">Se cambi rete o sbagli, tieni premuto il pulsante Reload per 5 secondi: il G3 cancella il Wi-Fi e crea la rete &quot;G3-Setup&quot; (pagina su 192.168.4.1). Se la rete salvata non risponde per 45 secondi, la crea da solo.</p>
<div class="r"><input id="f_ssid" placeholder="Nome rete (SSID)"><input id="f_wpw" type="password" placeholder="Password Wi-Fi"><button onclick="wf()">Salva e riavvia</button></div>
</section>
<section><h2>Console</h2>
<div class="r"><input id="f_cmd" placeholder="comando (es. get radio)" onkeydown="if(event.key=='Enter')go()"><button onclick="go()">Invia</button></div>
<pre id="out"></pre></section>
</main><script>
const $=i=>document.getElementById(i),H={'X-G3':'1','Content-Type':'application/x-www-form-urlencoded'};
function msg(t,bad){const m=$('m');m.textContent=t;m.style.display='block';m.style.background=bad?'#7a2a2a':'#245a3a';clearTimeout(msg.t);msg.t=setTimeout(()=>m.style.display='none',7000)}
async function post(u,b){const r=await fetch(u,{method:'POST',headers:H,body:b});if(r.status==401){location.reload();return null}if(!r.ok){return 'Errore '+r.status}const j=await r.json();return j.reply}
async function run(c,x){try{const r=await post('/api/cmd','c='+encodeURIComponent(c));if(r===null)return;msg((r||'OK')+(x||''));$('out').textContent='> '+c+'\n'+r}catch(e){msg('Nessuna risposta (il nodo potrebbe essersi riavviato)',1)}}
function sv(p,id,x){const v=$(id).value.trim();if(!v)return msg('Valore vuoto',1);return run(p+v,x)}
async function pos(){await sv('set lat ','f_lat');await sv('set lon ','f_lon')}
function pw(){const v=$('f_pw').value.trim();if(!v)return msg('Password vuota',1);if(confirm('Cambiare la password admin? Dovrai riaccedere.'))run('password '+v)}
function st(){run('time '+Math.floor(Date.now()/1000))}
function rb(){if(confirm('Riavviare il nodo?'))run('reboot')}
const PRE=[['Australia',915.8,250,10,5],['Australia (Narrow)',916.575,62.5,7,7],['Australia (Mid)',915.075,125,9,5],['Australia: SA, WA',923.125,62.5,8,8],['Australia: QLD',923.125,62.5,8,5],['Brazil',923.125,62.5,8,8],['Canada',910.525,62.5,7,5,3],['Costa Rica',910.525,125,11,5],['EU/UK (Narrow) - consigliato',869.618,62.5,8,8],['EU/UK (Deprecated)',869.525,250,11,5],['Czech Republic (Narrow)',869.432,62.5,7,5,2],['EU 433 MHz (Long Range)',433.65,250,11,5],['EU 433 MHz (Narrow)',433.65,62.5,8,8],['Hungary',869.618,62.5,7,5,2],['Netherlands',869.618,62.5,7,5],['Netherlands (Limburg)',869.618,62.5,8,8,2],['New Zealand (Narrow)',917.375,62.5,7,5,2],['New Zealand (Gisborne)',917.375,250,11,5,1],['Portugal 433',433.375,62.5,9,6],['Portugal 868',869.618,62.5,7,6],['Slovakia',869.618,62.5,7,5,2],['Switzerland',869.618,62.5,8,8],['Thailand',921,62.5,7,5],['USA',910.525,62.5,7,5],['USA - Southern California',927.875,62.5,7,5,3],['Vietnam (Narrow)',920.25,62.5,8,5],['Vietnam (Deprecated)',920.25,250,11,5]];
const BWS=[7.8,10.4,15.6,20.8,31.25,41.7,62.5,125,250,500];
function opt(sel,v,t){const o=document.createElement('option');o.value=v;o.textContent=t;sel.appendChild(o)}
function initRadio(){const p=$('f_preset');opt(p,'-1','Personalizzata');PRE.forEach((x,i)=>opt(p,i,x[0]+' - '+x[1]+' MHz / SF'+x[3]+' / BW'+x[2]+' / CR'+x[4]));
BWS.forEach(b=>opt($('f_bw'),b,b+' kHz'));for(let i=5;i<=12;i++)opt($('f_sf'),i,i);for(let i=5;i<=8;i++)opt($('f_cr'),i,i+' (4/'+i+')');opt($('f_hm'),0,'1 byte (massimo 64 hop)');opt($('f_hm'),1,'2 byte (massimo 32 hop)');opt($('f_hm'),2,'3 byte (massimo 21 hop)')}
function setRadio(f,b,sf,cr){$('f_freq').value=f;if(![...$('f_bw').options].some(o=>+o.value==b))opt($('f_bw'),b,b+' kHz');$('f_bw').value=b;$('f_sf').value=sf;$('f_cr').value=cr;
const k=PRE.findIndex(x=>x[1]==f&&x[2]==b&&x[3]==sf&&x[4]==cr);$('f_preset').value=k}
function pre(){const k=+$('f_preset').value;if(k<0)return;const x=PRE[k];setRadio(x[1],x[2],x[3],x[4]);if(x[5])$('f_hm').value=x[5]-1}
async function rad(){const f=parseFloat($('f_freq').value);if(!(f>=150&&f<=2500))return msg('Frequenza non valida',1);await run('set path.hash.mode '+$('f_hm').value);run('set radio '+f+','+$('f_bw').value+','+$('f_sf').value+','+$('f_cr').value,' - riavvia per applicare')}
function go(){const v=$('f_cmd').value.trim();if(v)run(v)}
async function wf(){const s=$('f_ssid').value.trim(),p=$('f_wpw').value;if(!s)return msg('SSID vuoto',1);if(p&&p.length<8)return msg('La password Wi-Fi deve avere almeno 8 caratteri',1);if(!confirm('Salvare il Wi-Fi e riavviare?'))return;try{const r=await post('/api/wifi','ssid='+encodeURIComponent(s)+'&pwd='+encodeURIComponent(p));if(r!==null)msg(r)}catch(e){msg('Riavvio in corso')}}
function fmt(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+' g ':'')+h+' h '+m+' min'}
let first=true;
async function refresh(){try{const r=await fetch('/api/status');if(r.status==401){location.reload();return}const s=await r.json(),w=s.wifi;
const rows=[['Nome',s.name],['Versione',s.ver],['Orologio (UTC)',s.clock],['Radio',s.radio],['Potenza TX',s.tx],['Hash percorso',isNaN(+s.hm)||s.hm===''?'?':(+s.hm+1)+' byte'],['Posizione',s.lat+', '+s.lon],['Wi-Fi',w.ssid?(w.ssid+' - '+w.ip+' - '+w.rssi+' dBm'):(w.ap?'modalità setup (rete G3-Setup)':'non connesso')],['Memoria libera',Math.round(s.heap/1024)+' kB'],['Attivo da',fmt(s.up)]];
const t=$('t');t.textContent='';rows.forEach(a=>{const tr=document.createElement('tr');a.forEach(x=>{const td=document.createElement('td');td.textContent=x;tr.appendChild(td)});t.appendChild(tr)});
$('core').textContent=s.core;$('rs').textContent=s.rs;$('pk').textContent=s.pk;$('nb').textContent=s.nb||'(nessuno)';
if(first){first=false;$('f_name').value=s.name;{const q=s.radio.split(',');if(q.length==4)setRadio(+q[0],+q[1],+q[2],+q[3]);if(s.hm!==''&&!isNaN(+s.hm))$('f_hm').value=+s.hm}$('f_tx').value=s.tx;$('f_lat').value=s.lat;$('f_lon').value=s.lon;$('f_ssid').value=w.cfg||''}
}catch(e){}}
initRadio();refresh();setInterval(refresh,10000);
</script></body></html>)HTML";

// ---- utilita' -------------------------------------------------------------

// Esegue un comando CLI; il risultato finisce in out (sempre terminato).
void runCmd(const char* c, uint32_t ts, char* out, size_t n) {
  char buf[160];
  strncpy(buf, c, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  for (char* p = buf; *p; ++p) {      // una sola riga: tronca al primo a capo
    if (*p == '\r' || *p == '\n') { *p = 0; break; }
  }
  char reply[512];
  reply[0] = 0;
  if (execCmd) execCmd(ts, buf, reply);
  strncpy(out, reply, n - 1);
  out[n - 1] = 0;
}

// Comandi non permessi dalla pagina web (restano disponibili da seriale).
bool blocked(const char* c) {
  static const char* const bad[] = {"erase", "poweroff", "shutdown", "start ota", "set prv.key", "get prv.key"};
  while (*c == ' ') c++;
  for (const char* b : bad) {
    size_t l = strlen(b);
    if (strncmp(c, b, l) == 0 && (c[l] == 0 || c[l] == ' ')) return true;
  }
  return false;
}

void jesc(String& o, const char* s) {
  o += '"';
  for (; *s; ++s) {
    unsigned char ch = (unsigned char)*s;
    if (ch == '"' || ch == '\\') { o += '\\'; o += (char)ch; }
    else if (ch == '\n') o += "\\n";
    else if (ch == '\r') { }
    else if (ch < 0x20) o += ' ';
    else o += (char)ch;
  }
  o += '"';
}

void field(String& o, const char* key, const char* cmd, uint32_t ts) {
  char r[512];
  runCmd(cmd, ts, r, sizeof(r));
  const char* v = r;
  if (v[0] == '>' && v[1] == ' ') v += 2;     // toglie il prefisso "> " delle risposte get
  o += '"'; o += key; o += "\":";
  jesc(o, v);
  o += ',';
}

// Autenticazione: utente "admin" + password admin del repeater.
bool authed() {
  if (millis() < lock_until) {
    web.send(429, "text/plain", "Troppi tentativi, riprova tra un minuto");
    return false;
  }
  const char* pw = getPassword ? getPassword() : "";
  if (!pw || !pw[0]) {
    web.send(403, "text/plain", "Password admin non impostata");
    return false;
  }
  if (web.authenticate("admin", pw)) { fails = 0; return true; }
  if (web.hasHeader("Authorization")) {   // credenziali presenti ma sbagliate
    if (++fails >= 5) { fails = 0; lock_until = millis() + 60000; }
  }
  web.requestAuthentication();
  return false;
}

// Protezione CSRF: le richieste di modifica devono avere l'intestazione X-G3.
bool csrfOk() {
  if (web.header("X-G3") == "1") return true;
  web.send(403, "text/plain", "Richiesta non valida");
  return false;
}

void sendReply(const char* reply) {
  String o;
  o.reserve(strlen(reply) + 16);
  o += "{\"reply\":";
  jesc(o, reply);
  o += '}';
  web.send(200, "application/json", o);
}

// ---- gestori --------------------------------------------------------------

void handleRoot() {
  if (!authed()) return;
  web.send_P(200, "text/html; charset=utf-8", PAGE);
}

void handleStatus() {
  if (!authed()) return;
  String o;
  o.reserve(2600);
  o += '{';
  field(o, "name", "get name", 1);
  field(o, "ver", "ver", 1);
  field(o, "clock", "clock", 1);
  field(o, "radio", "get radio", 1);
  field(o, "tx", "get tx", 1);
  field(o, "hm", "get path.hash.mode", 1);
  field(o, "lat", "get lat", 1);
  field(o, "lon", "get lon", 1);
  field(o, "core", "stats-core", 0);       // sola lettura: scelti dal firmware, non dall'utente
  field(o, "rs", "stats-radio", 0);
  field(o, "pk", "stats-packets", 0);
  field(o, "nb", "neighbors", 1);

  bool up = (WiFi.status() == WL_CONNECTED);
  o += "\"wifi\":{\"ssid\":";
  jesc(o, up ? WiFi.SSID().c_str() : "");
  o += ",\"ip\":";
  jesc(o, up ? WiFi.localIP().toString().c_str() : "");
  o += ",\"rssi\":";
  o += (up ? WiFi.RSSI() : 0);
  o += ",\"ap\":";
  o += (ap_on ? "true" : "false");
  o += ",\"cfg\":";
  jesc(o, saved_ssid);
  o += "},\"heap\":";
  o += (uint32_t)ESP.getFreeHeap();
  o += ",\"up\":";
  o += (uint32_t)(millis() / 1000);
  o += '}';
  web.send(200, "application/json", o);
}

void handleCmd() {
  if (!authed() || !csrfOk()) return;
  String c = web.arg("c");
  c.trim();
  if (c.length() == 0 || c.length() > 150) { web.send(400, "text/plain", "Comando non valido"); return; }
  if (blocked(c.c_str())) { sendReply("Comando non consentito dalla pagina web (usa la seriale)"); return; }
  char r[512];
  runCmd(c.c_str(), 1, r, sizeof(r));        // ts != 0: stessi permessi dell'amministrazione remota
  sendReply(r);
}

void handleWifi() {
  if (!authed() || !csrfOk()) return;
  String s = web.arg("ssid");
  String p = web.arg("pwd");
  s.trim();
  if (s.length() == 0 || s.length() > 32 || p.length() > 63 || (p.length() > 0 && p.length() < 8)) {
    web.send(400, "text/plain", "SSID o password non validi");
    return;
  }
  g3prefs.begin("g3web", false);
  g3prefs.putString("ssid", s);
  g3prefs.putString("pwd", p);
  g3prefs.end();
  sendReply("Wi-Fi salvato, riavvio in corso...");
  restart_at = millis() + 1500;
  if (restart_at == 0) restart_at = 1;
}

// Cancella il Wi-Fi salvato e riavvia: al prossimo avvio parte la rete "G3-Setup".
void resetWifi() {
  g3prefs.begin("g3web", false);
  g3prefs.remove("ssid");
  g3prefs.remove("pwd");
  g3prefs.end();
  Serial.println("[G3Web] Wi-Fi cancellato: riavvio in modalita' setup");
#ifdef P_LORA_TX_LED                    // LED LoRa del G3 (attivo basso): 3 lampi di conferma
  for (int i = 0; i < 3; i++) {
    digitalWrite(P_LORA_TX_LED, LOW);  delay(120);
    digitalWrite(P_LORA_TX_LED, HIGH); delay(120);
  }
#endif
  restart_at = millis() + 300;
  if (restart_at == 0) restart_at = 1;
}

void handleNotFound() {
  web.send(404, "text/plain", "Non trovato");
}

}  // namespace

// ---- API pubblica ---------------------------------------------------------

void g3web_start(G3WebPasswordFn password, G3WebExecFn exec) {
  getPassword = password;
  execCmd = exec;

  g3prefs.begin("g3web", false);
  String s = g3prefs.getString("ssid", "");
  String p = g3prefs.getString("pwd", "");
  g3prefs.end();
  strncpy(saved_ssid, s.c_str(), sizeof(saved_ssid) - 1);
  strncpy(saved_pwd, p.c_str(), sizeof(saved_pwd) - 1);
  sta_cfg = saved_ssid[0] != 0;

  WiFi.persistent(false);              // le credenziali stanno in Preferences, non nella config del Wi-Fi
  WiFi.setAutoReconnect(true);
  if (sta_cfg) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(G3WEB_HOSTNAME);
    WiFi.begin(saved_ssid, saved_pwd);
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(G3WEB_AP_NAME);        // rete aperta, ma ogni pagina richiede la password admin
    ap_on = true;
  }
  last_ok = millis();
#ifdef PIN_USER_BTN
  pinMode(PIN_USER_BTN, INPUT_PULLUP);
#endif

  web.on("/", HTTP_GET, handleRoot);
  web.on("/api/status", HTTP_GET, handleStatus);
  web.on("/api/cmd", HTTP_POST, handleCmd);
  web.on("/api/wifi", HTTP_POST, handleWifi);
  web.onNotFound(handleNotFound);
  static const char* keys[] = {"X-G3"};
  web.collectHeaders(keys, 1);
  web.begin();
}

void g3web_poll() {
  web.handleClient();
  uint32_t now = millis();
#ifdef PIN_USER_BTN
  // Pressione lunga del pulsante Reload = reset del Wi-Fi. Si arma solo dopo aver visto
  // il pulsante rilasciato almeno una volta, cosi' un livello inatteso non cancella nulla.
  static bool btn_armed = false;
  static uint32_t btn_down_at = 0;
  if (digitalRead(PIN_USER_BTN) != USER_BTN_PRESSED) {
    btn_armed = true;
    btn_down_at = 0;
  } else if (btn_armed && !restart_at) {
    if (btn_down_at == 0) btn_down_at = now ? now : 1;
    else if (now - btn_down_at >= G3WEB_RESET_HOLD_MS) resetWifi();
  }
#endif
  if (restart_at && (int32_t)(now - restart_at) >= 0) ESP.restart();
  if (now - last_check < 2000) return;
  last_check = now;

  if (WiFi.status() == WL_CONNECTED) {
    last_ok = now;
    if (!mdns_on && MDNS.begin(G3WEB_HOSTNAME)) {
      MDNS.addService("http", "tcp", 80);
      mdns_on = true;
      Serial.print("[G3Web] http://");
      Serial.print(WiFi.localIP());
      Serial.println("/");
    }
    if (ap_on && sta_cfg) {            // connesso: spegni la rete di setup
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      ap_on = false;
    }
  } else if (sta_cfg && !ap_on && now - last_ok > 45000) {
    WiFi.mode(WIFI_AP_STA);            // rete irraggiungibile: riapri il setup, continua a riprovare
    WiFi.softAP(G3WEB_AP_NAME);
    ap_on = true;
  }
}
