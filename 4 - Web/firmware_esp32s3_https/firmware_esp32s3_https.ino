/*
  Aide au tri sélectif - firmware ESP32-S3 en HTTPS + authentification par jeton
  ------------------------------------------------------------------------------
  Version sécurisée (option 4). La carte :
    - crée son Wi-Fi (point d'accès),
    - sert TOUT en HTTPS (chiffré) avec un certificat auto-signé généré au boot,
    - demande une connexion : POST /login -> renvoie un jeton,
    - protège /capture et /detect : elles exigent le jeton (Authorization: Bearer).

  Routes :
    GET  /         -> la page (intégrée, avec écran de connexion)
    POST /login    -> body {"user":"...","pass":"..."} ; 200 {"token","ttl"} ou 401
    GET  /capture  -> JPEG   (401 sans jeton valide)
    GET  /detect   -> JSON   (401 sans jeton valide)

  --- DÉPENDANCE À INSTALLER ---
  Bibliothèque "esp32_https_server" (Frank Hessel) :
    Arduino IDE > Tools > Manage Libraries > chercher "esp32_https_server".
  Si la compilation échoue avec le core ESP32 3.x, utiliser le fork maintenu
  compatible 3.x (esp32_https_server_compat) - même API.

  --- À SAVOIR ---
  1) Le dossier de ce sketch doit s'appeler "firmware_esp32s3_https".
  2) Carte : XIAO_ESP32S3, PSRAM activée (OPI PSRAM).
  3) Le certificat étant AUTO-SIGNÉ, le navigateur affichera un avertissement
     de sécurité la première fois : c'est normal et attendu (voir la note pour
     le jury dans authentification.pdf). On clique sur "Continuer / Avancé".
  4) La génération du certificat au démarrage prend ~30-60 s (clé 2048 bits).
     Pour aller plus vite en dev, passer KEYSIZE_2048 -> KEYSIZE_1024 (moins sûr).
*/

#include "esp_camera.h"
#include <WiFi.h>

#include <HTTPSServer.hpp>
#include <SSLCert.hpp>
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>

using namespace httpsserver;

// 0 = caméra + web + auth seuls (détections vides) ; 1 = vraie inférence FOMO
#define USE_EDGE_IMPULSE 1
#if USE_EDGE_IMPULSE
  #include <Projet_t__inferencing.h>   // <-- remplacer par TON export Edge Impulse
  #include <edge-impulse-sdk/dsp/image/image.hpp>   // crop_and_interpolate_rgb888

  // Garde-fou : ce firmware lit result.bounding_boxes, qui n'existe que pour un
  // modèle de détection d'objets (FOMO). Un export de classification simple
  // compilerait mais ne renverrait jamais de boîte.
  #if EI_CLASSIFIER_OBJECT_DETECTION != 1
    #error "L'export Edge Impulse n'est pas un modele de detection d'objets (FOMO)."
  #endif
#endif

// ---------- Identifiants (démo) ----------
// En production : ne jamais stocker le mot de passe en clair -> stocker un
// hachage salé (ex. SHA-256 + sel) et comparer les empreintes. Ici, en clair
// pour la démo, à changer avant toute vraie utilisation.
const char *IDENT_USER = "admin";
const char *IDENT_PASS = "tri";

// ---------- Point d'accès Wi-Fi ----------
const char *AP_SSID = "TriSelectif";
const char *AP_PASS = "tri12345";       // 8 caractères minimum

// ---------- Jetons de session ----------
#define MAX_SESSIONS 4
const unsigned long TTL_MS = 15UL * 60UL * 1000UL;   // durée de vie d'un jeton : 15 min

struct Session {
  String token;
  unsigned long expire;   // instant (millis) d'expiration
};
Session sessions[MAX_SESSIONS];

// ---------- Serveur HTTPS ----------
SSLCert *cert = nullptr;
HTTPSServer *secureServer = nullptr;

// ---------- Brochage caméra : Seeed XIAO ESP32-S3 Sense ----------
#define PWDN_GPIO_NUM   -1
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM   10
#define SIOD_GPIO_NUM   40
#define SIOC_GPIO_NUM   39
#define Y9_GPIO_NUM     48
#define Y8_GPIO_NUM     11
#define Y7_GPIO_NUM     12
#define Y6_GPIO_NUM     14
#define Y5_GPIO_NUM     16
#define Y4_GPIO_NUM     18
#define Y3_GPIO_NUM     17
#define Y2_GPIO_NUM     15
#define VSYNC_GPIO_NUM  38
#define HREF_GPIO_NUM   47
#define PCLK_GPIO_NUM   13

