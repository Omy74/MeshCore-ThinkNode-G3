// Interfaccia web per ThinkNode G3: stato e configurazione del repeater.
//
// - Il Wi-Fi si imposta dalla pagina stessa: senza rete salvata il G3 crea il
//   Wi-Fi di setup "Repeater-setup" (pagina su http://192.168.4.1/). SSID e password restano in flash.
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

#ifdef G3WEB_ETH_PROBE
#include <SPI.h>
#include "esp_system.h"
#include "esp_netif.h"
#include "lwip/netif.h"
#include "lwip/netifapi.h"
#include "lwip/tcpip.h"
#include "lwip/dhcp.h"
#include "lwip/etharp.h"
#include "lwip/pbuf.h"
#include "lwip/ethip6.h"
#include "netif/ethernet.h"
// Pin del CH390 ricavati dal firmware originale del G3 (SPI3, modo 0, 10 MHz).
#ifndef G3ETH_SCLK
#define G3ETH_SCLK 13
#define G3ETH_MISO 12
#define G3ETH_MOSI 11
#define G3ETH_CS   14
#define G3ETH_INT  10
#endif
#endif
#ifndef G3WEB_HOSTNAME
#define G3WEB_HOSTNAME "meshcore-repeater"
#endif
#ifndef G3WEB_BTN_NAME
#define G3WEB_BTN_NAME "Reload"
#endif
#ifndef G3WEB_AP_NAME
#define G3WEB_AP_NAME "Repeater-setup"
#endif
#ifndef USER_BTN_PRESSED
#define USER_BTN_PRESSED LOW
#endif
#define G3WEB_RESET_HOLD_MS 5000

