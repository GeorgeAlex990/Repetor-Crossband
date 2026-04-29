/*
 * Repetor Crossband VHF-UHF
 *
 * Pinout:
 *   SA818-V  UART TX -> GPIO16 | UART RX <- GPIO17 | PTT -> GPIO26 | SQ <- GPIO27
 *   SA818-U  UART TX -> GPIO14 | UART RX <- GPIO12 | PTT -> GPIO13 | SQ <- GPIO33
 *   Ton ID   DAC             -> GPIO25
 *   Baterie  ADC1_CH6        <- GPIO34
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <HardwareSerial.h>

// ─── Pinout ───────────────────────────────────────────────────────────────────
#define VHF_UART_TX  17
#define VHF_UART_RX  16
#define VHF_PTT      26   // HIGH = emite, LOW = receptie
#define VHF_SQ       27   // HIGH = squelch deschis (semnal prezent)

#define UHF_UART_TX  14
#define UHF_UART_RX  32 
#define UHF_PTT      13
#define UHF_SQ       33

#define TONE_PIN     25   // GPIO digital: HIGH/LOW 1kHz pentru ton Morse
#define BAT_ADC      34   // ADC1 -> divizor rezistiv baterie

// Divizor rezistiv baterie: R17=100k (spre baterie), R18=220k (spre GND)
// Vbat = Vadc * (100+220)/220 = Vadc * 1.4545
#define DIVIDER_RATIO  1.4545f

// ─── Structuri de date ────────────────────────────────────────────────────────
struct RadioCfg {
    char txFreq[10];   // "145.5000"
    char rxFreq[10];
    uint8_t bw;        // 0=12.5kHz, 1=25kHz
    uint8_t sq;        // 0-8
    uint8_t vol;       // 0-8
    char ctcss[5];     // "0000"
};

struct ChanStat {
    bool     rxActive = false;
    bool     txActive = false;
    uint32_t rxCount  = 0;
    uint32_t txCount  = 0;
};

// ─── Date globale ─────────────────────────────────────────────────────────────
RadioCfg vhfCfg = {"145.5000","145.5000", 0, 4, 8, "0000"};
RadioCfg uhfCfg = {"430.5000","430.5000", 0, 4, 8, "0000"};
uint32_t maxTxSec = 180;
uint32_t idIntSec = 600;
char     callsign[16] = "YO2GAR";

ChanStat vhfSt, uhfSt;

enum class State { IDLE, VHF_TO_UHF, UHF_TO_VHF };
State    repState  = State::IDLE;
uint32_t txStartMs = 0;
uint32_t lastIdMs  = 0;
float    batVolt   = 0.0f;
uint32_t bootMs    = 0;

HardwareSerial vhfUart(1);   // UART1
HardwareSerial uhfUart(2);   // UART2
Preferences    prefs;
WebServer      srv(80);

const char* WIFI_SSID = "CrossbandRepeater";
const char* WIFI_PASS = "repeater1234";

// ─── SA818 – comenzi AT ───────────────────────────────────────────────────────
static bool sa818Send(HardwareSerial& uart, const char* cmd,
                      char* respBuf, size_t respBufLen, uint32_t tmo = 700) {
    while (uart.available()) uart.read();   // goleste buffer
    uart.println(cmd);
    uint32_t t0  = millis();
    size_t   pos = 0;
    respBuf[0]   = '\0';
    while (millis() - t0 < tmo) {
        while (uart.available() && pos < respBufLen - 1)
            respBuf[pos++] = (char)uart.read();
        respBuf[pos] = '\0';
        if (strstr(respBuf, "\r\n")) break;
        delay(5);
    }
    // trim CR/LF
    while (pos > 0 && (respBuf[pos-1] == '\r' || respBuf[pos-1] == '\n'))
        respBuf[--pos] = '\0';
    Serial.printf("  AT>> %s | << %s\n", cmd, respBuf);
    return true;
}

static bool sa818Config(HardwareSerial& uart, const RadioCfg& c) {
    char cmd[80];
    char resp[64];
    // AT+DMOSETGROUP=BW,TxFreq,RxFreq,TxCTCSS,SQ,RxCTCSS
    snprintf(cmd, sizeof(cmd), "AT+DMOSETGROUP=%d,%s,%s,%s,%d,%s",
             c.bw, c.txFreq, c.rxFreq, c.ctcss, (int)c.sq, c.ctcss);
    sa818Send(uart, cmd, resp, sizeof(resp), 1200);
    if (!strstr(resp, "+DMOSETGROUP:0")) return false;
    snprintf(cmd, sizeof(cmd), "AT+DMOSETVOLUME=%d", (int)c.vol);
    sa818Send(uart, cmd, resp, sizeof(resp));
    return true;
}

static void sa818Init(HardwareSerial& uart, const RadioCfg& c, const char* tag) {
    char resp[64];
    Serial.printf("[%s] Initializare SA818...\n", tag);
    for (int i = 0; i < 3; i++) {
        sa818Send(uart, "AT+DMOCONNECT", resp, sizeof(resp), 800);
        if (strstr(resp, "+DMOCONNECT:0")) {
            Serial.printf("[%s] Conectat OK\n", tag); break;
        }
        delay(300);
    }
    Serial.printf("[%s] Config: %s\n", tag,
                  sa818Config(uart, c) ? "OK" : "ESUAT");
}

// ─── PTT / Squelch ────────────────────────────────────────────────────────────
inline void setPTT(int pin, bool tx) { digitalWrite(pin, tx ? HIGH : LOW); }
inline bool sqOpen(int pin)          { return digitalRead(pin) == HIGH; }

// ─── Baterie ──────────────────────────────────────────────────────────────────
static float readBat() {
    uint32_t s = 0;
    for (int i = 0; i < 16; i++) { s += analogRead(BAT_ADC); delay(1); }
    float vadc = (s / 16.0f) * 3.3f / 4095.0f;
    return vadc * DIVIDER_RATIO;
}

static int batPct(float v) {
    // Pb-acid / LiPo pack: 9V=0%, 12.6V=100%
    int p = (int)((v - 9.0f) / 3.6f * 100.0f);
    return p < 0 ? 0 : p > 100 ? 100 : p;
}

static void playTone(uint32_t durationMs);  // forward declaration

// ─── Morse code ───────────────────────────────────────────────────────────────
// Index 0-25 = A-Z, 26-35 = 0-9
// Morse ITU: indice 0=A...25=Z, 26=0...35=9
// dit='.', dat='-'
static const char MORSE_A[]  = ".-";
static const char MORSE_B[]  = "-...";
static const char MORSE_C[]  = "-.-.";
static const char MORSE_D[]  = "-..";
static const char MORSE_E[]  = ".";
static const char MORSE_F[]  = "..-.";
static const char MORSE_G[]  = "--.";
static const char MORSE_H[]  = "....";
static const char MORSE_I[]  = "..";
static const char MORSE_J[]  = ".---";
static const char MORSE_K[]  = "-.-";
static const char MORSE_L[]  = ".-..";
static const char MORSE_M[]  = "--";
static const char MORSE_N[]  = "-.";
static const char MORSE_O[]  = "---";
static const char MORSE_P[]  = ".--.";
static const char MORSE_Q[]  = "--.-";
static const char MORSE_R[]  = ".-.";
static const char MORSE_S[]  = "...";
static const char MORSE_T[]  = "-";
static const char MORSE_U[]  = "..-";
static const char MORSE_V[]  = "...-";
static const char MORSE_W[]  = ".--";
static const char MORSE_X[]  = "-..-";
static const char MORSE_Y[]  = "-.--";
static const char MORSE_Z[]  = "--..";
static const char MORSE_0[]  = "-----";
static const char MORSE_1[]  = ".----";
static const char MORSE_2[]  = "..---";
static const char MORSE_3[]  = "...--";
static const char MORSE_4[]  = "....-";
static const char MORSE_5[]  = ".....";
static const char MORSE_6[]  = "-....";
static const char MORSE_7[]  = "--...";
static const char MORSE_8[]  = "---..";
static const char MORSE_9[]  = "----.";

static const char* const MORSE_TABLE[] = {
    MORSE_A,MORSE_B,MORSE_C,MORSE_D,MORSE_E,MORSE_F,MORSE_G,MORSE_H,MORSE_I,MORSE_J,
    MORSE_K,MORSE_L,MORSE_M,MORSE_N,MORSE_O,MORSE_P,MORSE_Q,MORSE_R,MORSE_S,MORSE_T,
    MORSE_U,MORSE_V,MORSE_W,MORSE_X,MORSE_Y,MORSE_Z,
    MORSE_0,MORSE_1,MORSE_2,MORSE_3,MORSE_4,MORSE_5,MORSE_6,MORSE_7,MORSE_8,MORSE_9
};

// Modulare audio Morse pe pin digital (1kHz) pentru identificare vocala in timpul emisiunii.
// intarzierea SA818 de ~1s la fiecare tranzitie PTT.
static void playAudioMorse(const char* text, uint32_t unitMs = 80) {
    bool hadLetter = false;
    for (int i = 0; text[i]; i++) {
        char c = (char)toupper((unsigned char)text[i]);
        const char* pat = nullptr;
        if (c >= 'A' && c <= 'Z')      pat = MORSE_TABLE[c - 'A'];
        else if (c >= '0' && c <= '9') pat = MORSE_TABLE[26 + c - '0'];
        else if (c == ' ') {
            hadLetter = false;
            digitalWrite(TONE_PIN, LOW);
            delay(unitMs * 7);
            continue;
        }
        if (!pat) continue;
        if (hadLetter) { digitalWrite(TONE_PIN, LOW); delay(unitMs * 3); } // inter-litera
        hadLetter = true;
        for (int j = 0; pat[j]; j++) {
            if (j > 0) { digitalWrite(TONE_PIN, LOW); delay(unitMs); }    // intra-caracter
            playTone(pat[j] == '.' ? unitMs : unitMs * 3);
        }
    }
    digitalWrite(TONE_PIN, LOW); // repaus DAC dupa secventa
}

// ID periodic: ambele PTT ON → asteptam SA818 → Morse audio → PTT OFF
static void playMorseID(const char* text) {
    setPTT(VHF_PTT, true);
    setPTT(UHF_PTT, true);
    delay(1200);           // asteptam stabilizarea portantei SA818
    playAudioMorse(text);
    setPTT(VHF_PTT, false);
    setPTT(UHF_PTT, false);
}

// 'K' dupa relay: PTT este DEJA activ, pastram purtatoarea, asteptam 1s, modulam, eliberam.
static void playMorseOnPTT(int pttPin, const char* text) {
    delay(1000);
    playAudioMorse(text);
    setPTT(pttPin, false);
}

// ─── Ton identificare 1kHz prin GPIO digital ─────────────────────────────────
// Pin 25 comuta HIGH/LOW la 1kHz in timpul elementelor Morse.
// Intre elemente pinul ramane LOW (0V = silenta completa).
static void playTone(uint32_t durationMs) {
    uint32_t end = millis() + durationMs;
    uint32_t yieldAt = millis() + 10;
    while (millis() < end) {
        digitalWrite(TONE_PIN, HIGH); delayMicroseconds(500);
        digitalWrite(TONE_PIN, LOW);  delayMicroseconds(500);
        if (millis() >= yieldAt) { yield(); yieldAt = millis() + 10; }
    }
    digitalWrite(TONE_PIN, LOW);  // silenta = 0V
}

// ─── NVS – persistenta configuratie ──────────────────────────────────────────
static void nvsLoad() {
    prefs.begin("rep", false);   // false = RW, creeaza namespace la primul boot
    { String s = prefs.getString("v_txf","145.5000"); strlcpy(vhfCfg.txFreq, s.c_str(), 10); }
    { String s = prefs.getString("v_rxf","145.5000"); strlcpy(vhfCfg.rxFreq, s.c_str(), 10); }
    vhfCfg.bw  = prefs.getUChar("v_bw", 0);
    vhfCfg.sq  = prefs.getUChar("v_sq", 4);
    vhfCfg.vol = prefs.getUChar("v_vol",8);
    { String s = prefs.getString("v_ct","0000"); strlcpy(vhfCfg.ctcss, s.c_str(), 5); }

    { String s = prefs.getString("u_txf","430.5000"); strlcpy(uhfCfg.txFreq, s.c_str(), 10); }
    { String s = prefs.getString("u_rxf","430.5000"); strlcpy(uhfCfg.rxFreq, s.c_str(), 10); }
    uhfCfg.bw  = prefs.getUChar("u_bw", 0);
    uhfCfg.sq  = prefs.getUChar("u_sq", 4);
    uhfCfg.vol = prefs.getUChar("u_vol",8);
    { String s = prefs.getString("u_ct","0000"); strlcpy(uhfCfg.ctcss, s.c_str(), 5); }

    maxTxSec = prefs.getUInt("maxtx", 180);
    idIntSec = prefs.getUInt("idint", 600);
    { String s = prefs.getString("call","YO8XXX"); strlcpy(callsign, s.c_str(), 16); }
    prefs.end();
}

static void nvsSaveVHF() {
    prefs.begin("rep", false);
    prefs.putString("v_txf", vhfCfg.txFreq); prefs.putString("v_rxf", vhfCfg.rxFreq);
    prefs.putUChar("v_bw",  vhfCfg.bw);      prefs.putUChar("v_sq",  vhfCfg.sq);
    prefs.putUChar("v_vol", vhfCfg.vol);      prefs.putString("v_ct", vhfCfg.ctcss);
    prefs.end();
}

static void nvsSaveUHF() {
    prefs.begin("rep", false);
    prefs.putString("u_txf", uhfCfg.txFreq); prefs.putString("u_rxf", uhfCfg.rxFreq);
    prefs.putUChar("u_bw",  uhfCfg.bw);      prefs.putUChar("u_sq",  uhfCfg.sq);
    prefs.putUChar("u_vol", uhfCfg.vol);      prefs.putString("u_ct", uhfCfg.ctcss);
    prefs.end();
}

static void nvsSaveRep() {
    prefs.begin("rep", false);
    prefs.putUInt("maxtx", maxTxSec);
    prefs.putUInt("idint", idIntSec);
    prefs.putString("call", callsign);
    prefs.end();
}

// ─── JSON helpers ─────────────────────────────────────────────────────────────
// Extrage valoare string din JSON minimal {"key":"val"}
static String jStr(const String& json, const char* key) {
    String k = String("\"") + key + "\"";
    int ki = json.indexOf(k);
    if (ki < 0) return "";
    int ci = json.indexOf(':', ki + k.length());
    if (ci < 0) return "";
    int q1 = json.indexOf('"', ci + 1);
    if (q1 < 0) return "";
    int q2 = json.indexOf('"', q1 + 1);
    return json.substring(q1 + 1, q2);
}

// Extrage valoare numerica din JSON minimal
static int jInt(const String& json, const char* key) {
    String k = String("\"") + key + "\"";
    int ki = json.indexOf(k);
    if (ki < 0) return 0;
    int ci = json.indexOf(':', ki + k.length());
    if (ci < 0) return 0;
    return json.substring(ci + 1).toInt();
}

// ─── Pagina web (HTML/CSS/JS) ─────────────────────────────────────────────────
static const char HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="ro">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Repetor Crossband</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:'Segoe UI',sans-serif;background:linear-gradient(135deg,#4a56a6,#764ba2);min-height:100vh;padding:18px}
.wrap{max-width:980px;margin:auto}
h1{color:#fff;text-align:center;font-size:1.7rem;margin-bottom:4px}
.sub{color:#ccc;text-align:center;font-size:.82rem;margin-bottom:18px}
.grid3{display:grid;grid-template-columns:repeat(3,1fr);gap:14px;margin-bottom:18px}
.card{background:#1e2260;border-radius:10px;padding:14px;color:#fff}
.card h2{font-size:.82rem;color:#a0aec0;text-transform:uppercase;letter-spacing:.06em;
         margin-bottom:10px;border-bottom:1px solid #3a3a7a;padding-bottom:5px}
.row{display:flex;justify-content:space-between;margin:3px 0;font-size:.83rem}
.val{color:#90cdf4;font-weight:600}.act{color:#68d391!important}
.bat-bar{background:#3a3a7a;border-radius:4px;height:7px;margin-top:7px;overflow:hidden}
.bat-fill{background:#68d391;height:100%;border-radius:4px;transition:width .5s}
.bat-lbl{text-align:center;font-size:.73rem;color:#a0aec0;margin-top:3px}
.sec{background:#fff;border-radius:10px;padding:18px;margin-bottom:16px;border-left:4px solid #667eea}
.sec h3{font-size:.95rem;color:#2d2d5e;margin-bottom:14px;padding-bottom:7px;border-bottom:1px solid #e2e8f0}
.fr2{display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-bottom:12px}
label{font-size:.75rem;color:#718096;text-transform:uppercase;letter-spacing:.04em;display:block;margin-bottom:3px}
input,select{width:100%;padding:7px 9px;border:1px solid #e2e8f0;border-radius:6px;font-size:.88rem;outline:none;transition:border .2s}
input:focus,select:focus{border-color:#667eea}
.br{display:grid;grid-template-columns:1fr 1fr;gap:9px;margin-top:3px}
button{padding:9px;border:none;border-radius:6px;cursor:pointer;font-weight:700;font-size:.82rem;transition:opacity .2s}
button:hover{opacity:.82}
.bs{background:#667eea;color:#fff}.br2{background:#718096;color:#fff}
.full{grid-column:1/-1}
@media(max-width:620px){.grid3{grid-template-columns:1fr}.fr2{grid-template-columns:1fr}}
</style>
</head>
<body>
<div class="wrap">
  <h1>Cross-Band Repeater Control</h1>
  <p class="sub">VHF (SA818-V) &harr; UHF (SA818-U) &nbsp;|&nbsp; George Alexandru Reuca</p>

  <div class="grid3">
    <div class="card">
      <h2>VHF Status (144-146 MHz)</h2>
      <div class="row"><span>RX Status:</span><span class="val" id="v-rx">Idle</span></div>
      <div class="row"><span>TX Status:</span><span class="val" id="v-tx">Idle</span></div>
      <div class="row"><span>RX Count:</span><span class="val" id="v-rxc">0</span></div>
      <div class="row"><span>TX Count:</span><span class="val" id="v-txc">0</span></div>
    </div>
    <div class="card">
      <h2>UHF Status (430-440 MHz)</h2>
      <div class="row"><span>RX Status:</span><span class="val" id="u-rx">Idle</span></div>
      <div class="row"><span>TX Status:</span><span class="val" id="u-tx">Idle</span></div>
      <div class="row"><span>RX Count:</span><span class="val" id="u-rxc">0</span></div>
      <div class="row"><span>TX Count:</span><span class="val" id="u-txc">0</span></div>
    </div>
    <div class="card">
      <h2>System Status</h2>
      <div class="row"><span>Uptime:</span><span class="val" id="uptime">0h 0m</span></div>
      <div class="row"><span>Battery:</span><span class="val" id="bat-v">0.00 V</span></div>
      <div class="bat-bar"><div class="bat-fill" id="bat-f" style="width:0%"></div></div>
      <div class="bat-lbl" id="bat-p">0%</div>
    </div>
  </div>

  <div class="sec">
    <h3>VHF Configuration (SA818-V)</h3>
    <div class="fr2">
      <div><label>TX Frequency (MHz)</label><input id="v-txf" type="text"></div>
      <div><label>RX Frequency (MHz)</label><input id="v-rxf" type="text"></div>
    </div>
    <div class="fr2">
      <div><label>Bandwidth</label>
        <select id="v-bw"><option value="0">12.5 kHz</option><option value="1">25 kHz</option></select></div>
      <div><label>Volume (0-8)</label><input id="v-vol" type="number" min="0" max="8"></div>
    </div>
    <div class="fr2">
      <div><label>Squelch (0-8)</label><input id="v-sq" type="number" min="0" max="8"></div>
      <div><label>Tone CTCSS</label><input id="v-ct" type="text" maxlength="4" placeholder="0000"></div>
    </div>
    <div class="br">
      <button class="bs" onclick="saveVHF()">SAVE VHF</button>
      <button class="br2" onclick="resetVHF()">RESET</button>
    </div>
  </div>

  <div class="sec">
    <h3>UHF Configuration (SA818-U)</h3>
    <div class="fr2">
      <div><label>TX Frequency (MHz)</label><input id="u-txf" type="text"></div>
      <div><label>RX Frequency (MHz)</label><input id="u-rxf" type="text"></div>
    </div>
    <div class="fr2">
      <div><label>Bandwidth</label>
        <select id="u-bw"><option value="0">12.5 kHz</option><option value="1">25 kHz</option></select></div>
      <div><label>Volume (0-8)</label><input id="u-vol" type="number" min="0" max="8"></div>
    </div>
    <div class="fr2">
      <div><label>Squelch (0-8)</label><input id="u-sq" type="number" min="0" max="8"></div>
      <div><label>Tone CTCSS</label><input id="u-ct" type="text" maxlength="4" placeholder="0000"></div>
    </div>
    <div class="br">
      <button class="bs" onclick="saveUHF()">SAVE UHF</button>
      <button class="br2" onclick="resetUHF()">RESET</button>
    </div>
  </div>

  <div class="sec">
    <h3>Repeater Settings</h3>
    <div class="fr2">
      <div><label>Max TX Duration (secunde)</label><input id="maxtx" type="number" min="10" max="600"></div>
      <div><label>Interval ID Morse (secunde)</label><input id="idint" type="number" min="60" max="3600"></div>
    </div>
    <div class="fr2">
      <div class="full"><label>Identificator Repetor (Callsign)</label><input id="callsign" type="text" maxlength="10" placeholder="YO8XXX" style="text-transform:uppercase"></div>
    </div>
    <div class="br">
      <button class="bs full" onclick="saveRep()">SAVE REPEATER SETTINGS</button>
    </div>
  </div>
</div>

<script>
const $ = id => document.getElementById(id);
const setVal = (id, txt, active) => {
  const e = $(id); e.textContent = txt;
  e.className = 'val' + (active ? ' act' : '');
};
function applyConfig(c) {
  $('v-txf').value=c.v_txf; $('v-rxf').value=c.v_rxf;
  $('v-bw').value=c.v_bw;   $('v-vol').value=c.v_vol;
  $('v-sq').value=c.v_sq;   $('v-ct').value=c.v_ct;
  $('u-txf').value=c.u_txf; $('u-rxf').value=c.u_rxf;
  $('u-bw').value=c.u_bw;   $('u-vol').value=c.u_vol;
  $('u-sq').value=c.u_sq;   $('u-ct').value=c.u_ct;
  $('maxtx').value=c.maxtx; $('idint').value=c.idint;
  $('callsign').value=c.callsign||'';
}
function updateStatus(s) {
  setVal('v-rx', s.v_rx?'Activ':'Idle', s.v_rx);
  setVal('v-tx', s.v_tx?'Emite':'Idle', s.v_tx);
  $('v-rxc').textContent=s.v_rxc; $('v-txc').textContent=s.v_txc;
  setVal('u-rx', s.u_rx?'Activ':'Idle', s.u_rx);
  setVal('u-tx', s.u_tx?'Emite':'Idle', s.u_tx);
  $('u-rxc').textContent=s.u_rxc; $('u-txc').textContent=s.u_txc;
  const h=Math.floor(s.uptime/3600), m=Math.floor((s.uptime%3600)/60);
  $('uptime').textContent=h+'h '+m+'m';
  $('bat-v').textContent=s.bat_v.toFixed(2)+' V';
  $('bat-f').style.width=s.bat_p+'%';
  $('bat-p').textContent=s.bat_p+'%';
}
const post = (url, data) =>
  fetch(url,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(data)}).then(r=>r.json());

async function saveVHF() {
  const r=await post('/api/vhf',{txf:$('v-txf').value,rxf:$('v-rxf').value,
    bw:+$('v-bw').value,vol:+$('v-vol').value,sq:+$('v-sq').value,ct:$('v-ct').value});
  alert(r.ok?'VHF salvat si aplicat!':'Eroare: '+r.err);
}
async function resetVHF() {
  if(!confirm('Reset VHF la valorile implicite?'))return;
  const r=await post('/api/vhf/reset',{});
  if(r.ok){applyConfig(r.cfg);alert('VHF resetat.');}
}
async function saveUHF() {
  const r=await post('/api/uhf',{txf:$('u-txf').value,rxf:$('u-rxf').value,
    bw:+$('u-bw').value,vol:+$('u-vol').value,sq:+$('u-sq').value,ct:$('u-ct').value});
  alert(r.ok?'UHF salvat si aplicat!':'Eroare: '+r.err);
}
async function resetUHF() {
  if(!confirm('Reset UHF la valorile implicite?'))return;
  const r=await post('/api/uhf/reset',{});
  if(r.ok){applyConfig(r.cfg);alert('UHF resetat.');}
}
async function saveRep() {
  const r=await post('/api/rep',{maxtx:+$('maxtx').value,idint:+$('idint').value,
    callsign:$('callsign').value.toUpperCase()});
  alert(r.ok?'Setari repetor salvate!':'Eroare');
}
async function poll() {
  try { updateStatus(await fetch('/api/status').then(r=>r.json())); } catch(e){}
}
async function init() {
  try { applyConfig(await fetch('/api/config').then(r=>r.json())); } catch(e){}
}
init(); poll(); setInterval(poll, 2000);
</script>
</body>
</html>)HTML";

// ─── Handlere web ─────────────────────────────────────────────────────────────
static void sendConfigJson() {
    char buf[560];
    snprintf(buf, sizeof(buf),
        "{\"v_txf\":\"%s\",\"v_rxf\":\"%s\",\"v_bw\":%d,\"v_vol\":%d,\"v_sq\":%d,\"v_ct\":\"%s\","
        "\"u_txf\":\"%s\",\"u_rxf\":\"%s\",\"u_bw\":%d,\"u_vol\":%d,\"u_sq\":%d,\"u_ct\":\"%s\","
        "\"maxtx\":%u,\"idint\":%u,\"callsign\":\"%s\"}",
        vhfCfg.txFreq, vhfCfg.rxFreq, vhfCfg.bw, vhfCfg.vol, vhfCfg.sq, vhfCfg.ctcss,
        uhfCfg.txFreq, uhfCfg.rxFreq, uhfCfg.bw, uhfCfg.vol, uhfCfg.sq, uhfCfg.ctcss,
        maxTxSec, idIntSec, callsign);
    srv.send(200, "application/json", buf);
}

static void handleRoot()   { srv.send_P(200, "text/html", HTML); }
static void handleConfig() { sendConfigJson(); }

static void handleStatus() {
    uint32_t uptime = (millis() - bootMs) / 1000;
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"v_rx\":%d,\"v_tx\":%d,\"v_rxc\":%u,\"v_txc\":%u,"
        "\"u_rx\":%d,\"u_tx\":%d,\"u_rxc\":%u,\"u_txc\":%u,"
        "\"uptime\":%u,\"bat_v\":%.2f,\"bat_p\":%d}",
        vhfSt.rxActive, vhfSt.txActive, vhfSt.rxCount, vhfSt.txCount,
        uhfSt.rxActive, uhfSt.txActive, uhfSt.rxCount, uhfSt.txCount,
        uptime, batVolt, batPct(batVolt));
    srv.send(200, "application/json", buf);
}

static void handleSaveVHF() {
    String b = srv.arg("plain");
    strlcpy(vhfCfg.txFreq, jStr(b,"txf").c_str(), 10);
    strlcpy(vhfCfg.rxFreq, jStr(b,"rxf").c_str(), 10);
    vhfCfg.bw  = jInt(b,"bw");
    vhfCfg.vol = jInt(b,"vol");
    vhfCfg.sq  = jInt(b,"sq");
    strlcpy(vhfCfg.ctcss, jStr(b,"ct").c_str(), 5);
    bool ok = sa818Config(vhfUart, vhfCfg);
    if (ok) nvsSaveVHF();
    srv.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"SA818 nu raspunde\"}");
}

static void handleResetVHF() {
    vhfCfg = RadioCfg{"145.5000","145.5000", 0, 4, 8, "0000"};
    sa818Config(vhfUart, vhfCfg);
    nvsSaveVHF();
    // returneaza config curenta ca sa actualizeze UI-ul
    char buf[640];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cfg\":{\"v_txf\":\"%s\",\"v_rxf\":\"%s\",\"v_bw\":%d,\"v_vol\":%d,\"v_sq\":%d,\"v_ct\":\"%s\","
        "\"u_txf\":\"%s\",\"u_rxf\":\"%s\",\"u_bw\":%d,\"u_vol\":%d,\"u_sq\":%d,\"u_ct\":\"%s\","
        "\"maxtx\":%u,\"idint\":%u,\"callsign\":\"%s\"}}",
        vhfCfg.txFreq, vhfCfg.rxFreq, vhfCfg.bw, vhfCfg.vol, vhfCfg.sq, vhfCfg.ctcss,
        uhfCfg.txFreq, uhfCfg.rxFreq, uhfCfg.bw, uhfCfg.vol, uhfCfg.sq, uhfCfg.ctcss,
        maxTxSec, idIntSec, callsign);
    srv.send(200, "application/json", buf);
}

static void handleSaveUHF() {
    String b = srv.arg("plain");
    strlcpy(uhfCfg.txFreq, jStr(b,"txf").c_str(), 10);
    strlcpy(uhfCfg.rxFreq, jStr(b,"rxf").c_str(), 10);
    uhfCfg.bw  = jInt(b,"bw");
    uhfCfg.vol = jInt(b,"vol");
    uhfCfg.sq  = jInt(b,"sq");
    strlcpy(uhfCfg.ctcss, jStr(b,"ct").c_str(), 5);
    bool ok = sa818Config(uhfUart, uhfCfg);
    if (ok) nvsSaveUHF();
    srv.send(200, "application/json", ok ? "{\"ok\":true}" : "{\"ok\":false,\"err\":\"SA818 nu raspunde\"}");
}

static void handleResetUHF() {
    uhfCfg = RadioCfg{"430.5000","430.5000", 0, 4, 8, "0000"};
    sa818Config(uhfUart, uhfCfg);
    nvsSaveUHF();
    char buf[640];
    snprintf(buf, sizeof(buf),
        "{\"ok\":true,\"cfg\":{\"v_txf\":\"%s\",\"v_rxf\":\"%s\",\"v_bw\":%d,\"v_vol\":%d,\"v_sq\":%d,\"v_ct\":\"%s\","
        "\"u_txf\":\"%s\",\"u_rxf\":\"%s\",\"u_bw\":%d,\"u_vol\":%d,\"u_sq\":%d,\"u_ct\":\"%s\","
        "\"maxtx\":%u,\"idint\":%u,\"callsign\":\"%s\"}}",
        vhfCfg.txFreq, vhfCfg.rxFreq, vhfCfg.bw, vhfCfg.vol, vhfCfg.sq, vhfCfg.ctcss,
        uhfCfg.txFreq, uhfCfg.rxFreq, uhfCfg.bw, uhfCfg.vol, uhfCfg.sq, uhfCfg.ctcss,
        maxTxSec, idIntSec, callsign);
    srv.send(200, "application/json", buf);
}

static void handleSaveRep() {
    String b = srv.arg("plain");
    maxTxSec = jInt(b,"maxtx");
    idIntSec = jInt(b,"idint");
    String cs = jStr(b,"callsign");
    if (cs.length() > 0) {
        // Converteste la uppercase si pastram doar alfanumeric
        cs.toUpperCase();
        strlcpy(callsign, cs.c_str(), 16);
    }
    nvsSaveRep();
    srv.send(200, "application/json", "{\"ok\":true}");
}

// ─── Setup ────────────────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n=== Repetor Crossband VHF-UHF ===");

    // GPIO
    pinMode(VHF_PTT, OUTPUT); digitalWrite(VHF_PTT, LOW);   // LOW = receptie (idle)
    pinMode(UHF_PTT, OUTPUT); digitalWrite(UHF_PTT, LOW);
    pinMode(VHF_SQ, INPUT_PULLDOWN);   // HIGH = semnal prezent
    pinMode(UHF_SQ, INPUT_PULLDOWN);
    pinMode(TONE_PIN, OUTPUT); digitalWrite(TONE_PIN, LOW);  // ton Morse: digital HIGH/LOW
    analogSetAttenuation(ADC_11db);                  // ADC pana la ~3.6V

    // NVS
    nvsLoad();

    // UART SA818 (9600 baud implicit)
    vhfUart.begin(9600, SERIAL_8N1, VHF_UART_RX, VHF_UART_TX);
    uhfUart.begin(9600, SERIAL_8N1, UHF_UART_RX, UHF_UART_TX);
    delay(2000);   

    // Initializare module radio
    sa818Init(vhfUart, vhfCfg, "VHF");
    delay(500);
    sa818Init(uhfUart, uhfCfg, "UHF");

    // WiFi Access Point
    WiFi.mode(WIFI_AP);
    WiFi.softAP(WIFI_SSID, WIFI_PASS);
    Serial.printf("[WiFi] AP: %s  IP: %s\n",
                  WIFI_SSID, WiFi.softAPIP().toString().c_str());

    // Rute web
    srv.on("/",              HTTP_GET,  handleRoot);
    srv.on("/api/status",   HTTP_GET,  handleStatus);
    srv.on("/api/config",   HTTP_GET,  handleConfig);
    srv.on("/api/vhf",      HTTP_POST, handleSaveVHF);
    srv.on("/api/vhf/reset",HTTP_POST, handleResetVHF);
    srv.on("/api/uhf",      HTTP_POST, handleSaveUHF);
    srv.on("/api/uhf/reset",HTTP_POST, handleResetUHF);
    srv.on("/api/rep",      HTTP_POST, handleSaveRep);
    srv.begin();
    Serial.println("[Web] Server pornit pe portul 80");

    bootMs  = millis();
    lastIdMs = millis();
    Serial.println("[BOOT] Gata.");
}

// ─── Logica repetor ────────────────────────────────────────────────────────────
/*
 * Arhitectura audio: AF_OUT (SA818 receptor) → MIC_IN (SA818 emitator)
 * este cablu direct in hardware.
 * ESP32 gestioneaza DOAR pinii PTT si SQ.
 *
 * Stari:
 *   IDLE        – niciun semnal pe nicio banda
 *   VHF_TO_UHF  – VHF receptioneaza, UHF emite
 *   UHF_TO_VHF  – UHF receptioneaza, VHF emite
 */