// Résolution de capture. Doit rester cohérent avec config.frame_size ci-dessous
// ET avec CAM_W/CAM_H dans la page (calcul du recadrage pour placer les marqueurs).
const int CAM_W = 320, CAM_H = 240;   // FRAMESIZE_QVGA

bool initCamera() {
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM; config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM; config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM; config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM; config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM; config.pin_href = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM; config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM; config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.frame_size   = FRAMESIZE_QVGA;   // 320x240
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode    = CAMERA_GRAB_LATEST;
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.jpeg_quality = 12;
  config.fb_count     = 2;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) { Serial.printf("Echec init camera 0x%x\n", err); return false; }
  return true;
}

#if USE_EDGE_IMPULSE
// ---------- Inférence FOMO ----------
// Tampon RGB888 de travail, alloué une fois en PSRAM (320*240*3 = 230 400 o).
// Il sert d'abord à recevoir l'image décodée, puis reçoit sur place la version
// recadrée/redimensionnée à l'entrée du modèle.
static uint8_t *rgb_buf = nullptr;

// Callback réclamée par le SDK Edge Impulse : il lui donne un signal et attend
// des pixels un par un. `offset` et `length` sont en PIXELS, pas en octets ; la
// valeur rendue est le pixel empaqueté 0xRRGGBB converti en float.
static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr) {
  size_t pixel_ix = offset * 3;
  for (size_t i = 0; i < length; i++) {
    out_ptr[i] = (rgb_buf[pixel_ix] << 16)
               + (rgb_buf[pixel_ix + 1] << 8)
               +  rgb_buf[pixel_ix + 2];
    pixel_ix += 3;
  }
  return 0;
}
#endif

// ---------- Gestion des jetons ----------
String genererJeton() {
  char b[9]; String t;
  for (int i = 0; i < 4; i++) { sprintf(b, "%08x", (unsigned) esp_random()); t += b; }
  return t;  // 32 caractères hexa (128 bits d'aléa)
}

String creerSession() {
  String tok = genererJeton();
  unsigned long now = millis();
  int slot = -1;
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].token.length() == 0 || now > sessions[i].expire) { slot = i; break; }
  }
  if (slot < 0) slot = 0;   // toutes occupées : on écrase la première (simplifié)
  sessions[slot].token = tok;
  sessions[slot].expire = now + TTL_MS;
  return tok;
}

bool validerJeton(const String &tok) {
  unsigned long now = millis();   // NB: millis() déborde après ~49 j (OK pour la démo)
  for (int i = 0; i < MAX_SESSIONS; i++) {
    if (sessions[i].token.length() && sessions[i].token == tok && now <= sessions[i].expire)
      return true;
  }
  return false;
}

// ---------- Utilitaires requête/réponse ----------
String lireCorps(HTTPRequest *req) {
  String body;
  byte buf[257];
  while (!req->requestComplete()) {
    size_t n = req->readBytes(buf, 256);
    if (n == 0) break;              // sécurité anti-boucle
    buf[n] = 0;
    body += String((char *) buf);
  }
  return body;
}

// Extraction très simple d'une valeur texte dans un JSON plat (démo).
// Suppose {"cle":"valeur"} sans guillemets échappés dans la valeur.
String extraireJson(const String &json, const char *cle) {
  String motif = String("\"") + cle + "\"";
  int k = json.indexOf(motif);            if (k < 0) return "";
  int deux = json.indexOf(':', k + motif.length()); if (deux < 0) return "";
  int q1 = json.indexOf('"', deux);       if (q1 < 0) return "";
  int q2 = json.indexOf('"', q1 + 1);     if (q2 < 0) return "";
  return json.substring(q1 + 1, q2);
}

bool estAutorise(HTTPRequest *req) {
  std::string h = req->getHeader("Authorization");
  const std::string pre = "Bearer ";
  if (h.rfind(pre, 0) != 0) return false;                 // ne commence pas par "Bearer "
  String tok = String(h.substr(pre.size()).c_str());
  return validerJeton(tok);
}