namespace {

WebServer web(80);
#ifdef G3WEB_ETH_PROBE
// Ethernet via CH390 (compatibile DM9051) su SPI: driver minimo + interfaccia lwIP con DHCP.
SPIClass ethSPI(FSPI);                    // la radio LoRa usa gia' HSPI (SPI3): qui serve l'altro bus
SemaphoreHandle_t eth_mtx = nullptr;
struct netif eth_if;
uint16_t eth_vid = 0, eth_pid = 0;
uint8_t eth_rev = 0, eth_nsr = 0, eth_ncr = 0;
bool eth_ready = false, eth_link = false, eth_dhcp = false, eth_ip_logged = false;
uint32_t eth_rx = 0, eth_tx = 0, eth_rxerr = 0, eth_txerr = 0, eth_resets = 0;
uint8_t eth_mac[6];
uint8_t eth_rxbuf[1600];

enum { R_NCR = 0x00, R_NSR = 0x01, R_TCR = 0x02, R_RCR = 0x05, R_BPTR = 0x08, R_FCTR = 0x09, R_FCR = 0x0A,
       R_WCR = 0x0F, R_PAR = 0x10, R_MAR = 0x16, R_GPCR = 0x1E, R_GPR = 0x1F, R_TXPLL = 0x7C, R_TXPLH = 0x7D,
       R_MRCMDX = 0x70, R_MRCMD = 0x72, R_MWCMD = 0x78, R_MPCR = 0x55, R_ISR = 0xFE, R_IMR = 0xFF };

inline void csLow()  { ethSPI.beginTransaction(SPISettings(10000000, MSBFIRST, SPI_MODE0)); digitalWrite(G3ETH_CS, LOW); }
inline void csHigh() { digitalWrite(G3ETH_CS, HIGH); ethSPI.endTransaction(); }

uint8_t ethRead(uint8_t reg) {
  csLow();
  ethSPI.transfer(reg & 0x7F);              // bit 7 = 0: lettura
  uint8_t v = ethSPI.transfer(0);
  csHigh();
  return v;
}

void ethWrite(uint8_t reg, uint8_t v) {
  csLow();
  ethSPI.transfer(0x80 | reg);              // bit 7 = 1: scrittura
  ethSPI.transfer(v);
  csHigh();
}

void ethRxReset() {                         // ripristino del motore di ricezione dopo un errore
  ethWrite(R_RCR, 0x00);
  ethWrite(R_MPCR, 0x02);                   // svuota la FIFO
  ethWrite(R_MPCR, 0x01);                   // reset puntatori
  delay(2);
  ethWrite(R_ISR, 0x3F);
  ethWrite(R_RCR, 0x31);                    // RX on, scarta CRC e pacchetti lunghi
  eth_resets++;
}

void ethHwInit() {
  ethWrite(R_GPCR, 0x01);
  ethWrite(R_GPR, 0x00);                    // accende il PHY interno
  delay(20);
  ethWrite(R_NCR, 0x01);                    // reset del MAC
  delay(5);
  ethWrite(R_NCR, 0x00);
  ethWrite(R_GPR, 0x00);
  delay(50);
  ethWrite(R_NCR, 0x00);
  ethWrite(R_TCR, 0x00);
  ethWrite(R_BPTR, 0x3F);
  ethWrite(R_FCTR, 0x38);
  ethWrite(R_FCR, 0x00);
  ethWrite(R_WCR, 0x00);
  for (int i = 0; i < 6; i++) ethWrite(R_PAR + i, eth_mac[i]);
  for (int i = 0; i < 8; i++) ethWrite(R_MAR + i, 0xFF);   // accetta broadcast e multicast
  ethWrite(R_NSR, 0x2C);                    // azzera i flag di stato
  ethWrite(R_ISR, 0x3F);
  ethWrite(R_IMR, 0x80);                    // puntatore memoria circolare (niente interrupt: si legge a polling)
  ethWrite(R_RCR, 0x31);                    // RX on, scarta CRC e pacchetti lunghi
}

err_t ethLowOutput(struct netif*, struct pbuf* p) {
  if (!eth_ready || !eth_link || p->tot_len > 1514) return ERR_IF;
  if (xSemaphoreTake(eth_mtx, pdMS_TO_TICKS(50)) != pdTRUE) return ERR_TIMEOUT;
  uint32_t t0 = millis();
  while (ethRead(R_TCR) & 0x01) {           // attende la fine dell'invio precedente
    if (millis() - t0 > 20) { eth_txerr++; xSemaphoreGive(eth_mtx); return ERR_TIMEOUT; }
  }
  uint16_t len = p->tot_len;
  ethWrite(R_TXPLL, len & 0xFF);
  ethWrite(R_TXPLH, len >> 8);
  csLow();
  ethSPI.transfer(0x80 | R_MWCMD);
  for (struct pbuf* q = p; q; q = q->next) ethSPI.writeBytes((const uint8_t*)q->payload, q->len);
  csHigh();
  ethWrite(R_TCR, 0x01);                    // avvia l'invio
  eth_tx++;
  xSemaphoreGive(eth_mtx);
  return ERR_OK;
}

err_t ethNetifInit(struct netif* n) {
  n->name[0] = 'e'; n->name[1] = 't';
  n->mtu = 1500;
  n->hwaddr_len = 6;
  memcpy(n->hwaddr, eth_mac, 6);
  n->output = etharp_output;
#if LWIP_IPV6
  n->output_ip6 = ethip6_output;
#endif
  n->linkoutput = ethLowOutput;
  n->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP;
#if LWIP_IGMP
  n->flags |= NETIF_FLAG_IGMP;
#endif
#if LWIP_NETIF_HOSTNAME
  n->hostname = G3WEB_HOSTNAME;
#endif
  return ERR_OK;
}

// Legge un pacchetto dalla memoria del chip; true se ne ha consumato uno.
bool ethRxOne() {
  ethRead(R_MRCMDX);                        // lettura a vuoto: aggiorna lo stato di pronto
  uint8_t rdy = ethRead(R_MRCMDX);
  if (rdy == 0) return false;
  if (rdy != 1) { eth_rxerr++; ethRxReset(); return false; }
  uint8_t h[4];
  csLow();
  ethSPI.transfer(R_MRCMD);
  ethSPI.transferBytes(nullptr, h, 4);
  uint16_t len = h[2] | (h[3] << 8);        // comprende i 4 byte di CRC
  if (len < 18 || len > sizeof(eth_rxbuf)) { csHigh(); eth_rxerr++; ethRxReset(); return false; }
  ethSPI.transferBytes(nullptr, eth_rxbuf, len);
  csHigh();
  if (h[1] & 0xBF) { eth_rxerr++; return true; }      // pacchetto con errori: scartato
  len -= 4;
  struct pbuf* p = pbuf_alloc(PBUF_RAW, len, PBUF_POOL);
  if (!p) { eth_rxerr++; return true; }
  pbuf_take(p, eth_rxbuf, len);
  eth_rx++;
  if (eth_if.input(p, &eth_if) != ERR_OK) pbuf_free(p);
  return true;
}

bool ethHasIp() { return eth_link && !ip4_addr_isany_val(*netif_ip4_addr(&eth_if)); }

void ethTask(void*) {
  uint32_t last_link = 0;
  for (;;) {
    uint32_t now = millis();
    if (xSemaphoreTake(eth_mtx, pdMS_TO_TICKS(20)) == pdTRUE) {
      if (now - last_link >= 500) {
        last_link = now;
        eth_nsr = ethRead(R_NSR);
        eth_ncr = ethRead(R_NCR);
        bool l = (eth_nsr & 0x40) != 0;
        if (l != eth_link) {
          eth_link = l;
          xSemaphoreGive(eth_mtx);
          if (l) {
            Serial.println("[G3Web] Ethernet: cavo collegato");
            netifapi_netif_set_link_up(&eth_if);
            netifapi_dhcp_start(&eth_if);
            eth_dhcp = true;
          } else {
            Serial.println("[G3Web] Ethernet: cavo scollegato");
            netifapi_dhcp_stop(&eth_if);
            eth_dhcp = false;
            ip4_addr_t z; ip4_addr_set_zero(&z);
            netifapi_netif_set_addr(&eth_if, &z, &z, &z);
            netifapi_netif_set_link_down(&eth_if);
            eth_ip_logged = false;
          }
          xSemaphoreTake(eth_mtx, portMAX_DELAY);
        }
      }
      if (eth_link) {
        int n = 0;
        while (n++ < 8 && ethRxOne()) {}
      }
      xSemaphoreGive(eth_mtx);
    }
    if (eth_link && !eth_ip_logged && !ip4_addr_isany_val(*netif_ip4_addr(&eth_if))) {
      eth_ip_logged = true;
      if (!netif_default) netifapi_netif_set_default(&eth_if);
      Serial.printf("[G3Web] Ethernet: http://%s/\n", ip4addr_ntoa(netif_ip4_addr(&eth_if)));
    }
    vTaskDelay(pdMS_TO_TICKS(eth_link ? 2 : 100));
  }
}

void ethStart() {
  pinMode(G3ETH_CS, OUTPUT);
  digitalWrite(G3ETH_CS, HIGH);
  pinMode(G3ETH_INT, INPUT);
  ethSPI.begin(G3ETH_SCLK, G3ETH_MISO, G3ETH_MOSI, -1);
  eth_mtx = xSemaphoreCreateMutex();
  esp_read_mac(eth_mac, ESP_MAC_ETH);
  ethHwInit();
  eth_vid = ethRead(0x28) | (ethRead(0x29) << 8);
  eth_pid = ethRead(0x2A) | (ethRead(0x2B) << 8);
  eth_rev = ethRead(0x2C);
  eth_nsr = ethRead(R_NSR);
  Serial.printf("[G3Web] CH390: VID=%04X PID=%04X REV=%02X NSR=%02X\n", eth_vid, eth_pid, eth_rev, eth_nsr);
  if (eth_vid != 0x1C00) return;            // chip assente o non risponde: niente rete cablata
  esp_netif_init();                         // avvia lwIP anche con il Wi-Fi spento
  ip4_addr_t z; ip4_addr_set_zero(&z);
  netifapi_netif_add(&eth_if, &z, &z, &z, nullptr, ethNetifInit, tcpip_input);
  netifapi_netif_set_up(&eth_if);
  eth_ready = true;
  xTaskCreatePinnedToCore(ethTask, "g3eth", 4096, nullptr, 3, nullptr, 0);
}
#endif

Preferences g3prefs;

G3WebPasswordFn getPassword = nullptr;
G3WebExecFn execCmd = nullptr;

char saved_ssid[33] = "";
char saved_pwd[65] = "";
bool sta_cfg = false;
bool wifi_on = true;
int8_t wifi_pending = -1;           // -1 nessuna richiesta, 0 spegni, 1 accendi
uint32_t wifi_pending_at = 0;
bool ap_on = false;
bool mdns_on = false;
uint32_t last_ok = 0;
uint32_t last_check = 0;
uint32_t last_try = 0;
uint32_t restart_at = 0;
uint32_t lock_until = 0;
uint8_t fails = 0;

const char PAGE[] PROGMEM = R"HTML(<!doctype html><html lang="it"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>MeshCore Repeater</title>
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
.r select{flex:1;min-width:90px}label{display:block;font-size:12px;color:#9fb3c8;margin:8px 0 2px}.g{display:grid;grid-template-columns:1fr 1fr;gap:6px 10px}.g select,.g input{width:100%;box-sizing:border-box}svg.c{width:100%;height:auto;display:block;background:#0f141b;border-radius:8px}.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(130px,1fr));gap:8px}.k{background:#0f141b;border-radius:8px;padding:8px 10px}.k b{display:block;font-size:17px;margin-top:2px}.k span{font-size:12px;color:#9fb3c8}.lg{font-size:12px;color:#9fb3c8;margin:4px 0 10px;min-height:16px}summary{cursor:pointer;color:#9fb3c8;font-size:13px;margin-top:10px}
small{display:block;color:#7f93a8;margin-top:6px;font-size:12px}
pre{white-space:pre-wrap;word-break:break-all;background:#0f141b;padding:8px;border-radius:6px;margin:6px 0;font-size:12px}
#m{position:sticky;top:0;padding:6px 16px;display:none}
</style></head><body>
<header id="hd">MeshCore Repeater</header><div id="m"></div><main>
<section><h2>Stato</h2><table id="t"></table></section>
<section><h2>Statistiche</h2><div class="cards" id="cards"></div><details><summary>Dati grezzi</summary><pre id="core"></pre><pre id="rs"></pre><pre id="pk"></pre></details></section>
<section><h2>Grafici</h2>
<label>Segnale radio (dBm)</label><div id="ch1"></div>
<label>Qualità del segnale, SNR (dB)</label><div id="ch2"></div>
<label>Traffico (pacchetti al minuto)</label><div id="ch3"></div>
<small>I grafici si costruiscono mentre la pagina è aperta (un campione ogni 10 secondi, ultima ora) e ripartono da zero se la ricarichi. Tocca o passa sopra un grafico per leggere i valori.</small></section>
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
<label>Modalità ripetitore</label>
<div class="r"><button class="s" id="b_rep" onclick="rep()">Disattiva ripetizione</button></div>
<small>Spento, il nodo non ripete i messaggi degli altri ma funziona e si configura come sempre.</small>
<label>Potenza TX (dBm)</label>
<div class="r"><input id="f_tx" type="number" min="1" max="22" placeholder="Potenza TX (dBm)"><button onclick="sv('set tx ','f_tx')">Salva TX</button></div>
<label>Posizione</label>
<div class="r"><input id="f_lat" placeholder="Latitudine"><input id="f_lon" placeholder="Longitudine"><button onclick="pos()">Salva posizione</button></div>
<div class="r"><input id="f_pw" type="password" placeholder="Nuova password admin"><button onclick="pw()">Cambia password</button></div>
<div class="r"><button class="s" onclick="run('advert')">Invia advert</button><button class="s" onclick="st()">Sincronizza ora</button><button class="s" onclick="rb()">Riavvia</button></div>
</section>
<section><h2>Wi-Fi</h2>
<p id="rst" style="margin:0 0 8px;font-size:13px;color:#9fb3c8"></p>
<div class="r"><button class="s" id="b_wen" onclick="wen()">Disattiva Wi-Fi</button></div>
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
function setRadio(f,b,sf,cr){f=Math.round(f*1000)/1000;$('f_freq').value=f;if(![...$('f_bw').options].some(o=>+o.value==b))opt($('f_bw'),b,b+' kHz');$('f_bw').value=b;$('f_sf').value=sf;$('f_cr').value=cr;
const k=PRE.findIndex(x=>Math.abs(x[1]-f)<.0005&&x[2]==b&&x[3]==sf&&x[4]==cr);$('f_preset').value=k}
function pre(){const k=+$('f_preset').value;if(k<0)return;const x=PRE[k];setRadio(x[1],x[2],x[3],x[4]);if(x[5])$('f_hm').value=x[5]-1}
async function rad(){const f=parseFloat($('f_freq').value);if(!(f>=150&&f<=2500))return msg('Frequenza non valida',1);await run('set path.hash.mode '+$('f_hm').value);run('set radio '+f+','+$('f_bw').value+','+$('f_sf').value+','+$('f_cr').value,' - riavvia per applicare')}
function go(){const v=$('f_cmd').value.trim();if(v)run(v)}
async function wf(){const s=$('f_ssid').value.trim(),p=$('f_wpw').value;if(!s)return msg('SSID vuoto',1);if(p&&p.length<8)return msg('La password Wi-Fi deve avere almeno 8 caratteri',1);if(!confirm('Salvare il Wi-Fi e riavviare?'))return;try{const r=await post('/api/wifi','ssid='+encodeURIComponent(s)+'&pwd='+encodeURIComponent(p));if(r!==null)msg(r)}catch(e){msg('Riavvio in corso')}}
let WEN=true,REP=true;
async function rep(){await run('set repeat '+(REP?'off':'on'));refresh()}
async function wen(){const msgt=WEN?'Disattivare il Wi-Fi? La pagina sarà raggiungibile solo dalla rete cablata. Se non hai la LAN attiva non potrai più aprirla: per riattivare il Wi-Fi tieni premuto il pulsante di reset per 5 secondi.':'Riattivare il Wi-Fi?';if(!confirm(msgt))return;try{const r=await post('/api/wifien','en='+(WEN?0:1));if(r!==null){msg(r);WEN=!WEN;$('b_wen').textContent=WEN?'Disattiva Wi-Fi':'Attiva Wi-Fi'}}catch(e){msg('Collegamento interrotto: il Wi-Fi potrebbe essersi spento',1)}}
function ethTxt(e){if(e.vid!==0x1c00)return 'chip non rilevato';if(!e.ready)return 'errore di avvio';
if(!e.link)return 'cavo scollegato';return e.spd+' Mbps '+(e.fdx?'full':'half')+' duplex - '+(e.ip&&e.ip!=='0.0.0.0'?e.ip+' (gateway '+e.gw+')':'attesa indirizzo DHCP...')+' - rx '+e.rx+' tx '+e.tx+(e.err?' err '+e.err:'')}
function fmt(s){const d=Math.floor(s/86400),h=Math.floor(s%86400/3600),m=Math.floor(s%3600/60);return (d?d+' g ':'')+h+' h '+m+' min'}
const HMAX=360,HI={t:[],noise:[],rssi:[],snr:[],rx:[],tx:[]};let prevC=null;
function hpush(k,v){const a=HI[k];a.push(v);if(a.length>HMAX)a.shift()}
const NS='http://www.w3.org/2000/svg';
function se(n,a,p){const e=document.createElementNS(NS,n);for(const k in a)e.setAttribute(k,a[k]);if(p)p.appendChild(e);return e}
function fv(v,d){return v==null?'-':(+v).toFixed(d)}
function chart(id,defs,o){const box=$(id);box.textContent='';const W=600,Ht=170,L=46,R=8,T=8,B=20,n=HI.t.length;
const svg=se('svg',{viewBox:'0 0 '+W+' '+Ht,'class':'c'},box);
let lo=1e9,hi=-1e9;defs.forEach(d=>HI[d.k].forEach(v=>{if(v!=null){if(v<lo)lo=v;if(v>hi)hi=v}}));
if(lo>hi){const t=se('text',{x:W/2,y:Ht/2,fill:'#9fb3c8','text-anchor':'middle','font-size':13},svg);t.textContent='in attesa di dati...';return}
if(o.zero)lo=0;if(hi-lo<1){hi+=.5;if(!o.zero)lo-=.5}
const pad=(hi-lo)*.1;hi+=pad;if(!o.zero)lo-=pad;
const X=i=>L+(W-L-R)*(n>1?i/(n-1):1),Y=v=>T+(Ht-T-B)*(1-(v-lo)/(hi-lo));
for(let g=0;g<=4;g++){const v=lo+(hi-lo)*g/4,y=Y(v);se('line',{x1:L,x2:W-R,y1:y,y2:y,stroke:'#2a3342','stroke-width':1},svg);const t=se('text',{x:L-5,y:y+4,fill:'#9fb3c8','text-anchor':'end','font-size':11},svg);t.textContent=fv(v,o.dec)}
const ago=n>1?Math.round((HI.t[n-1]-HI.t[0])/1000):0;
let t1=se('text',{x:L,y:Ht-5,fill:'#9fb3c8','font-size':11},svg);t1.textContent=ago>=90?'-'+Math.round(ago/60)+' min':'-'+ago+' s';
t1=se('text',{x:W-R,y:Ht-5,fill:'#9fb3c8','text-anchor':'end','font-size':11},svg);t1.textContent='ora';
defs.forEach(d=>{let p='',pen=false;HI[d.k].forEach((v,i)=>{if(v==null){pen=false;return}p+=(pen?'L':'M')+X(i).toFixed(1)+' '+Y(v).toFixed(1);pen=true});
const a={d:p,fill:'none',stroke:d.c,'stroke-width':2,'stroke-linejoin':'round'};if(d.dash)a['stroke-dasharray']='6 4';se('path',a,svg)});
const cur=se('line',{x1:0,x2:0,y1:T,y2:Ht-B,stroke:'#9fb3c8','stroke-width':1,visibility:'hidden'},svg);
const lg=document.createElement('div');lg.className='lg';box.appendChild(lg);
function show(i,hov){const tm=hov?new Date(HI.t[i]).toLocaleTimeString('it-IT')+'  ':'ora:  ';lg.textContent=tm+defs.map(d=>(d.dash?'- - ':'— ')+d.n+' '+fv(HI[d.k][i],o.dec)+o.u).join('    ');
if(hov){cur.setAttribute('x1',X(i));cur.setAttribute('x2',X(i));cur.setAttribute('visibility','visible')}else cur.setAttribute('visibility','hidden')}
show(n-1,false);
function mv(e){const r=svg.getBoundingClientRect(),c=e.touches?e.touches[0]:e,x=(c.clientX-r.left)/r.width*W;let i=Math.round((x-L)/(W-L-R)*(n-1));i=Math.max(0,Math.min(n-1,i));show(i,true)}
svg.addEventListener('mousemove',mv);svg.addEventListener('touchmove',mv,{passive:true});svg.addEventListener('touchstart',mv,{passive:true});svg.addEventListener('mouseleave',()=>show(n-1,false))}
function fa(x){return x<60?x+' s':fmt(x)}
function jp(x){try{return JSON.parse(x)||{}}catch(e){return {}}}
function cards(C,Rr,P,up){const k=[];
if(C.battery_mv>0)k.push(['Batteria',(C.battery_mv/1000).toFixed(2)+' V']);
k.push(['Coda in uscita',C.queue_len],['Errori',C.errors],['Rumore di fondo',Rr.noise_floor+' dBm'],['Ultimo RSSI',Rr.last_rssi+' dBm'],['Ultimo SNR',fv(Rr.last_snr,2)+' dB'],['Tempo in trasmissione',fa(Rr.tx_air_secs||0)],['Tempo in ricezione',fa(Rr.rx_air_secs||0)],['Occupazione TX',up>0?fv((Rr.tx_air_secs||0)*100/up,2)+' %':'-'],['Pacchetti ricevuti',P.recv],['Pacchetti inviati',P.sent],['Flood RX / TX',P.flood_rx+' / '+P.flood_tx],['Direct RX / TX',P.direct_rx+' / '+P.direct_tx],['Errori di ricezione',P.recv_errors]);
const c=$('cards');c.textContent='';k.forEach(a=>{const d=document.createElement('div');d.className='k';const sp=document.createElement('span');sp.textContent=a[0];const b=document.createElement('b');b.textContent=a[1]==null||a[1]==='undefined dBm'?'-':a[1];d.appendChild(sp);d.appendChild(b);c.appendChild(d)})}
function sample(Rr,P){const now=Date.now();let rx=null,tx=null;
if(prevC&&P.recv!=null){const dt=(now-prevC.t)/60000;if(dt>0&&P.recv>=prevC.r&&P.sent>=prevC.s){rx=(P.recv-prevC.r)/dt;tx=(P.sent-prevC.s)/dt}}
prevC=P.recv!=null?{t:now,r:P.recv,s:P.sent}:null;
const nr=(Rr.last_rssi===0&&Rr.last_snr===0);
hpush('t',now);hpush('noise',Rr.noise_floor==null?null:Rr.noise_floor);hpush('rssi',nr||Rr.last_rssi==null?null:Rr.last_rssi);hpush('snr',nr||Rr.last_snr==null?null:Rr.last_snr);hpush('rx',rx);hpush('tx',tx)}
function charts(){chart('ch1',[{n:'Rumore di fondo',k:'noise',c:'#4da3ff'},{n:'Ultimo RSSI',k:'rssi',c:'#f6ad55',dash:1}],{u:' dBm',dec:0});chart('ch2',[{n:'SNR',k:'snr',c:'#68d391'}],{u:' dB',dec:1});chart('ch3',[{n:'Ricevuti',k:'rx',c:'#4da3ff'},{n:'Inviati',k:'tx',c:'#f6ad55',dash:1}],{u:'/min',dec:1,zero:1})}
let first=true;
async function refresh(){try{const r=await fetch('/api/status');if(r.status==401){location.reload();return}const s=await r.json(),w=s.wifi;
const rows=[['Nome',s.name],['Versione',s.ver],['Orologio (UTC)',s.clock],['Radio',s.radio],['Potenza TX',s.tx],['Ripetizione',s.rep==='off'?'disattivata':'attiva'],['Hash percorso',isNaN(+s.hm)||s.hm===''?'?':(+s.hm+1)+' byte'],['Posizione',s.lat+', '+s.lon],['Wi-Fi',s.wen===false?'disattivato':w.ssid?(w.ssid+' - '+w.ip+' - '+w.rssi+' dBm'):(w.ap?'modalità setup (rete '+s.apname+')':'non connesso')],['Memoria libera',Math.round(s.heap/1024)+' kB'],['Attivo da',fmt(s.up)]];
if(s.eth)rows.push(['Ethernet',ethTxt(s.eth)]);
const t=$('t');t.textContent='';rows.forEach(a=>{const tr=document.createElement('tr');a.forEach(x=>{const td=document.createElement('td');td.textContent=x;tr.appendChild(td)});t.appendChild(tr)});
$('core').textContent=s.core;$('rs').textContent=s.rs;$('pk').textContent=s.pk;$('nb').textContent=s.nb||'(nessuno)';
REP=s.rep!=='off';$('b_rep').textContent=REP?'Disattiva ripetizione':'Attiva ripetizione';WEN=s.wen!==false;$('b_wen').textContent=WEN?'Disattiva Wi-Fi':'Attiva Wi-Fi';$('hd').textContent=s.name+' \u00b7 MeshCore Repeater';document.title=s.name;$('rst').textContent='Se cambi rete o sbagli, tieni premuto il pulsante '+s.btn+' per 5 secondi: il nodo cancella il Wi-Fi e crea la rete "'+s.apname+'" (pagina su 192.168.4.1). Se la rete salvata non risponde per 45 secondi, la crea da solo.';
const C=jp(s.core),Rr=jp(s.rs),P=jp(s.pk);cards(C,Rr,P,s.up);sample(Rr,P);charts();
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
  field(o, "rep", "get repeat", 1);
  o += wifi_on ? "\"wen\":true," : "\"wen\":false,";
#ifdef G3WEB_ETH_PROBE
  o += "\"eth\":{\"vid\":"; o += (uint32_t)eth_vid;
  o += ",\"ready\":"; o += eth_ready ? "true" : "false";
  o += ",\"link\":"; o += eth_link ? "true" : "false";
  o += ",\"spd\":"; o += (eth_nsr & 0x80) ? 10 : 100;
  o += ",\"fdx\":"; o += (eth_ncr & 0x08) ? "true" : "false";
  o += ",\"ip\":"; jesc(o, eth_ready ? ip4addr_ntoa(netif_ip4_addr(&eth_if)) : "");
  o += ",\"gw\":"; jesc(o, eth_ready ? ip4addr_ntoa(netif_ip4_gw(&eth_if)) : "");
  o += ",\"rx\":"; o += eth_rx;
  o += ",\"tx\":"; o += eth_tx;
  o += ",\"err\":"; o += (uint32_t)(eth_rxerr + eth_txerr);
  o += "},";
#endif
  o += "\"apname\":"; jesc(o, G3WEB_AP_NAME); o += ",\"btn\":"; jesc(o, G3WEB_BTN_NAME); o += ',';
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

// Cancella il Wi-Fi salvato e riavvia: al prossimo avvio parte la rete di setup (G3WEB_AP_NAME).
// Accende o spegne il Wi-Fi senza riavviare il nodo (l'orologio e il ripetitore non si fermano).
void applyWifi(bool on) {
  if (!on) {
    wifi_on = false;
    if (mdns_on) { MDNS.end(); mdns_on = false; }
    if (ap_on) { WiFi.softAPdisconnect(true); ap_on = false; }
    WiFi.disconnect(true, false);
    WiFi.mode(WIFI_OFF);
    return;
  }
  wifi_on = true;
  last_ok = millis();
  last_try = last_ok;
  if (sta_cfg) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(G3WEB_HOSTNAME);
    WiFi.setSleep(false);
    WiFi.begin(saved_ssid, saved_pwd);
  } else {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(G3WEB_AP_NAME);
    ap_on = true;
  }
}

void handleWifiEn() {
  if (!authed() || !csrfOk()) return;
  String v = web.arg("en");
  if (v.length() != 1 || (v[0] != '0' && v[0] != '1')) { web.send(400, "text/plain", "Valore non valido"); return; }
  g3prefs.begin("g3web", false);
  g3prefs.putUChar("wen", v[0] == '1' ? 1 : 0);
  g3prefs.end();
  sendReply(v[0] == '1' ? "Wi-Fi in accensione..." : "Wi-Fi in spegnimento: questa pagina smettera' di rispondere se la stai usando dal Wi-Fi");
  wifi_pending = (v[0] == '1') ? 1 : 0;
  wifi_pending_at = millis() + 400;   // lascia il tempo di inviare la risposta
}
void resetWifi() {
  g3prefs.begin("g3web", false);
  g3prefs.remove("ssid");
  g3prefs.remove("pwd");
  g3prefs.putUChar("wen", 1);          // il reset riattiva anche il Wi-Fi
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
  wifi_on = g3prefs.getUChar("wen", 1) != 0;
  g3prefs.end();
  strncpy(saved_ssid, s.c_str(), sizeof(saved_ssid) - 1);
  strncpy(saved_pwd, p.c_str(), sizeof(saved_pwd) - 1);
  sta_cfg = saved_ssid[0] != 0;

  WiFi.persistent(false);              // le credenziali stanno in Preferences, non nella config del Wi-Fi
  WiFi.setAutoReconnect(true);
  if (!wifi_on) {
    WiFi.mode(WIFI_OFF);               // Wi-Fi disattivato dall'utente: resta solo la rete cablata
  } else if (sta_cfg) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(G3WEB_HOSTNAME);
    WiFi.setSleep(false);              // niente risparmio energetico del modem: collegamento piu' stabile
    WiFi.begin(saved_ssid, saved_pwd);
    last_try = millis();
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
  web.on("/api/wifien", HTTP_POST, handleWifiEn);
  web.onNotFound(handleNotFound);
  static const char* keys[] = {"X-G3"};
  web.collectHeaders(keys, 1);
  web.begin();
#ifdef G3WEB_ETH_PROBE
  ethStart();
#endif
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
  if (wifi_pending >= 0 && (int32_t)(now - wifi_pending_at) >= 0) {
    applyWifi(wifi_pending == 1);
    wifi_pending = -1;
  }
  if (now - last_check < 2000) return;
  last_check = now;
  if (!wifi_on) return;                // Wi-Fi spento: niente controlli di rete

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
  } else {
    if (sta_cfg && now - last_try > 20000) {   // non collegato: rilancia la connessione ogni 20 s
      last_try = now;
      WiFi.disconnect(false);
      WiFi.begin(saved_ssid, saved_pwd);
    }
    if (sta_cfg && !ap_on && now - last_ok > 45000
#ifdef G3WEB_ETH_PROBE
        && !ethHasIp()                  // con la rete cablata attiva non serve il Wi-Fi di setup
#endif
        ) {
      WiFi.mode(WIFI_AP_STA);          // rete irraggiungibile: riapri il setup, continua a riprovare
      WiFi.softAP(G3WEB_AP_NAME);
      ap_on = true;
    }
  }
}
