/*
  page_web.h - page HTML/JS servie par la carte (route GET /).
  Fichier séparé du .ino EXPRÈS : le pré-processeur de l'IDE Arduino ne
  réécrit que les .ino et mange les grandes chaînes brutes R"(...)" ;
  dans un .h, la page passe intacte au compilateur.
*/
#pragma once

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
  .stack{position:relative;width:320px;height:240px;border:1px solid var(--border);border-radius:6px;box-shadow:var(--shadow);overflow:hidden;background:#000}
  .stack img{display:block;width:320px;height:240px}
  .stack canvas{position:absolute;left:0;top:0;background:transparent}
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
    <!-- Le flux MJPEG s'affiche dans l'<img> ; le canvas transparent par-dessus
         ne porte que les marqueurs de détection. -->
    <div class="stack">
      <img id="stream" alt="">
      <canvas id="cam" width="320" height="240"></canvas>
    </div>
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
        <div class="m"><div class="k">Débit modèle</div><div class="v"><span id="m-fps">-</span> fps</div></div>
        <div class="m"><div class="k">Objets retenus</div><div class="v"><span id="m-count">-</span></div></div>
      </div>
    </div>
  </section>
</main>
<script>
const URL_LOGIN="/login", URL_JSON="/detect";
// Le flux MJPEG vit sur son propre serveur, port 81 (voir firmware).
const URL_STREAM=location.protocol+"//"+location.hostname+":81/stream";
const POLL_INTERVAL_MS=300;   // /detect renvoie un cache : le polling est bon marché
const MODEL_W=160, MODEL_H=160, DISPLAY_W=320, DISPLAY_H=240;
const CAM_W=320, CAM_H=240;   // résolution du flux : doit suivre CAM_W/CAM_H du firmware
const N_STABLE=3;
const CLASS_COLORS={bouteille:"#2f6fed",canette:"#e0781a",carton:"#2f9e44"};
const CONSIGNES={bouteille:{titre:"PLASTIQUE"},canette:{titre:"MÉTAL"},carton:{titre:"CARTON"}};

// Le firmware recadre l'image (crop CENTRÉ) au format du modèle avant analyse.
// On refait le même calcul ici pour placer les marqueurs au bon endroit.
const RATIO=MODEL_W/MODEL_H;
let CROP_W=CAM_W, CROP_H=CAM_W/RATIO;
if(CROP_H>CAM_H){ CROP_H=CAM_H; CROP_W=CAM_H*RATIO; }
const SCALE_X=(CROP_W/MODEL_W)*(DISPLAY_W/CAM_W), SCALE_Y=(CROP_H/MODEL_H)*(DISPLAY_H/CAM_H);
const OFFSET_X=((CAM_W-CROP_W)/2)*(DISPLAY_W/CAM_W), OFFSET_Y=((CAM_H-CROP_H)/2)*(DISPLAY_H/CAM_H);
function modeleVersAffichage(x,y){return {x:x*SCALE_X+OFFSET_X, y:y*SCALE_Y+OFFSET_Y};}

const canvas=document.getElementById("cam"), ctx=canvas.getContext("2d");
const streamImg=document.getElementById("stream");
let seuil=0.6, candidateSig=null, candidateCount=0, stableCounts=null, token=null, tokenExpire=0, timer=null;
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
async function chargerDetections(){ const res=await authFetch(URL_JSON+"?t="+Date.now()); return res.json(); }

// Comptage par classe : {bouteille:1, canette:2, ...} (classes absentes omises)
function compterParClasse(dets){
  const c={}; for(const d of dets) c[d.label]=(c[d.label]||0)+1; return c;
}
// Même vote de stabilité qu'avant (N_STABLE frames identiques), mais sur la
// COMBINAISON complète classes+quantités au lieu de la seule classe dominante.
function majComptageStable(counts){
  const sig=CLASSES.map(k=>k+":"+(counts[k]||0)).join("|");
  if(sig===candidateSig) candidateCount++;
  else{ candidateSig=sig; candidateCount=1; }
  if(candidateCount>=N_STABLE) stableCounts=counts;
  return stableCounts;
}
function dessiner(dets){
  // Canvas transparent : on n'y dessine QUE les marqueurs, la vidéo vit dans l'<img>.
  ctx.clearRect(0,0,DISPLAY_W,DISPLAY_H);
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
  const actives=stableCounts?CLASSES.filter(c=>stableCounts[c]>0):[];
  if(actives.length>0){
    // Ex. 1 seule classe : "MÉTAL" (ou "x2 MÉTAL") ; plusieurs : "x1 MÉTAL + x1 PLASTIQUE"
    titreEl.textContent=actives.map(c=>{
      const n=stableCounts[c];
      return (actives.length>1||n>1?"x"+n+" ":"")+CONSIGNES[c].titre;
    }).join(" + ");
    sousEl.textContent=actives.map(c=>stableCounts[c]+" "+c).join(", ");
    // Couleur de la classe si une seule ; neutre si consigne multiple
    titreEl.style.color=(actives.length===1)?CLASS_COLORS[actives[0]]:"var(--text)";
    titreEl.style.fontSize=(actives.length>1)?"26px":"";   // titre long : un cran plus petit
  } else {
    titreEl.textContent="Aucun objet détecté"; sousEl.textContent=""; titreEl.style.color="var(--muted)";
    titreEl.style.fontSize="";
  }
  document.getElementById("m-dsp").textContent=timing.dsp;
  document.getElementById("m-inf").textContent=timing.classification;
  document.getElementById("m-fps").textContent=fps;
  document.getElementById("m-count").textContent=dets.length;
}
async function cycle(){
  if(jetonExpire()){ deconnexion("Session expirée, reconnecte-toi."); return; }
  try{
    const data=await chargerDetections();
    const retenues=(data.detections||[]).filter(d=>d.conf>=seuil);
    majComptageStable(compterParClasse(retenues));
    dessiner(retenues);
    majPanneau(retenues,data.timing||{dsp:0,classification:0},data.fps??0);
  }catch(e){ console.warn("cycle KO:",e); }
}
function demarrerBoucle(){
  if(timer) return;
  streamImg.src=URL_STREAM+"?token="+encodeURIComponent(token);   // ouvre le flux MJPEG
  cycle(); timer=setInterval(cycle,POLL_INTERVAL_MS);
}
function arreterBoucle(){
  if(timer) clearInterval(timer); timer=null;
  streamImg.src="";                                               // ferme le flux
  ctx.clearRect(0,0,DISPLAY_W,DISPLAY_H);
}

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
document.getElementById("btn-logout").addEventListener("click", ()=>deconnexion());
</script>
)HTMLPAGE";
