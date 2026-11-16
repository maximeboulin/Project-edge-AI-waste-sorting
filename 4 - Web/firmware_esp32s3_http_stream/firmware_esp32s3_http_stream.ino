/*
  Aide au tri sélectif - firmware ESP32-S3 : HTTP + flux MJPEG + inférence en tâche dédiée
  ----------------------------------------------------------------------------------------
  Version "fluide". Par rapport à la version HTTPS précédente :

    1) HTTP simple au lieu de HTTPS.
       Le handshake TLS coûtait ~0,5-1 s de calcul PAR IMAGE (une connexion par
       frame) et des dizaines de Ko de RAM par socket -> échecs "SSL_accept failed"
       et vidéo saccadée. En HTTP le flux est instantané.
       NOTE SÉCURITÉ (pour le jury) : l'authentification par jeton est CONSERVÉE
       (login + Bearer token). Seul le chiffrement de transport est retiré, en
       échange de la fluidité — compromis assumé sur un réseau local dédié
       (point d'accès privé de la carte). Le code HTTPS reste disponible dans
       l'ancienne version du fichier pour la démonstration.

    2) Flux vidéo MJPEG (multipart/x-mixed-replace) sur un serveur dédié (port 81)
       au lieu du polling /capture : UNE connexion persistante, la carte pousse
       les JPEG. Plus aucun coût d'établissement de connexion par frame.

    3) Inférence découplée du serveur web : elle tourne en continu dans une tâche
       FreeRTOS épinglée sur le coeur 0 (le serveur web reste sur le coeur 1).
       /detect ne calcule plus rien : il renvoie le DERNIER résultat en cache,
       donc répond en quelques millisecondes.

    4) Post-traitement anti-doublons : les centroïdes de même classe distants de
       moins de MERGE_DIST_PX pixels sont fusionnés (un grand carton ne compte
       plus double). Le vote de stabilité sur N frames est déjà côté page web.

  Routes :
    GET  /            -> la page (intégrée, avec écran de connexion)     [port 80]
    POST /login       -> body {"user":"...","pass":"..."} ; {"token","ttl"} ou 401
    GET  /detect      -> JSON du dernier résultat d'inférence (Bearer requis)
    GET  /stream      -> flux MJPEG (jeton en paramètre ?token=..., car une
                         balise <img> ne peut pas envoyer d'en-tête Authorization)
                                                                          [port 81]

  --- DÉPENDANCES ---
  Plus AUCUNE bibliothèque externe à installer : le serveur esp_http_server est
  intégré au core ESP32. La bibliothèque "esp32_https_server" n'est plus utilisée.

  --- À SAVOIR ---
  1) Le dossier de ce sketch doit s'appeler "firmware_esp32s3_http_stream".
  2) Carte : XIAO_ESP32S3, PSRAM activée (OPI PSRAM).
  3) Plus de certificat à générer : le démarrage est quasi immédiat, et plus
     d'avertissement de sécurité navigateur. Ouvrir http://192.168.4.1/
*/

#include "esp_camera.h"
#include "img_converters.h"
#include <WiFi.h>
#include "esp_http_server.h"

// 0 = caméra + web + auth seuls (détections vides) ; 1 = vraie inférence FOMO
#define USE_EDGE_IMPULSE 1
#if USE_EDGE_IMPULSE
  #include <Projet_t__inferencing.h>   // <-- remplacer par TON export Edge Impulse
  #include <edge-impulse-sdk/dsp/image/image.hpp>   // crop_and_interpolate_rgb888

  // Garde-fou : ce firmware lit result.bounding_boxes, qui n'existe que pour un
  // modèle de détection d'objets (FOMO).
  #if EI_CLASSIFIER_OBJECT_DETECTION != 1
    #error "L'export Edge Impulse n'est pas un modele de detection d'objets (FOMO)."
  #endif
#endif

// ---------- Identifiants (démo) ----------
// En production : stocker un hachage salé, jamais le mot de passe en clair.
const char *IDENT_USER = "admin";
const char *IDENT_PASS = "tri";

// ---------- Point d'accès Wi-Fi ----------
const char *AP_SSID = "TriSelectif";
const char *AP_PASS = "tri12345";       // 8 caractères minimum