static void repeaterTask() {
    bool vhfHasSig = sqOpen(VHF_SQ);
    bool uhfHasSig = sqOpen(UHF_SQ);

    switch (repState) {

    case State::IDLE:
        if (vhfHasSig) {
            repState = State::VHF_TO_UHF;
            txStartMs = millis();
            setPTT(UHF_PTT, true);
            uhfSt.txActive = true; uhfSt.txCount++;
            vhfSt.rxActive = true; vhfSt.rxCount++;
            Serial.println("[REP] VHF RX → UHF TX");
        } else if (uhfHasSig) {
            repState = State::UHF_TO_VHF;
            txStartMs = millis();
            setPTT(VHF_PTT, true);
            vhfSt.txActive = true; vhfSt.txCount++;
            uhfSt.rxActive = true; uhfSt.rxCount++;
            Serial.println("[REP] UHF RX → VHF TX");
        }
        break;

    case State::VHF_TO_UHF:
        if (!vhfHasSig) {
            // Semnal VHF disparut – trimite 'K' in Morse dupa 1 secunda, pe UHF
            Serial.println("[REP] VHF RX terminat → K Morse pe UHF → IDLE");
            //playMorseOnPTT(UHF_PTT, "K"); // PTT ramas activ, asteapta 1s, moduleaza, elibereaza
            playMorseID("K"); // Varianta cu ID Morse complet, ambele PTT ON, apoi OFF
            uhfSt.txActive = false;
            vhfSt.rxActive = false;
            repState = State::IDLE;
        } else if (millis() - txStartMs >= maxTxSec * 1000UL) {
            setPTT(UHF_PTT, false);
            uhfSt.txActive = false;
            vhfSt.rxActive = false;
            repState = State::IDLE;
            Serial.println("[REP] TIMEOUT UHF TX → IDLE");
        }
        break;

    case State::UHF_TO_VHF:
        if (!uhfHasSig) {
            // Semnal UHF disparut – trimite 'K' in Morse dupa 1 secunda, pe VHF
            Serial.println("[REP] UHF RX terminat → K Morse pe VHF → IDLE");
            //playMorseOnPTT(VHF_PTT, "K"); // PTT ramas activ, asteapta 1s, moduleaza, elibereaza
            playMorseID("K"); // Varianta cu ID Morse complet, ambele PTT ON, apoi OFF
            vhfSt.txActive = false;
            uhfSt.rxActive = false;
            repState = State::IDLE;
        } else if (millis() - txStartMs >= maxTxSec * 1000UL) {
            setPTT(VHF_PTT, false);
            vhfSt.txActive = false;
            uhfSt.rxActive = false;
            repState = State::IDLE;
            Serial.println("[REP] TIMEOUT VHF TX → IDLE");
        }
        break;
    }
}

// ─── Loop principal ────────────────────────────────────────────────────────────
void loop() {
    srv.handleClient();

    repeaterTask();

    // Citire baterie la fiecare 5 secunde
    static uint32_t lastBatMs = 0;
    if (millis() - lastBatMs >= 5000) {
        lastBatMs = millis();
        batVolt = readBat();
    }

    // Identificare Morse periodica (numai cand ambele canale sunt libere)
    if ((millis() - lastIdMs) >= idIntSec * 1000UL) {
        if (repState == State::IDLE) {
            lastIdMs = millis();
            Serial.printf("[ID] Identificare Morse: %s\n", callsign);
            playMorseID(callsign);
        }
    }
}