void envoyer401(HTTPResponse *res) {
  res->setStatusCode(401);
  res->setHeader("Content-Type", "application/json");
  res->print("{\"error\":\"non autorise\"}");
}

// ---------- Page web intégrée (HTTPS, même origine, avec login) ----------
const char PAGE_HTML[] PROGMEM = R"HTMLPAGE(
<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Aide au tri sélectif</title>
<style>
  :root{--bg:#f4f6f8;--panel:#fff;--border:#d5dbe1;--text:#1b232b;--muted:#5a6773;--shadow:0 1px 3px rgba(0,0,0,.08)}
  *{box-sizing:border-box}
  body{margin:0;font-family:system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;background:var(--bg);color:var(--text);line-height:1.4}
  header{padding:14px 22px;background:var(--panel);border-bottom:1px solid var(--border);display:flex;align-items:center;justify-content:space-between}
  header h1{margin:0;font-size:18px}
  .logout{border:1px solid var(--border);background:var(--bg);border-radius:6px;padding:6px 12px;font-size:13px;cursor:pointer;color:var(--text)}
  main{display:flex;flex-wrap:wrap;gap:22px;padding:22px;align-items:flex-start}
  .viewer{flex:0 0 auto}
  canvas{display:block;background:#000;border:1px solid var(--border);border-radius:6px;box-shadow:var(--shadow)}
  .controls{margin-top:14px;padding:12px 14px;background:var(--panel);border:1px solid var(--border);border-radius:6px;box-shadow:var(--shadow);max-width:320px}
  .controls label{font-size:13px;color:var(--muted)}
  .controls input[type="range"]{width:100%}
  .controls .seuil-val{font-weight:600;color:var(--text)}
  .legend{display:flex;flex-wrap:wrap;gap:12px;margin-top:10px;font-size:13px}
  .legend span{display:inline-flex;align-items:center;gap:6px}
  .legend .dot{width:12px;height:12px;border-radius:3px;display:inline-block}
  .panel{flex:1 1 300px;min-width:280px;display:flex;flex-direction:column;gap:16px}
  .consigne{padding:22px;border-radius:8px;background:var(--panel);border:2px solid var(--border);box-shadow:var(--shadow);text-align:center}
  .consigne .titre{font-size:34px;font-weight:800;letter-spacing:.5px;line-height:1.15}
  .consigne .sous{font-size:16px;color:var(--muted);margin-top:6px}
  .card{background:var(--panel);border:1px solid var(--border);border-radius:8px;box-shadow:var(--shadow);padding:14px 16px}
  .card h2{margin:0 0 10px;font-size:14px;text-transform:uppercase;letter-spacing:.5px;color:var(--muted)}
  ul.detections{list-style:none;margin:0;padding:0}
  ul.detections li{display:flex;align-items:center;gap:10px;padding:6px 0;border-bottom:1px solid var(--border);font-size:15px}
  ul.detections li:last-child{border-bottom:none}
  ul.detections .dot{width:12px;height:12px;border-radius:3px}
  ul.detections .label{flex:1;font-weight:600}
  ul.detections .conf{color:var(--muted);font-variant-numeric:tabular-nums}
  .vide{color:var(--muted);font-style:italic}
  .metrics{display:grid;grid-template-columns:1fr 1fr;gap:10px}
  .metrics .m{background:var(--bg);border:1px solid var(--border);border-radius:6px;padding:8px 10px}
  .metrics .m .k{font-size:12px;color:var(--muted)}
  .metrics .m .v{font-size:20px;font-weight:700;font-variant-numeric:tabular-nums}
  .overlay{position:fixed;inset:0;background:rgba(20,26,32,.55);display:flex;align-items:center;justify-content:center;z-index:10}
  .login-box{background:var(--panel);padding:26px 28px;border-radius:10px;box-shadow:0 10px 30px rgba(0,0,0,.2);width:300px;display:flex;flex-direction:column;gap:12px}
  .login-box h2{margin:0;font-size:20px}
  .login-box .sub{margin:0 0 6px;font-size:13px;color:var(--muted)}
  .login-box label{display:flex;flex-direction:column;gap:4px;font-size:13px;color:var(--muted)}
  .login-box input{padding:8px 10px;border:1px solid var(--border);border-radius:6px;font-size:15px}
  .login-box button{margin-top:6px;padding:10px;border:none;border-radius:6px;background:#2f6fed;color:#fff;font-size:15px;font-weight:600;cursor:pointer}
  .login-box .err{margin:0;color:#c0392b;font-size:13px}
  [hidden]{display:none !important}
</style>
<div class="overlay" id="login-overlay">
  <form class="login-box" id="login-form">
    <h2>Connexion</h2>
    <p class="sub">Accès au flux de détection</p>
    <label>Identifiant<input id="in-user" autocomplete="username" required></label>
    <label>Mot de passe<input id="in-pass" type="password" autocomplete="current-password" required></label>
    <p class="err" id="login-err" hidden>Identifiants incorrects.</p>
    <button type="submit">Se connecter</button>
  </form>
</div>
<header>
  <h1>Aide au tri sélectif</h1>
  <button id="btn-logout" class="logout" hidden>Se déconnecter</button>
</header>
<main>
  <section class="viewer">
    <canvas id="cam" width="320" height="240"></canvas>
    <div class="controls">
      <label for="seuil">Seuil de confiance : <span class="seuil-val" id="seuil-val">0.60</span></label>
      <input type="range" id="seuil" min="0" max="1" step="0.01" value="0.6">
      <div class="legend" id="legend"></div>
    </div>
  </section>
  <section class="panel">
    <div class="consigne"><div class="titre" id="consigne-titre">-</div><div class="sous" id="consigne-sous"></div></div>
    <div class="card"><h2>Détections</h2><ul class="detections" id="liste"></ul></div>
    <div class="card"><h2>Performances</h2>
      <div class="metrics">
        <div class="m"><div class="k">Latence DSP</div><div class="v"><span id="m-dsp">-</span> ms</div></div>
        <div class="m"><div class="k">Inférence</div><div class="v"><span id="m-inf">-</span> ms</div></div>
        <div class="m"><div class="k">Débit</div><div class="v"><span id="m-fps">-</span> fps</div></div>
        <div class="m"><div class="k">Objets retenus</div><div class="v"><span id="m-count">-</span></div></div>
      </div>
    </div>
  </section>
</main>
<script>
const URL_LOGIN="/login", URL_IMAGE="/capture", URL_JSON="/detect";
const POLL_INTERVAL_MS=500;
const MODEL_W=160, MODEL_H=160, DISPLAY_W=320, DISPLAY_H=240;
const CAM_W=320, CAM_H=240;   // résolution de /capture : doit suivre CAM_W/CAM_H du firmware
const N_STABLE=3;
const CLASS_COLORS={bouteille:"#2f6fed",canette:"#e0781a",carton:"#2f9e44"};
const CONSIGNES={bouteille:{titre:"PLASTIQUE"},canette:{titre:"MÉTAL"},carton:{titre:"CARTON"}};

// Avant de l'analyser, le firmware recadre l'image au format du modèle (crop
// CENTRÉ, cf. crop_and_interpolate_rgb888). Les coordonnées reçues sont donc
// dans ce recadrage et non dans l'image entière : on refait le même calcul ici,
// sinon les marqueurs tombent à côté des objets. En 320x240 -> 96x96, le crop
// fait 240x240 et on rogne 40 px de chaque côté en largeur.
const RATIO=MODEL_W/MODEL_H;
let CROP_W=CAM_W, CROP_H=CAM_W/RATIO;
if(CROP_H>CAM_H){ CROP_H=CAM_H; CROP_W=CAM_H*RATIO; }
const SCALE_X=(CROP_W/MODEL_W)*(DISPLAY_W/CAM_W), SCALE_Y=(CROP_H/MODEL_H)*(DISPLAY_H/CAM_H);
const OFFSET_X=((CAM_W-CROP_W)/2)*(DISPLAY_W/CAM_W), OFFSET_Y=((CAM_H-CROP_H)/2)*(DISPLAY_H/CAM_H);
function modeleVersAffichage(x,y){return {x:x*SCALE_X+OFFSET_X, y:y*SCALE_Y+OFFSET_Y};}

const canvas=document.getElementById("cam"), ctx=canvas.getContext("2d");
let seuil=0.6, candidateClass=null, candidateCount=0, stableClass=null, token=null, tokenExpire=0, timer=null;
const CLASSES=Object.keys(CLASS_COLORS);

async function connexion(user,pass){
  const res=await fetch(URL_LOGIN,{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({user,pass})});
  if(!res.ok) return false;
  const data=await res.json(); token=data.token; tokenExpire=Date.now()+(data.ttl||0)*1000; return true;
}
function jetonExpire(){ return tokenExpire!==0 && Date.now()>=tokenExpire; }
async function authFetch(url,options={}){
  const opts=Object.assign({},options);
  opts.headers=Object.assign({},options.headers, token?{Authorization:"Bearer "+token}:{});
  const res=await fetch(url,opts);
  if(res.status===401){ deconnexion(); throw new Error("401"); }
  return res;
}
function deconnexion(message){
  token=null; tokenExpire=0; arreterBoucle();
  document.getElementById("btn-logout").hidden=true;
  const err=document.getElementById("login-err");
  if(message){ err.textContent=message; err.hidden=false; }
  document.getElementById("login-overlay").hidden=false;
}
async function chargerImage(){
  const res=await authFetch(URL_IMAGE+"?t="+Date.now());
  const blob=await res.blob(); const url=URL.createObjectURL(blob);
  try{ return await new Promise((ok,ko)=>{const im=new Image();im.onload=()=>ok(im);im.onerror=ko;im.src=url;}); }
  finally{ URL.revokeObjectURL(url); }
}
async function chargerDetections(){ const res=await authFetch(URL_JSON+"?t="+Date.now()); return res.json(); }

function majClasseStable(dom){
  if(dom===candidateClass) candidateCount++;
  else{ candidateClass=dom; candidateCount=1; }
  if(candidateCount>=N_STABLE) stableClass=candidateClass;
  return stableClass;
}
function classeDominante(dets){
  if(dets.length===0) return null;
  let best=dets[0]; for(const d of dets) if(d.conf>best.conf) best=d; return best.label;
}
function dessiner(image,dets){
  ctx.clearRect(0,0,DISPLAY_W,DISPLAY_H);
  ctx.drawImage(image,0,0,DISPLAY_W,DISPLAY_H);
  ctx.font="600 13px system-ui, sans-serif"; ctx.textBaseline="bottom";
  for(const d of dets){
    const couleur=CLASS_COLORS[d.label]||"#fff";
    const p=modeleVersAffichage(d.x,d.y), r=8;
    ctx.lineWidth=3; ctx.strokeStyle=couleur; ctx.strokeRect(p.x-r,p.y-r,r*2,r*2);
    ctx.fillStyle=couleur; ctx.beginPath(); ctx.arc(p.x,p.y,2.5,0,Math.PI*2); ctx.fill();
    const texte=d.label+" "+Math.round(d.conf*100)+"%";
    const w=ctx.measureText(texte).width+8, yT=p.y-r-4;
    ctx.fillStyle=couleur; ctx.fillRect(p.x-r,yT-16,w,16);
    ctx.fillStyle="#fff"; ctx.fillText(texte,p.x-r+4,yT);
  }
}
function majPanneau(dets,timing,fps){
  const liste=document.getElementById("liste"); liste.innerHTML="";
  if(dets.length===0){
    const li=document.createElement("li"); li.className="vide"; li.textContent="Aucun objet détecté"; liste.appendChild(li);
  } else {
    for(const d of dets){
      const li=document.createElement("li");
      const dot=document.createElement("span"); dot.className="dot"; dot.style.background=CLASS_COLORS[d.label]||"#999";
      const label=document.createElement("span"); label.className="label"; label.textContent=d.label;
      const conf=document.createElement("span"); conf.className="conf"; conf.textContent=Math.round(d.conf*100)+" %";
      li.append(dot,label,conf); liste.appendChild(li);
    }
  }
  const titreEl=document.getElementById("consigne-titre"), sousEl=document.getElementById("consigne-sous");
  if(stableClass && CONSIGNES[stableClass]){
    titreEl.textContent=CONSIGNES[stableClass].titre; sousEl.textContent=stableClass;
    titreEl.style.color=CLASS_COLORS[stableClass];
  } else {
    titreEl.textContent="Aucun objet détecté"; sousEl.textContent=""; titreEl.style.color="var(--muted)";
  }
  document.getElementById("m-dsp").textContent=timing.dsp;
  document.getElementById("m-inf").textContent=timing.classification;
  document.getElementById("m-fps").textContent=fps;
  document.getElementById("m-count").textContent=dets.length;
}
async function cycle(){
  if(jetonExpire()){ deconnexion("Session expirée, reconnecte-toi."); return; }
  try{
    const [image,data]=await Promise.all([chargerImage(),chargerDetections()]);
    const retenues=(data.detections||[]).filter(d=>d.conf>=seuil);
    majClasseStable(classeDominante(retenues));
    dessiner(image,retenues);
    majPanneau(retenues,data.timing||{dsp:0,classification:0},data.fps??0);
  }catch(e){ console.warn("cycle KO:",e); }
}
function demarrerBoucle(){ if(timer) return; cycle(); timer=setInterval(cycle,POLL_INTERVAL_MS); }
function arreterBoucle(){ if(timer) clearInterval(timer); timer=null; }

const slider=document.getElementById("seuil"), seuilVal=document.getElementById("seuil-val");
slider.addEventListener("input",()=>{seuil=parseFloat(slider.value);seuilVal.textContent=seuil.toFixed(2);});

const legend=document.getElementById("legend");
for(const cls of CLASSES){
  const span=document.createElement("span");
  const dot=document.createElement("span"); dot.className="dot"; dot.style.background=CLASS_COLORS[cls];
  span.append(dot,document.createTextNode(cls)); legend.appendChild(span);
}

const overlay=document.getElementById("login-overlay"), form=document.getElementById("login-form"), errEl=document.getElementById("login-err");
form.addEventListener("submit", async (e)=>{
  e.preventDefault(); errEl.hidden=true;
  const user=document.getElementById("in-user").value, pass=document.getElementById("in-pass").value;
  try{
    const ok=await connexion(user,pass);
    if(!ok){ errEl.textContent="Identifiants incorrects."; errEl.hidden=false; return; }
    overlay.hidden=true; document.getElementById("btn-logout").hidden=false; demarrerBoucle();
  }catch(err){ errEl.textContent="Serveur injoignable."; errEl.hidden=false; }
});
document.getElementById("btn-logout").addEventListener("click", deconnexion);
</script>
)HTMLPAGE";

// ---------- Handlers HTTPS ----------
void handleRoot(HTTPRequest *req, HTTPResponse *res) {
  res->setHeader("Content-Type", "text/html");
  res->print(PAGE_HTML);
}

void handleLogin(HTTPRequest *req, HTTPResponse *res) {
  String body = lireCorps(req);
  String user = extraireJson(body, "user");
  String pass = extraireJson(body, "pass");

  res->setHeader("Content-Type", "application/json");
  if (user == IDENT_USER && pass == IDENT_PASS) {
    String tok = creerSession();
    res->setStatusCode(200);
    res->print(String("{\"token\":\"") + tok + "\",\"ttl\":" + String(TTL_MS / 1000) + "}");
  } else {
    res->setStatusCode(401);
    res->print("{\"error\":\"identifiants invalides\"}");
  }
}

void handleCapture(HTTPRequest *req, HTTPResponse *res) {
  if (!estAutorise(req)) { envoyer401(res); return; }

  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb) { res->setStatusCode(500); res->print("capture failed"); return; }
  res->setHeader("Content-Type", "image/jpeg");
  res->setHeader("Cache-Control", "no-store");
  res->write(fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

void handleDetect(HTTPRequest *req, HTTPResponse *res) {
  if (!estAutorise(req)) { envoyer401(res); return; }

  String json = "{\"detections\":[";
  int dsp = 0, classification = 0;
  float fps = 0.0f;

#if USE_EDGE_IMPULSE
  // Chaîne complète : capture JPEG -> RGB888 -> recadrage/redimensionnement à
  // l'entrée du modèle -> run_classifier -> boîtes FOMO -> JSON.
  // NB : cette capture est distincte de celle de /capture (choix assumé) ;
  // l'image affichée et les détections viennent donc de deux trames voisines.
  camera_fb_t *fb = esp_camera_fb_get();
  if (fb && rgb_buf) {
    int src_w = fb->width, src_h = fb->height;
    bool ok = fmt2rgb888(fb->buf, fb->len, fb->format, rgb_buf);
    esp_camera_fb_return(fb);      // rendu tout de suite : la suite ne lit que rgb_buf

    // Recadrage centré au format du modèle puis mise à l'échelle, sur place
    // (la fonction accepte le même tampon en source et en destination).
    if (ok && (src_w != EI_CLASSIFIER_INPUT_WIDTH || src_h != EI_CLASSIFIER_INPUT_HEIGHT)) {
      ok = ei::image::processing::crop_and_interpolate_rgb888(
             rgb_buf, src_w, src_h,
             rgb_buf, EI_CLASSIFIER_INPUT_WIDTH, EI_CLASSIFIER_INPUT_HEIGHT) == EIDSP_OK;
    }

    if (ok) {
      ei::signal_t signal;
      signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
      signal.get_data = &ei_camera_get_data;

      ei_impulse_result_t result = { 0 };
      if (run_classifier(&signal, &result, false) == EI_IMPULSE_OK) {
        bool premier = true;
        for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
          ei_impulse_result_bounding_box_t bb = result.bounding_boxes[i];
          if (bb.value == 0) continue;      // FOMO remplit la grille : on saute les cases vides
          int cx = bb.x + bb.width / 2;     // centroïde, repère du modèle
          int cy = bb.y + bb.height / 2;
          if (!premier) json += ",";
          premier = false;
          json += "{\"label\":\"" + String(bb.label) + "\",";
          json += "\"conf\":" + String(bb.value, 2) + ",";
          json += "\"x\":" + String(cx) + ",\"y\":" + String(cy) + "}";
        }
        dsp = result.timing.dsp;
        classification = result.timing.classification;
        int total = dsp + classification;
        fps = total > 0 ? 1000.0f / total : 0.0f;
      } else {
        Serial.println("run_classifier a echoue.");
      }
    } else {
      Serial.println("Conversion/redimensionnement de l'image impossible.");
    }
  } else if (fb) {
    esp_camera_fb_return(fb);      // tampon PSRAM absent : on ne fuit pas la trame
  }
#else
  // Stub : aucune détection tant que le modèle n'est pas branché.
#endif

  json += "],\"timing\":{\"dsp\":" + String(dsp) +
          ",\"classification\":" + String(classification) + "},";
  json += "\"fps\":" + String(fps, 1) + "}";

  res->setHeader("Content-Type", "application/json");
  res->setHeader("Cache-Control", "no-store");
  res->print(json);
}

void setup() {
  Serial.begin(115200);
  delay(200);

  if (!initCamera()) Serial.println("Camera KO - verifier la carte et la PSRAM.");

#if USE_EDGE_IMPULSE
  // Tampon de travail pour l'inférence, alloué une fois pour toutes en PSRAM.
  rgb_buf = (uint8_t *) ps_malloc(CAM_W * CAM_H * 3);
  if (!rgb_buf) Serial.println("PSRAM insuffisante : inference desactivee.");
#endif

  // Point d'accès Wi-Fi
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASS);
  Serial.print("Point d'acces \""); Serial.print(AP_SSID);
  Serial.print("\" - IP "); Serial.println(WiFi.softAPIP());   // 192.168.4.1

  // Certificat auto-signé (généré au démarrage). Long avec une clé 2048 bits.
  Serial.println("Generation du certificat auto-signe (patienter ~30-60 s)...");
  cert = new SSLCert();
  int r = createSelfSignedCert(
      *cert, KEYSIZE_2048,
      "CN=192.168.4.1,O=TriSelectif,C=FR",
      "20240101000000",   // valide à partir de
      "20340101000000");  // valide jusqu'à
  if (r != 0) {
    Serial.printf("Echec creation certificat: %d\n", r);
    while (true) delay(1000);
  }

  // Serveur HTTPS + routes
  secureServer = new HTTPSServer(cert);
  secureServer->registerNode(new ResourceNode("/",        "GET",  &handleRoot));
  secureServer->registerNode(new ResourceNode("/login",   "POST", &handleLogin));
  secureServer->registerNode(new ResourceNode("/capture", "GET",  &handleCapture));
  secureServer->registerNode(new ResourceNode("/detect",  "GET",  &handleDetect));
  secureServer->start();

  if (secureServer->isRunning())
    Serial.println("HTTPS pret : ouvrir https://192.168.4.1/");
  else
    Serial.println("Echec demarrage du serveur HTTPS.");
}

void loop() {
  secureServer->loop();
  delay(1);
}