// ---------- Jetons de session ----------
#define MAX_SESSIONS 4
const unsigned long TTL_MS = 15UL * 60UL * 1000UL;   // 15 min

struct Session {
  String token;
  unsigned long expire;
};
Session sessions[MAX_SESSIONS];

// ---------- Serveurs HTTP ----------
// Deux instances : le flux MJPEG occupe sa connexion en permanence, il vit donc
// sur son propre serveur (port 81) pour ne jamais bloquer /login et /detect.
httpd_handle_t serveur_web    = NULL;   // port 80 : page, login, detect
httpd_handle_t serveur_stream = NULL;   // port 81 : flux MJPEG

// ---------- Anti-doublons FOMO ----------
// Deux centroïdes de même classe à moins de MERGE_DIST_PX pixels (repère du
// modèle) sont considérés comme le même objet et fusionnés.
#define MERGE_DIST_PX 24
#define MAX_DETS 16

// ---------- Résultat d'inférence partagé (tâche coeur 0 -> handler /detect) ----------
SemaphoreHandle_t detect_mutex = NULL;
String detect_json = "{\"detections\":[],\"timing\":{\"dsp\":0,\"classification\":0},\"fps\":0}";

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

// Résolution de capture. Cohérent avec config.frame_size ET avec CAM_W/CAM_H
// dans la page (calcul du recadrage pour placer les marqueurs).
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
static uint8_t *rgb_buf = nullptr;

// Callback du SDK Edge Impulse : offset/length en PIXELS, valeur 0xRRGGBB en float.
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

// ---------- Utilitaires requête/réponse (API esp_http_server) ----------
// Extraction très simple d'une valeur texte dans un JSON plat (démo).
String extraireJson(const String &json, const char *cle) {
  String motif = String("\"") + cle + "\"";
  int k = json.indexOf(motif);            if (k < 0) return "";
  int deux = json.indexOf(':', k + motif.length()); if (deux < 0) return "";
  int q1 = json.indexOf('"', deux);       if (q1 < 0) return "";
  int q2 = json.indexOf('"', q1 + 1);     if (q2 < 0) return "";
  return json.substring(q1 + 1, q2);
}

// Jeton dans l'en-tête "Authorization: Bearer ..." (pour /detect)
bool estAutorise(httpd_req_t *req) {
  char buf[96];
  if (httpd_req_get_hdr_value_str(req, "Authorization", buf, sizeof(buf)) != ESP_OK)
    return false;
  String h(buf);
  if (!h.startsWith("Bearer ")) return false;
  return validerJeton(h.substring(7));
}

// Jeton en paramètre d'URL "?token=..." (pour /stream : une balise <img> ne
// peut pas envoyer d'en-tête HTTP ; le jeton passe donc par l'URL. Acceptable
// ici : réseau local privé + jeton à durée de vie courte).
bool estAutoriseParQuery(httpd_req_t *req) {
  char query[160], val[64];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) return false;
  if (httpd_query_key_value(query, "token", val, sizeof(val)) != ESP_OK) return false;
  return validerJeton(String(val));
}

esp_err_t envoyer401(httpd_req_t *req) {
  httpd_resp_set_status(req, "401 Unauthorized");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, "{\"error\":\"non autorise\"}", HTTPD_RESP_USE_STRLEN);
}

// ---------- Page web intégrée (voir page_web.h, second onglet du sketch) ----------
#include "page_web.h"

// ---------- Tâche d'inférence (coeur 0) ----------
#if USE_EDGE_IMPULSE
// Détections fusionnées (anti-doublons)
struct DetFusion {
  const char *label;
  float conf;
  float x, y;    // centroïde (repère du modèle)
  int n;         // nb de blobs fusionnés
};

void tacheInference(void *param) {
  unsigned long dernier = millis();
  for (;;) {
    if (!rgb_buf) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) { vTaskDelay(pdMS_TO_TICKS(50)); continue; }

    int src_w = fb->width, src_h = fb->height;
    bool ok = fmt2rgb888(fb->buf, fb->len, fb->format, rgb_buf);
    esp_camera_fb_return(fb);   // rendu tout de suite : la suite ne lit que rgb_buf

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

        // --- Fusion des centroïdes proches de même classe (anti-doublons) ---
        DetFusion dets[MAX_DETS];
        int n_dets = 0;
        for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
          ei_impulse_result_bounding_box_t bb = result.bounding_boxes[i];
          if (bb.value == 0) continue;      // FOMO remplit la grille : cases vides sautées
          float cx = bb.x + bb.width / 2.0f;
          float cy = bb.y + bb.height / 2.0f;

          int cible = -1;
          for (int j = 0; j < n_dets; j++) {
            if (strcmp(dets[j].label, bb.label) != 0) continue;
            float dx = dets[j].x - cx, dy = dets[j].y - cy;
            if (dx * dx + dy * dy <= (float)(MERGE_DIST_PX * MERGE_DIST_PX)) { cible = j; break; }
          }
          if (cible >= 0) {
            // Moyenne pondérée de la position, confiance = max des blobs fusionnés
            DetFusion &d = dets[cible];
            d.x = (d.x * d.n + cx) / (d.n + 1);
            d.y = (d.y * d.n + cy) / (d.n + 1);
            if (bb.value > d.conf) d.conf = bb.value;
            d.n++;
          } else if (n_dets < MAX_DETS) {
            dets[n_dets].label = bb.label;
            dets[n_dets].conf  = bb.value;
            dets[n_dets].x = cx; dets[n_dets].y = cy;
            dets[n_dets].n = 1;
            n_dets++;
          }
        }

        // --- Cadence réelle de la boucle d'inférence ---
        unsigned long maintenant = millis();
        float fps = (maintenant > dernier) ? 1000.0f / (float)(maintenant - dernier) : 0.0f;
        dernier = maintenant;

        // --- Construction du JSON puis publication atomique ---
        String json = "{\"detections\":[";
        for (int j = 0; j < n_dets; j++) {
          if (j) json += ",";
          json += "{\"label\":\"" + String(dets[j].label) + "\",";
          json += "\"conf\":" + String(dets[j].conf, 2) + ",";
          json += "\"x\":" + String((int)dets[j].x) + ",\"y\":" + String((int)dets[j].y) + "}";
        }
        json += "],\"timing\":{\"dsp\":" + String(result.timing.dsp) +
                ",\"classification\":" + String(result.timing.classification) + "},";
        json += "\"fps\":" + String(fps, 1) + "}";

        if (xSemaphoreTake(detect_mutex, portMAX_DELAY) == pdTRUE) {
          detect_json = json;
          xSemaphoreGive(detect_mutex);
        }
      } else {
        Serial.println("run_classifier a echoue.");
      }
    } else {
      Serial.println("Conversion/redimensionnement de l'image impossible.");
    }

    // Petit répit : laisse la caméra alimenter aussi le flux MJPEG.
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}
#endif

// ---------- Handlers HTTP ----------
esp_err_t handleRoot(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, PAGE_HTML, HTTPD_RESP_USE_STRLEN);
}

esp_err_t handleLogin(httpd_req_t *req) {
  char buf[257];
  String body;
  int restant = req->content_len;
  while (restant > 0) {
    int n = httpd_req_recv(req, buf, min(restant, 256));
    if (n <= 0) break;
    buf[n] = 0;
    body += String(buf);
    restant -= n;
  }
  String user = extraireJson(body, "user");
  String pass = extraireJson(body, "pass");

  httpd_resp_set_type(req, "application/json");
  if (user == IDENT_USER && pass == IDENT_PASS) {
    String tok = creerSession();
    String rep = String("{\"token\":\"") + tok + "\",\"ttl\":" + String(TTL_MS / 1000) + "}";
    return httpd_resp_send(req, rep.c_str(), HTTPD_RESP_USE_STRLEN);
  }
  httpd_resp_set_status(req, "401 Unauthorized");
  return httpd_resp_send(req, "{\"error\":\"identifiants invalides\"}", HTTPD_RESP_USE_STRLEN);
}

// /detect : renvoie le DERNIER résultat calculé par la tâche d'inférence.
// Aucune capture ni inférence ici -> réponse en quelques millisecondes.
esp_err_t handleDetect(httpd_req_t *req) {
  if (!estAutorise(req)) return envoyer401(req);

  String copie;
  if (xSemaphoreTake(detect_mutex, portMAX_DELAY) == pdTRUE) {
    copie = detect_json;
    xSemaphoreGive(detect_mutex);
  }
  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, copie.c_str(), HTTPD_RESP_USE_STRLEN);
}

// /stream : flux MJPEG. UNE connexion persistante ; la carte pousse les JPEG.
esp_err_t handleStream(httpd_req_t *req) {
  if (!estAutoriseParQuery(req)) return envoyer401(req);

  httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  char entete[80];
  for (;;) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) break;

    size_t hlen = snprintf(entete, sizeof(entete),
        "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
        (unsigned) fb->len);

    bool okEnvoi =
        httpd_resp_send_chunk(req, entete, hlen) == ESP_OK &&
        httpd_resp_send_chunk(req, (const char *) fb->buf, fb->len) == ESP_OK &&
        httpd_resp_send_chunk(req, "\r\n", 2) == ESP_OK;

    esp_camera_fb_return(fb);
    if (!okEnvoi) break;               // client parti : on sort proprement

    vTaskDelay(pdMS_TO_TICKS(50));     // ~20 fps max ; laisse des trames à l'inférence
  }
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

void setup() {
  Serial.begin(115200);
  delay(200);

  if (!initCamera()) Serial.println("Camera KO - verifier la carte et la PSRAM.");

  detect_mutex = xSemaphoreCreateMutex();

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

  // ----- Serveur principal (port 80) : page, login, detect -----
  httpd_config_t conf_web = HTTPD_DEFAULT_CONFIG();
  conf_web.server_port = 80;
  conf_web.ctrl_port   = 32768;
  conf_web.lru_purge_enable = true;
  if (httpd_start(&serveur_web, &conf_web) == ESP_OK) {
    httpd_uri_t uri_root   = { .uri = "/",       .method = HTTP_GET,  .handler = handleRoot,   .user_ctx = NULL };
    httpd_uri_t uri_login  = { .uri = "/login",  .method = HTTP_POST, .handler = handleLogin,  .user_ctx = NULL };
    httpd_uri_t uri_detect = { .uri = "/detect", .method = HTTP_GET,  .handler = handleDetect, .user_ctx = NULL };
    httpd_register_uri_handler(serveur_web, &uri_root);
    httpd_register_uri_handler(serveur_web, &uri_login);
    httpd_register_uri_handler(serveur_web, &uri_detect);
  } else {
    Serial.println("Echec demarrage serveur web (port 80).");
  }

  // ----- Serveur du flux (port 81) : /stream -----
  // Instance séparée : le flux occupe sa connexion en continu et ne doit
  // jamais bloquer /login ou /detect.
  httpd_config_t conf_stream = HTTPD_DEFAULT_CONFIG();
  conf_stream.server_port = 81;
  conf_stream.ctrl_port   = 32769;
  conf_stream.max_open_sockets = 2;    // 1 spectateur + marge
  if (httpd_start(&serveur_stream, &conf_stream) == ESP_OK) {
    httpd_uri_t uri_stream = { .uri = "/stream", .method = HTTP_GET, .handler = handleStream, .user_ctx = NULL };
    httpd_register_uri_handler(serveur_stream, &uri_stream);
  } else {
    Serial.println("Echec demarrage serveur stream (port 81).");
  }

#if USE_EDGE_IMPULSE
  // Inférence en continu sur le COEUR 0 ; les serveurs web (tâches ESP-IDF) et
  // la loop() Arduino restent sur le coeur 1. La vidéo ne gèle plus pendant
  // qu'un objet est analysé.
  xTaskCreatePinnedToCore(tacheInference, "inference", 20480, NULL, 1, NULL, 0);
#endif

  Serial.println("HTTP pret : ouvrir http://192.168.4.1/");
}

void loop() {
  // Les serveurs esp_http_server tournent dans leurs propres tâches :
  // plus rien à faire ici.
  delay(1000);
}
