#include "dlna_renderer.h"

#include "audio_output.h"
#include "display.h"
#include "ethernet.h"
#include "playback_control.h"
#include "settings.h"
#include "software_dsp.h"
#include "source_manager.h"
#include "wifi.h"

#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define DLNA_SOAP_BODY_MAX      8192
#define DLNA_INPUT_BUFFER_SIZE  4096
#define DLNA_OUTPUT_BUFFER_SIZE 8192
#define DLNA_SSDP_PORT          1900
#define DLNA_NOTIFY_SECONDS     900
#define DLNA_PLAYER_STACK_SIZE  10240
#define DLNA_SSDP_STACK_SIZE    8192
#define DLNA_SSDP_PACKET_SIZE   1536
#define DLNA_SSDP_MESSAGE_SIZE  896
#define DLNA_SERVER_NAME        "ESP32/5.5 UPnP/1.0 AirPlayDLNA/3.4.0"

typedef enum {
  DLNA_STATE_STOPPED,
  DLNA_STATE_PLAYING,
  DLNA_STATE_PAUSED,
  DLNA_STATE_TRANSITIONING,
} dlna_state_t;

static const char *TAG = "dlna";

static TaskHandle_t s_ssdp_task;
static TaskHandle_t s_player_task;
static esp_http_client_handle_t s_http_client;
static volatile dlna_state_t s_state = DLNA_STATE_STOPPED;
static volatile bool s_stop_requested;
static volatile bool s_pause_requested;
static volatile bool s_resume_fade_pending;
static volatile bool s_ssdp_running;
static volatile bool s_airplay_active;
static volatile bool s_ssdp_announce_pending;
static volatile int s_volume_percent = 50;
static volatile bool s_muted;
static char s_uri[1024];
static char s_metadata[2048];
static char s_udn[48];
static bool s_handlers_registered;
static bool s_decoders_registered;
static uint16_t s_http_port = 80;

static const char *AVTRANSPORT_SERVICE =
    "urn:schemas-upnp-org:service:AVTransport:1";
static const char *RENDERING_SERVICE =
    "urn:schemas-upnp-org:service:RenderingControl:1";
static const char *CONNECTION_SERVICE =
    "urn:schemas-upnp-org:service:ConnectionManager:1";

static const char *AVTRANSPORT_SCPD =
    "<?xml version=\"1.0\"?><scpd "
    "xmlns=\"urn:schemas-upnp-org:service-1-0\"><specVersion><major>1</major>"
    "<minor>0</minor></specVersion><actionList>"
    "<action><name>SetAVTransportURI</name></action>"
    "<action><name>Play</name></action><action><name>Pause</name></action>"
    "<action><name>Stop</name></action>"
    "<action><name>GetTransportInfo</name></action>"
    "<action><name>GetPositionInfo</name></action>"
    "<action><name>GetMediaInfo</name></action></actionList>"
    "<serviceStateTable><stateVariable sendEvents=\"yes\">"
    "<name>TransportState</name><dataType>string</dataType>"
    "</stateVariable></serviceStateTable></scpd>";

static const char *RENDERING_SCPD =
    "<?xml version=\"1.0\"?><scpd "
    "xmlns=\"urn:schemas-upnp-org:service-1-0\"><specVersion><major>1</major>"
    "<minor>0</minor></specVersion><actionList>"
    "<action><name>GetVolume</name></action><action><name>SetVolume</name>"
    "</action><action><name>GetMute</name></action><action><name>SetMute</name>"
    "</action></actionList><serviceStateTable>"
    "<stateVariable "
    "sendEvents=\"yes\"><name>Volume</name><dataType>ui2</dataType>"
    "<allowedValueRange><minimum>0</minimum><maximum>100</maximum><step>1</"
    "step>"
    "</allowedValueRange></stateVariable></serviceStateTable></scpd>";

static const char *CONNECTION_SCPD =
    "<?xml version=\"1.0\"?><scpd "
    "xmlns=\"urn:schemas-upnp-org:service-1-0\"><specVersion><major>1</major>"
    "<minor>0</minor></specVersion><actionList>"
    "<action><name>GetProtocolInfo</name></action>"
    "<action><name>GetCurrentConnectionIDs</name></action>"
    "<action><name>GetCurrentConnectionInfo</name></action></actionList>"
    "<serviceStateTable/></scpd>";

static const char *state_name(void) {
  switch (s_state) {
  case DLNA_STATE_PLAYING:
    return "PLAYING";
  case DLNA_STATE_PAUSED:
    return "PAUSED_PLAYBACK";
  case DLNA_STATE_TRANSITIONING:
    return "TRANSITIONING";
  default:
    return "STOPPED";
  }
}

static void current_ip(char *out, size_t out_len) {
  if (ethernet_is_connected() && ethernet_get_ip_str(out, out_len) == ESP_OK) {
    return;
  }
  if (wifi_get_ip_str(out, out_len) != ESP_OK) {
    strlcpy(out, "0.0.0.0", out_len);
  }
}

static void make_udn(void) {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(s_udn, sizeof(s_udn), "uuid:41505258-%02x%02x-%02x%02x-%02x%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static esp_err_t send_xml(httpd_req_t *req, const char *xml) {
  httpd_resp_set_type(req, "text/xml; charset=\"utf-8\"");
  httpd_resp_set_hdr(req, "Server", DLNA_SERVER_NAME);
  return httpd_resp_sendstr(req, xml);
}

static esp_err_t device_xml_handler(httpd_req_t *req) {
  char name[65] = SETTINGS_DEFAULT_DEVICE_NAME;
  char ip[16];
  settings_get_device_name(name, sizeof(name));
  current_ip(ip, sizeof(ip));

  char *xml = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!xml) {
    xml = malloc(4096);
  }
  if (!xml) {
    return httpd_resp_send_500(req);
  }

  snprintf(
      xml, 4096,
      "<?xml version=\"1.0\"?><root "
      "xmlns=\"urn:schemas-upnp-org:device-1-0\"><specVersion><major>1</major>"
      "<minor>0</minor></specVersion><URLBase>http://%s:%u/</URLBase><device>"
      "<deviceType>urn:schemas-upnp-org:device:MediaRenderer:1</deviceType>"
      "<friendlyName>%s</friendlyName><manufacturer>AirPlay and DLNA "
      "Receiver</manufacturer>"
      "<modelName>AirPlay 2 and DLNA Receiver</modelName>"
      "<modelNumber>3.4.0</modelNumber><UDN>%s</UDN>"
      "<serviceList><service><serviceType>%s</serviceType>"
      "<serviceId>urn:upnp-org:serviceId:AVTransport</serviceId>"
      "<SCPDURL>/dlna/avtransport.xml</SCPDURL>"
      "<controlURL>/dlna/control/avtransport</controlURL>"
      "<eventSubURL>/dlna/event/avtransport</eventSubURL></service>"
      "<service><serviceType>%s</serviceType>"
      "<serviceId>urn:upnp-org:serviceId:RenderingControl</serviceId>"
      "<SCPDURL>/dlna/renderingcontrol.xml</SCPDURL>"
      "<controlURL>/dlna/control/renderingcontrol</controlURL>"
      "<eventSubURL>/dlna/event/renderingcontrol</eventSubURL></service>"
      "<service><serviceType>%s</serviceType>"
      "<serviceId>urn:upnp-org:serviceId:ConnectionManager</serviceId>"
      "<SCPDURL>/dlna/connectionmanager.xml</SCPDURL>"
      "<controlURL>/dlna/control/connectionmanager</controlURL>"
      "<eventSubURL>/dlna/event/connectionmanager</eventSubURL></service>"
      "</serviceList></device></root>",
      ip, (unsigned)s_http_port, name, s_udn, AVTRANSPORT_SERVICE,
      RENDERING_SERVICE, CONNECTION_SERVICE);
  esp_err_t err = send_xml(req, xml);
  free(xml);
  return err;
}

static char *recv_body(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len >= DLNA_SOAP_BODY_MAX) {
    return NULL;
  }
  char *body = malloc((size_t)req->content_len + 1);
  if (!body) {
    return NULL;
  }
  int got = 0;
  while (got < req->content_len) {
    int n = httpd_req_recv(req, body + got, req->content_len - got);
    if (n <= 0) {
      free(body);
      return NULL;
    }
    got += n;
  }
  body[got] = 0;
  return body;
}

static bool xml_value(const char *body, const char *tag, char *out,
                      size_t out_len) {
  char open[64], close[64];
  snprintf(open, sizeof(open), "<%s>", tag);
  snprintf(close, sizeof(close), "</%s>", tag);
  const char *start = strstr(body, open);
  if (!start) {
    return false;
  }
  start += strlen(open);
  const char *end = strstr(start, close);
  if (!end) {
    return false;
  }
  size_t n = (size_t)(end - start);
  if (n >= out_len) {
    n = out_len - 1;
  }
  memcpy(out, start, n);
  out[n] = 0;

  struct {
    const char *entity;
    char value;
  } entities[] = {{"&amp;", '&'},
                  {"&lt;", '<'},
                  {"&gt;", '>'},
                  {"&quot;", '"'},
                  {"&apos;", '\''}};
  for (size_t i = 0; i < sizeof(entities) / sizeof(entities[0]); i++) {
    char *p;
    while ((p = strstr(out, entities[i].entity)) != NULL) {
      size_t elen = strlen(entities[i].entity);
      *p = entities[i].value;
      memmove(p + 1, p + elen, strlen(p + elen) + 1);
    }
  }
  return true;
}

/* Extract HH:MM:SS[.mmm] from the DIDL-Lite res duration attribute. */
static uint32_t metadata_duration_secs(const char *metadata) {
  const char *p = metadata ? strstr(metadata, "duration=") : NULL;
  if (!p) {
    return 0;
  }
  p += strlen("duration=");
  if (*p != '\'' && *p != '"') {
    return 0;
  }
  p++;

  unsigned hours = 0, minutes = 0;
  double seconds = 0;
  if (sscanf(p, "%u:%u:%lf", &hours, &minutes, &seconds) != 3 || minutes > 59 ||
      seconds < 0 || seconds >= 60) {
    return 0;
  }
  return hours * 3600U + minutes * 60U + (uint32_t)seconds;
}

/* DLNA previously kept DIDL metadata private to the renderer, so the display
 * only received AirPlay RTSP metadata. Translate the current DIDL-Lite item
 * into the existing display event format instead of adding another UI path. */
static void emit_dlna_metadata(void) {
  char title[64] = {0};
  char artist[64] = {0};
  char album[64] = {0};
  bool found = false;

  found |= xml_value(s_metadata, "dc:title", title, sizeof(title));
  found |= xml_value(s_metadata, "upnp:artist", artist, sizeof(artist));
  found |= xml_value(s_metadata, "upnp:album", album, sizeof(album));
  uint32_t duration_secs = metadata_duration_secs(s_metadata);
  found |= duration_secs > 0;

  /* Some controllers omit DIDL-Lite. Give the display a useful fallback
   * rather than showing three dashes. */
  if (!title[0] && s_uri[0]) {
    const char *name = strrchr(s_uri, '/');
    name = name ? name + 1 : s_uri;
    size_t len = strcspn(name, "?#");
    if (len >= sizeof(title)) {
      len = sizeof(title) - 1;
    }
    memcpy(title, name, len);
    title[len] = '\0';
    found |= len > 0;
  }

  if (found) {
    ESP_LOGI(TAG,
             "Display metadata: title='%s' artist='%s' album='%s' "
             "duration=%lus",
             title, artist, album, (unsigned long)duration_secs);
    display_notify_metadata(title, artist, album, duration_secs, 0);
  }
}

static const char *soap_action(httpd_req_t *req, const char *body) {
  static char action[64];
  if (httpd_req_get_hdr_value_str(req, "SOAPACTION", action, sizeof(action)) ==
      ESP_OK) {
    char *hash = strrchr(action, '#');
    if (hash) {
      char *end = strpbrk(hash + 1, "\"' ");
      if (end) {
        *end = 0;
      }
      return hash + 1;
    }
  }
  const char *u = strstr(body, "<u:");
  if (!u) {
    return "";
  }
  u += 3;
  size_t n = strcspn(u, " >");
  if (n >= sizeof(action)) {
    n = sizeof(action) - 1;
  }
  memcpy(action, u, n);
  action[n] = 0;
  return action;
}

static esp_err_t soap_response(httpd_req_t *req, const char *service,
                               const char *action, const char *arguments) {
  char *xml = malloc(4096);
  if (!xml) {
    return httpd_resp_send_500(req);
  }
  snprintf(
      xml, 4096,
      "<?xml version=\"1.0\"?><s:Envelope "
      "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body><u:%sResponse xmlns:u=\"urn:schemas-upnp-org:service:%s:1\">"
      "%s</u:%sResponse></s:Body></s:Envelope>",
      action, service, arguments ? arguments : "", action);
  esp_err_t err = send_xml(req, xml);
  free(xml);
  return err;
}

static esp_err_t soap_fault(httpd_req_t *req, int code,
                            const char *description) {
  char xml[768];
  snprintf(
      xml, sizeof(xml),
      "<?xml version=\"1.0\"?><s:Envelope "
      "xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\">"
      "<s:Body><s:Fault><faultcode>s:Client</faultcode>"
      "<faultstring>UPnPError</faultstring><detail><UPnPError "
      "xmlns=\"urn:schemas-upnp-org:control-1-0\"><errorCode>%d</errorCode>"
      "<errorDescription>%s</errorDescription></UPnPError></detail>"
      "</s:Fault></s:Body></s:Envelope>",
      code, description);
  httpd_resp_set_status(req, "500 Internal Server Error");
  return send_xml(req, xml);
}

static void pcm_apply_gain(int16_t *pcm, size_t samples) {
  int volume = s_muted ? 0 : s_volume_percent;
  int32_t gain_q15 = (int32_t)volume * 32767 / 100;
  for (size_t i = 0; i < samples; i++) {
    pcm[i] = (int16_t)(((int32_t)pcm[i] * gain_q15) >> 15);
  }
}

static bool player_wait_if_paused(void);

static esp_err_t write_stereo_block(int16_t *pcm, size_t frames) {
  bool fade_out = s_pause_requested;
  bool fade_in = s_resume_fade_pending;
  esp_err_t err = audio_output_write_pcm_transition(pcm, frames, fade_in,
                                                    fade_out, portMAX_DELAY);
  if (err != ESP_OK) {
    return err;
  }
  if (fade_in) {
    s_resume_fade_pending = false;
  }
  if (fade_out) {
    // Enter PAUSED only after a zero-ending block is safely queued to I2S.
    s_pause_requested = false;
    s_state = DLNA_STATE_PAUSED;
    display_notify_playback(true);
    software_dsp_reset_state();
    if (!player_wait_if_paused()) {
      return ESP_ERR_INVALID_STATE;
    }
  }
  return ESP_OK;
}

static esp_err_t write_pcm(uint8_t *pcm_bytes, size_t bytes, int channels) {
  if (channels == 2) {
    bytes &= ~(size_t)3;
    pcm_apply_gain((int16_t *)pcm_bytes, bytes / sizeof(int16_t));
    return bytes ? write_stereo_block((int16_t *)pcm_bytes, bytes / 4) : ESP_OK;
  }
  if (channels != 1) {
    return ESP_ERR_NOT_SUPPORTED;
  }

  const int16_t *mono = (const int16_t *)pcm_bytes;
  size_t frames = bytes / sizeof(int16_t);
  int16_t stereo[512 * 2];
  while (frames && !s_stop_requested) {
    size_t count = frames > 512 ? 512 : frames;
    for (size_t i = 0; i < count; i++) {
      stereo[i * 2] = mono[i];
      stereo[i * 2 + 1] = mono[i];
    }
    pcm_apply_gain(stereo, count * 2);
    esp_err_t err = write_stereo_block(stereo, count);
    if (err != ESP_OK) {
      return err;
    }
    mono += count;
    frames -= count;
  }
  return ESP_OK;
}

static bool player_wait_if_paused(void) {
  while (!s_stop_requested && s_state == DLNA_STATE_PAUSED) {
    vTaskDelay(pdMS_TO_TICKS(25));
  }
  return !s_stop_requested;
}

static bool has_extension(const char *uri, const char *extension) {
  const char *query = strchr(uri, '?');
  size_t uri_len = query ? (size_t)(query - uri) : strlen(uri);
  size_t ext_len = strlen(extension);
  return uri_len >= ext_len &&
         !strncasecmp(uri + uri_len - ext_len, extension, ext_len);
}

static esp_audio_simple_dec_type_t
detect_decoder_type(const char *uri, const uint8_t *data, size_t len) {
  /* Container signatures are checked before extensions. Windows DLNA often
   * serves media through extensionless URLs. */
  if (len >= 4 && !memcmp(data, "fLaC", 4)) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
  }
  if (len >= 12 && !memcmp(data, "RIFF", 4) && !memcmp(data + 8, "WAVE", 4)) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
  }
  if (len >= 12 && !memcmp(data + 4, "ftyp", 4)) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
  }
  if (len >= 4 && !memcmp(data, "OggS", 4)) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
  }
  if (len >= 9 && !memcmp(data, "#!AMR-WB\n", 9)) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_AMRWB;
  }
  if (len >= 6 && !memcmp(data, "#!AMR\n", 6)) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_AMRNB;
  }
  /* ADTS AAC uses a 12-bit sync word, a variable MPEG-ID bit and layer 00.
   * Check it before the more general MPEG audio sync used for MP3. */
  if (len >= 2 && data[0] == 0xff && (data[1] & 0xf6) == 0xf0) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
  }
  if (has_extension(uri, ".flac")) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
  }
  if (has_extension(uri, ".wav")) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;
  }
  if (has_extension(uri, ".aac") || has_extension(uri, ".adts")) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
  }
  if (has_extension(uri, ".m4a") || has_extension(uri, ".m4b") ||
      has_extension(uri, ".mp4")) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;
  }
  if (has_extension(uri, ".ogg") || has_extension(uri, ".oga") ||
      has_extension(uri, ".opus")) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
  }
  if (has_extension(uri, ".awb") || has_extension(uri, ".amrwb")) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_AMRWB;
  }
  if (has_extension(uri, ".amr") || has_extension(uri, ".amrnb")) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_AMRNB;
  }
  if (has_extension(uri, ".mp3") || (len >= 3 && !memcmp(data, "ID3", 3)) ||
      (len >= 2 && data[0] == 0xff && (data[1] & 0xe0) == 0xe0)) {
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
  }
  return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

static const char *decoder_name(esp_audio_simple_dec_type_t type) {
  switch (type) {
  case ESP_AUDIO_SIMPLE_DEC_TYPE_MP3:
    return "MP3";
  case ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC:
    return "FLAC";
  case ESP_AUDIO_SIMPLE_DEC_TYPE_WAV:
    return "WAV";
  case ESP_AUDIO_SIMPLE_DEC_TYPE_AAC:
    return "AAC";
  case ESP_AUDIO_SIMPLE_DEC_TYPE_M4A:
    return "M4A";
  case ESP_AUDIO_SIMPLE_DEC_TYPE_OGG:
    return "OGG/Vorbis/Opus";
  case ESP_AUDIO_SIMPLE_DEC_TYPE_AMRNB:
    return "AMR-NB";
  case ESP_AUDIO_SIMPLE_DEC_TYPE_AMRWB:
    return "AMR-WB";
  default:
    return "UNKNOWN";
  }
}

static esp_err_t decode_http_stream(esp_http_client_handle_t client,
                                    const char *uri, uint8_t *input,
                                    size_t first_len) {
  esp_audio_simple_dec_type_t type = detect_decoder_type(uri, input, first_len);
  if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
    ESP_LOGE(TAG, "Unsupported DLNA media format");
    return ESP_ERR_NOT_SUPPORTED;
  }

  if (!s_decoders_registered) {
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    s_decoders_registered = true;
  }

  esp_audio_simple_dec_cfg_t cfg = {
      .dec_type = type,
      .dec_cfg = NULL,
      .cfg_size = 0,
      .use_frame_dec = false,
  };
  esp_audio_simple_dec_handle_t decoder = NULL;
  if (esp_audio_simple_dec_open(&cfg, &decoder) != ESP_AUDIO_ERR_OK ||
      !decoder) {
    ESP_LOGE(TAG, "DLNA decoder could not start");
    return ESP_ERR_NO_MEM;
  }

  uint8_t *output = heap_caps_malloc(DLNA_OUTPUT_BUFFER_SIZE,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!output) {
    output = malloc(DLNA_OUTPUT_BUFFER_SIZE);
  }
  if (!output) {
    ESP_LOGE(TAG, "Not enough memory for DLNA decoder");
    esp_audio_simple_dec_close(decoder);
    return ESP_ERR_NO_MEM;
  }

  size_t output_capacity = DLNA_OUTPUT_BUFFER_SIZE;
  size_t input_len = first_len;
  bool eof = false;
  bool format_ready = false;
  esp_err_t result = ESP_OK;

  while (!s_stop_requested) {
    if (!player_wait_if_paused()) {
      break;
    }

    if (input_len == 0 && !eof) {
      int n =
          esp_http_client_read(client, (char *)input, DLNA_INPUT_BUFFER_SIZE);
      if (n > 0) {
        input_len = (size_t)n;
      } else if (n == 0) {
        eof = true;
      } else if (s_stop_requested) {
        break;
      } else {
        ESP_LOGW(TAG, "DLNA HTTP read interrupted: %d", n);
        result = ESP_FAIL;
        break;
      }
    }

    esp_audio_simple_dec_raw_t raw = {
        .buffer = input,
        .len = input_len,
        .eos = eof,
        .consumed = 0,
        .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
    };
    bool process_once = true;
    while (!s_stop_requested && (raw.len > 0 || (eof && process_once))) {
      esp_audio_simple_dec_out_t out = {
          .buffer = output,
          .len = output_capacity,
      };
      uint32_t before = raw.len;
      esp_audio_err_t dec_err =
          esp_audio_simple_dec_process(decoder, &raw, &out);
      if (dec_err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH &&
          out.needed_size > output_capacity) {
        uint8_t *larger = realloc(output, out.needed_size);
        if (!larger) {
          result = ESP_ERR_NO_MEM;
          s_stop_requested = true;
          break;
        }
        output = larger;
        output_capacity = out.needed_size;
        continue;
      }
      if (dec_err != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "DLNA decode failed: %d", dec_err);
        result = ESP_FAIL;
        s_stop_requested = true;
        break;
      }

      if (out.decoded_size) {
        esp_audio_simple_dec_info_t info = {0};
        if (esp_audio_simple_dec_get_info(decoder, &info) != ESP_AUDIO_ERR_OK ||
            info.bits_per_sample != 16 ||
            (info.channel != 1 && info.channel != 2)) {
          ESP_LOGE(TAG, "Unsupported decoded PCM format");
          result = ESP_ERR_NOT_SUPPORTED;
          s_stop_requested = true;
          break;
        }
        if (!format_ready) {
          audio_output_set_sample_rate(info.sample_rate);
          ESP_LOGI(TAG, "DLNA %s: %lu Hz, %u channel", decoder_name(type),
                   (unsigned long)info.sample_rate, info.channel);
          format_ready = true;
        }
        result = write_pcm(out.buffer, out.decoded_size, info.channel);
        if (result != ESP_OK) {
          s_stop_requested = true;
          break;
        }
      }

      if (raw.consumed > raw.len) {
        result = ESP_FAIL;
        s_stop_requested = true;
        break;
      }
      raw.buffer += raw.consumed;
      raw.len -= raw.consumed;
      process_once = false;
      if (before == raw.len && out.decoded_size == 0) {
        break;
      }
    }
    input_len = 0;
    if (eof) {
      break;
    }
  }

  free(output);
  esp_audio_simple_dec_close(decoder);
  return result;
}

static void restore_airplay_output(void) {
  audio_output_set_sample_rate(CONFIG_OUTPUT_SAMPLE_RATE_HZ);
  audio_output_start();
  playback_control_set_source(PLAYBACK_SOURCE_AIRPLAY);
}

static void player_task(void *arg) {
  (void)arg;
  char uri[sizeof(s_uri)];
  strlcpy(uri, s_uri, sizeof(uri));

  if (s_airplay_active) {
    display_notify_dlna_active(false);
    s_state = DLNA_STATE_STOPPED;
    s_player_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  s_state = DLNA_STATE_TRANSITIONING;
  audio_output_stop();
  playback_control_set_source(PLAYBACK_SOURCE_DLNA);

  uint8_t *input = heap_caps_malloc(DLNA_INPUT_BUFFER_SIZE,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!input) {
    input = malloc(DLNA_INPUT_BUFFER_SIZE);
  }

  esp_http_client_config_t config = {
      .url = uri,
      .timeout_ms = 1500,
      .buffer_size = DLNA_INPUT_BUFFER_SIZE,
      .buffer_size_tx = 1024,
      .disable_auto_redirect = false,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  s_http_client = client;
  esp_err_t err = client ? esp_http_client_open(client, 0) : ESP_FAIL;
  if (err == ESP_OK) {
    esp_http_client_fetch_headers(client);
  }

  size_t first_len = 0;
  if (err == ESP_OK && input) {
    int n = esp_http_client_read(client, (char *)input, 16);
    if (n > 0) {
      first_len = (size_t)n;
    }
  }

  if (err == ESP_OK && input && first_len > 0 && !s_stop_requested &&
      !s_airplay_active) {
    s_state = DLNA_STATE_PLAYING;
    emit_dlna_metadata();
    display_notify_playback(false);
    decode_http_stream(client, uri, input, first_len);
  } else if (!s_stop_requested && !s_airplay_active) {
    ESP_LOGE(TAG, "Cannot open DLNA media URL");
  }

  s_http_client = NULL;
  if (client) {
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
  }
  free(input);

  s_state = DLNA_STATE_STOPPED;
  s_stop_requested = false;
  s_pause_requested = false;
  s_resume_fade_pending = false;
  source_manager_release(SOURCE_MANAGER_DLNA);
  if (playback_control_get_source() == PLAYBACK_SOURCE_DLNA) {
    playback_control_set_source(PLAYBACK_SOURCE_NONE);
  }
  display_notify_dlna_active(false);
  /* AirPlay's CONNECTED event has already prepared its normal screen during
   * a takeover. Do not overwrite that screen with DLNA's standby cleanup. */
  if (!s_airplay_active) {
    display_notify_stopped();
  }
  restore_airplay_output();
  ESP_LOGI(TAG, "DLNA playback ended; AirPlay remained available");
  s_player_task = NULL;
  vTaskDelete(NULL);
}

static bool stop_player(bool wait) {
  s_stop_requested = true;
  esp_http_client_handle_t client = s_http_client;
  if (client) {
    esp_http_client_close(client);
  }
  if (wait) {
    for (int i = 0; i < 100 && s_player_task; i++) {
      vTaskDelay(pdMS_TO_TICKS(20));
    }
  }
  if (!s_player_task) {
    s_state = DLNA_STATE_STOPPED;
  }
  return s_player_task == NULL;
}

static bool start_player(void) {
  if (!s_uri[0] || s_airplay_active) {
    return false;
  }
  if (s_player_task && !stop_player(true)) {
    return false;
  }
  if (!source_manager_acquire(SOURCE_MANAGER_DLNA)) {
    return false;
  }
  display_notify_dlna_active(true);
  s_stop_requested = false;
  s_pause_requested = false;
  s_resume_fade_pending = false;
  BaseType_t result = xTaskCreatePinnedToCore(player_task, "dlna_stream",
                                              DLNA_PLAYER_STACK_SIZE, NULL, 6,
                                              &s_player_task, 1);
  if (result != pdPASS) {
    s_player_task = NULL;
    source_manager_release(SOURCE_MANAGER_DLNA);
    display_notify_dlna_active(false);
    return false;
  }
  playback_control_set_source(PLAYBACK_SOURCE_DLNA);
  return true;
}

static esp_err_t avtransport_control_handler(httpd_req_t *req) {
  char *body = recv_body(req);
  if (!body) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SOAP body");
  }
  const char *action = soap_action(req, body);
  char action_copy[64];
  strlcpy(action_copy, action, sizeof(action_copy));
  char args[3072] = {0};

  if (!strcmp(action_copy, "SetAVTransportURI")) {
    xml_value(body, "CurrentURI", s_uri, sizeof(s_uri));
    xml_value(body, "CurrentURIMetaData", s_metadata, sizeof(s_metadata));
    ESP_LOGI(TAG, "Set URI: %.160s", s_uri);
    if (!s_airplay_active) {
      display_notify_dlna_active(true);
      emit_dlna_metadata();
    }
  } else if (!strcmp(action_copy, "Play")) {
    if (s_airplay_active) {
      free(body);
      return soap_fault(req, 701, "AirPlay has priority");
    }
    if (s_state == DLNA_STATE_PAUSED) {
      s_resume_fade_pending = true;
      s_state = DLNA_STATE_PLAYING;
      display_notify_playback(false);
    } else if (!start_player()) {
      free(body);
      return soap_fault(req, 704, "Cannot play media");
    }
  } else if (!strcmp(action_copy, "Pause")) {
    if (s_state == DLNA_STATE_PLAYING) {
      s_pause_requested = true;
    }
  } else if (!strcmp(action_copy, "Stop")) {
    stop_player(false);
    display_notify_dlna_active(false);
    if (!s_airplay_active) {
      display_notify_stopped();
    }
  } else if (!strcmp(action_copy, "GetTransportInfo")) {
    snprintf(args, sizeof(args),
             "<CurrentTransportState>%s</CurrentTransportState>"
             "<CurrentTransportStatus>OK</CurrentTransportStatus>"
             "<CurrentSpeed>1</CurrentSpeed>",
             state_name());
  } else if (!strcmp(action_copy, "GetPositionInfo")) {
    snprintf(args, sizeof(args),
             "<Track>1</Track><TrackDuration>00:00:00</TrackDuration>"
             "<TrackMetaData></TrackMetaData><TrackURI>%s</TrackURI>"
             "<RelTime>00:00:00</RelTime><AbsTime>00:00:00</AbsTime>"
             "<RelCount>0</RelCount><AbsCount>0</AbsCount>",
             s_uri);
  } else if (!strcmp(action_copy, "GetMediaInfo")) {
    snprintf(args, sizeof(args),
             "<NrTracks>1</NrTracks><MediaDuration>00:00:00</MediaDuration>"
             "<CurrentURI>%s</CurrentURI>"
             "<CurrentURIMetaData></CurrentURIMetaData>"
             "<NextURI></NextURI><NextURIMetaData></NextURIMetaData>"
             "<PlayMedium>NETWORK</PlayMedium>"
             "<RecordMedium>NOT_IMPLEMENTED</RecordMedium>"
             "<WriteStatus>NOT_IMPLEMENTED</WriteStatus>",
             s_uri);
  } else {
    free(body);
    return soap_fault(req, 401, "Invalid Action");
  }

  esp_err_t result = soap_response(req, "AVTransport", action_copy, args);
  free(body);
  return result;
}

static esp_err_t rendering_control_handler(httpd_req_t *req) {
  char *body = recv_body(req);
  if (!body) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SOAP body");
  }
  const char *action = soap_action(req, body);
  char action_copy[64];
  strlcpy(action_copy, action, sizeof(action_copy));
  char args[128] = {0};
  char value[16];

  if (!strcmp(action_copy, "SetVolume") &&
      xml_value(body, "DesiredVolume", value, sizeof(value))) {
    int v = atoi(value);
    s_volume_percent = v < 0 ? 0 : (v > 100 ? 100 : v);
  } else if (!strcmp(action_copy, "GetVolume")) {
    snprintf(args, sizeof(args), "<CurrentVolume>%d</CurrentVolume>",
             s_volume_percent);
  } else if (!strcmp(action_copy, "SetMute") &&
             xml_value(body, "DesiredMute", value, sizeof(value))) {
    s_muted = atoi(value) != 0 || !strcasecmp(value, "true");
  } else if (!strcmp(action_copy, "GetMute")) {
    snprintf(args, sizeof(args), "<CurrentMute>%d</CurrentMute>",
             s_muted ? 1 : 0);
  } else {
    free(body);
    return soap_fault(req, 401, "Invalid Action");
  }

  esp_err_t result = soap_response(req, "RenderingControl", action_copy, args);
  free(body);
  return result;
}

static esp_err_t connection_control_handler(httpd_req_t *req) {
  char *body = recv_body(req);
  if (!body) {
    return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid SOAP body");
  }
  const char *action = soap_action(req, body);
  char action_copy[64];
  strlcpy(action_copy, action, sizeof(action_copy));
  char args[1024] = {0};

  if (!strcmp(action_copy, "GetProtocolInfo")) {
    snprintf(args, sizeof(args),
             "<Source></Source><Sink>http-get:*:audio/mpeg:*,"
             "http-get:*:audio/mp3:*,http-get:*:audio/flac:*,"
             "http-get:*:audio/x-flac:*,http-get:*:audio/wav:*,"
             "http-get:*:audio/x-wav:*,http-get:*:audio/aac:*,"
             "http-get:*:audio/aacp:*,http-get:*:audio/mp4:*,"
             "http-get:*:audio/x-m4a:*,http-get:*:audio/ogg:*,"
             "http-get:*:application/ogg:*,http-get:*:audio/opus:*,"
             "http-get:*:audio/amr:*,http-get:*:audio/amr-wb:*</Sink>");
  } else if (!strcmp(action_copy, "GetCurrentConnectionIDs")) {
    snprintf(args, sizeof(args), "<ConnectionIDs>0</ConnectionIDs>");
  } else if (!strcmp(action_copy, "GetCurrentConnectionInfo")) {
    snprintf(args, sizeof(args),
             "<RcsID>0</RcsID><AVTransportID>0</AVTransportID>"
             "<ProtocolInfo></ProtocolInfo><PeerConnectionManager>"
             "</PeerConnectionManager><PeerConnectionID>-1</PeerConnectionID>"
             "<Direction>Input</Direction><Status>OK</Status>");
  } else {
    free(body);
    return soap_fault(req, 401, "Invalid Action");
  }

  esp_err_t result = soap_response(req, "ConnectionManager", action_copy, args);
  free(body);
  return result;
}

static esp_err_t dlna_get_handler(httpd_req_t *req) {
  if (!strcmp(req->uri, "/dlna/device.xml")) {
    return device_xml_handler(req);
  }
  if (!strcmp(req->uri, "/dlna/avtransport.xml")) {
    return send_xml(req, AVTRANSPORT_SCPD);
  }
  if (!strcmp(req->uri, "/dlna/renderingcontrol.xml")) {
    return send_xml(req, RENDERING_SCPD);
  }
  if (!strcmp(req->uri, "/dlna/connectionmanager.xml")) {
    return send_xml(req, CONNECTION_SCPD);
  }
  return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                             "DLNA endpoint not found");
}

static esp_err_t dlna_post_handler(httpd_req_t *req) {
  if (!strcmp(req->uri, "/dlna/control/avtransport")) {
    return avtransport_control_handler(req);
  }
  if (!strcmp(req->uri, "/dlna/control/renderingcontrol")) {
    return rendering_control_handler(req);
  }
  if (!strcmp(req->uri, "/dlna/control/connectionmanager")) {
    return connection_control_handler(req);
  }
  return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                             "DLNA endpoint not found");
}

/* The UPnP eventSubURL endpoints use SUBSCRIBE (HTTP method 26) and
 * UNSUBSCRIBE (method 27). Registering only GET/POST made controllers retry
 * continuously after every 405 response. */
static esp_err_t dlna_event_handler(httpd_req_t *req) {
  if (strncmp(req->uri, "/dlna/event/", strlen("/dlna/event/")) != 0) {
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND,
                               "DLNA event endpoint not found");
  }

  httpd_resp_set_hdr(req, "Server", DLNA_SERVER_NAME);
  httpd_resp_set_hdr(req, "Connection", "close");

  if (req->method == HTTP_SUBSCRIBE) {
    char sid[96] = {0};
    size_t sid_len = httpd_req_get_hdr_value_len(req, "SID");
    if (sid_len > 0 && sid_len < sizeof(sid) &&
        httpd_req_get_hdr_value_str(req, "SID", sid, sizeof(sid)) != ESP_OK) {
      sid[0] = '\0';
    }
    if (sid[0] == '\0') {
      strlcpy(sid, s_udn, sizeof(sid));
    }
    httpd_resp_set_hdr(req, "SID", sid);
    httpd_resp_set_hdr(req, "TIMEOUT", "Second-1800");
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_send(req, NULL, 0);
  }

  if (req->method == HTTP_UNSUBSCRIBE) {
    httpd_resp_set_status(req, "200 OK");
    return httpd_resp_send(req, NULL, 0);
  }

  return httpd_resp_send_err(req, HTTPD_405_METHOD_NOT_ALLOWED,
                             "Unsupported DLNA event method");
}

/* SSDP uses comparatively large UDP and HTTP-style text buffers. Keeping
 * these buffers on the FreeRTOS task stack caused an immediate stack overflow
 * during the first ssdp:alive announcement. Prefer PSRAM and fall back to the
 * normal heap so boards without PSRAM still work. */
static char *ssdp_alloc_buffer(size_t size) {
  char *buffer = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer) {
    buffer = malloc(size);
  }
  return buffer;
}

static void ssdp_send_response(int sock, const struct sockaddr_in *to,
                               const char *st) {
  char ip[16];
  current_ip(ip, sizeof(ip));
  char *response = ssdp_alloc_buffer(DLNA_SSDP_MESSAGE_SIZE);
  if (!response) {
    ESP_LOGW(TAG, "No memory for SSDP response");
    return;
  }
  snprintf(response, DLNA_SSDP_MESSAGE_SIZE,
           "HTTP/1.1 200 OK\r\nCACHE-CONTROL: max-age=1800\r\nEXT:\r\n"
           "LOCATION: http://%s:%u/dlna/device.xml\r\n"
           "SERVER: %s\r\nST: %s\r\nUSN: %s::%s\r\n"
           "BOOTID.UPNP.ORG: 1\r\nCONFIGID.UPNP.ORG: 1\r\n\r\n",
           ip, (unsigned)s_http_port, DLNA_SERVER_NAME, st, s_udn, st);
  sendto(sock, response, strlen(response), 0, (const struct sockaddr *)to,
         sizeof(*to));
  free(response);
}

static void ssdp_notify(int sock, const char *nts) {
  char ip[16];
  current_ip(ip, sizeof(ip));
  struct sockaddr_in addr = {.sin_family = AF_INET,
                             .sin_port = htons(DLNA_SSDP_PORT)};
  inet_pton(AF_INET, "239.255.255.250", &addr.sin_addr);
  const char *types[] = {"upnp:rootdevice",
                         s_udn,
                         "urn:schemas-upnp-org:device:MediaRenderer:1",
                         AVTRANSPORT_SERVICE,
                         RENDERING_SERVICE,
                         CONNECTION_SERVICE};
  char *msg = ssdp_alloc_buffer(DLNA_SSDP_MESSAGE_SIZE);
  if (!msg) {
    ESP_LOGW(TAG, "No memory for SSDP notification");
    return;
  }
  for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
    bool udn_only = !strcmp(types[i], s_udn);
    snprintf(msg, DLNA_SSDP_MESSAGE_SIZE,
             "NOTIFY * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
             "CACHE-CONTROL: max-age=1800\r\n"
             "LOCATION: http://%s:%u/dlna/device.xml\r\n"
             "NT: %s\r\nNTS: %s\r\nSERVER: %s\r\n"
             "USN: %s%s%s\r\nBOOTID.UPNP.ORG: 1\r\n"
             "CONFIGID.UPNP.ORG: 1\r\n\r\n",
             ip, (unsigned)s_http_port, types[i], nts, DLNA_SERVER_NAME, s_udn,
             udn_only ? "" : "::", udn_only ? "" : types[i]);
    sendto(sock, msg, strlen(msg), 0, (struct sockaddr *)&addr, sizeof(addr));
  }
  free(msg);
}

static void ssdp_task(void *arg) {
  (void)arg;

  /* The setup AP is for provisioning only. Do not advertise an unusable DLNA
   * renderer until a station or Ethernet connection has a real network IP. */
  while (s_ssdp_running && !wifi_is_connected() && !ethernet_is_connected()) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  if (!s_ssdp_running) {
    s_ssdp_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  char *packet = ssdp_alloc_buffer(DLNA_SSDP_PACKET_SIZE);
  if (!packet) {
    ESP_LOGE(TAG, "No memory for SSDP receive buffer");
    s_ssdp_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "SSDP socket failed: errno=%d", errno);
    free(packet);
    s_ssdp_task = NULL;
    vTaskDelete(NULL);
    return;
  }
  int reuse = 1;
  setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
  struct sockaddr_in bind_addr = {.sin_family = AF_INET,
                                  .sin_port = htons(DLNA_SSDP_PORT),
                                  .sin_addr.s_addr = htonl(INADDR_ANY)};
  if (bind(sock, (struct sockaddr *)&bind_addr, sizeof(bind_addr)) < 0) {
    ESP_LOGE(TAG, "SSDP bind failed: errno=%d", errno);
    close(sock);
    free(packet);
    s_ssdp_task = NULL;
    vTaskDelete(NULL);
    return;
  }

  struct ip_mreq membership = {
      .imr_multiaddr.s_addr = inet_addr("239.255.255.250"),
      .imr_interface.s_addr = htonl(INADDR_ANY),
  };
  setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership,
             sizeof(membership));
  struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  if (!s_airplay_active) {
    ssdp_notify(sock, "ssdp:alive");
  }
  int elapsed = 0;
  while (s_ssdp_running) {
    if (s_airplay_active) {
      vTaskDelay(pdMS_TO_TICKS(250));
      continue;
    }
    if (s_ssdp_announce_pending) {
      s_ssdp_announce_pending = false;
      ssdp_notify(sock, "ssdp:alive");
      elapsed = 0;
    }

    struct sockaddr_in from;
    socklen_t from_len = sizeof(from);
    int n = recvfrom(sock, packet, DLNA_SSDP_PACKET_SIZE - 1, 0,
                     (struct sockaddr *)&from, &from_len);
    if (n > 0) {
      packet[n] = 0;
      if (strstr(packet, "M-SEARCH") && strstr(packet, "ssdp:discover")) {
        const char *types[] = {"upnp:rootdevice",
                               s_udn,
                               "urn:schemas-upnp-org:device:MediaRenderer:1",
                               AVTRANSPORT_SERVICE,
                               RENDERING_SERVICE,
                               CONNECTION_SERVICE};
        for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
          if (strstr(packet, "ssdp:all") || strstr(packet, types[i])) {
            ssdp_send_response(sock, &from, types[i]);
          }
        }
      }
    }
    if (++elapsed >= DLNA_NOTIFY_SECONDS) {
      ssdp_notify(sock, "ssdp:alive");
      elapsed = 0;
    }
  }

  if (!s_airplay_active) {
    ssdp_notify(sock, "ssdp:byebye");
  }
  close(sock);
  free(packet);
  s_ssdp_task = NULL;
  vTaskDelete(NULL);
}

esp_err_t dlna_renderer_register(httpd_handle_t server, uint16_t server_port) {
  if (!server) {
    return ESP_ERR_INVALID_ARG;
  }
  s_http_port = server_port ? server_port : 80;
  make_udn();

  if (!s_handlers_registered) {
    const httpd_uri_t get_uri = {
        .uri = "/dlna/*", .method = HTTP_GET, .handler = dlna_get_handler};
    const httpd_uri_t post_uri = {
        .uri = "/dlna/*", .method = HTTP_POST, .handler = dlna_post_handler};
    const httpd_uri_t subscribe_uri = {.uri = "/dlna/event/*",
                                       .method = HTTP_SUBSCRIBE,
                                       .handler = dlna_event_handler};
    const httpd_uri_t unsubscribe_uri = {.uri = "/dlna/event/*",
                                         .method = HTTP_UNSUBSCRIBE,
                                         .handler = dlna_event_handler};
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &get_uri), TAG,
                        "register DLNA GET failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &post_uri), TAG,
                        "register DLNA POST failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &subscribe_uri), TAG,
                        "register DLNA SUBSCRIBE failed");
    ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &unsubscribe_uri),
                        TAG, "register DLNA UNSUBSCRIBE failed");
    s_handlers_registered = true;
  }

  if (!s_ssdp_task) {
    s_ssdp_running = true;
    if (xTaskCreate(ssdp_task, "dlna_ssdp", DLNA_SSDP_STACK_SIZE, NULL, 4,
                    &s_ssdp_task) != pdPASS) {
      s_ssdp_running = false;
      s_ssdp_task = NULL;
      return ESP_ERR_NO_MEM;
    }
  }

  ESP_LOGI(TAG, "DLNA added beside official AirPlay "
                "(MP3/FLAC/WAV/AAC/M4A/OGG/OPUS/AMR)");
  return ESP_OK;
}

void dlna_renderer_stop(void) {
  stop_player(true);
  s_ssdp_running = false;
}

void dlna_renderer_set_airplay_active(bool active) {
  if (s_airplay_active == active) {
    return;
  }
  s_airplay_active = active;
  if (active) {
    display_notify_dlna_active(false);
    ESP_LOGI(TAG, "SSDP paused; AirPlay has exclusive network priority");
    if (!stop_player(true)) {
      ESP_LOGW(TAG, "DLNA player did not stop before AirPlay takeover");
    }
  } else {
    s_ssdp_announce_pending = true;
  }
}

bool dlna_renderer_is_playing(void) {
  return s_state != DLNA_STATE_STOPPED;
}

void dlna_renderer_toggle_pause(void) {
  if (s_state == DLNA_STATE_PLAYING) {
    s_pause_requested = true;
  } else if (s_state == DLNA_STATE_PAUSED) {
    s_resume_fade_pending = true;
    s_state = DLNA_STATE_PLAYING;
    display_notify_playback(false);
  }
}

void dlna_renderer_toggle_mute(void) {
  s_muted = !s_muted;
}

void dlna_renderer_set_muted(bool muted) {
  s_muted = muted;
}

bool dlna_renderer_is_muted(void) {
  return s_muted;
}

void dlna_renderer_volume_step(int percent) {
  int v = s_volume_percent + percent;
  s_volume_percent = v < 0 ? 0 : (v > 100 ? 100 : v);
}

esp_err_t dlna_renderer_play_uri(const char *uri) {
  if (!uri ||
      (strncmp(uri, "http://", 7) != 0 && strncmp(uri, "https://", 8) != 0)) {
    return ESP_ERR_INVALID_ARG;
  }
  if (s_airplay_active) {
    return ESP_ERR_INVALID_STATE;
  }
  strlcpy(s_uri, uri, sizeof(s_uri));
  s_metadata[0] = '\0';
  display_notify_dlna_active(true);
  emit_dlna_metadata();
  if (start_player()) {
    return ESP_OK;
  }
  display_notify_dlna_active(false);
  return ESP_FAIL;
}

void dlna_renderer_stop_playback(void) {
  stop_player(false);
  display_notify_dlna_active(false);
  display_notify_stopped();
}

const char *dlna_renderer_current_uri(void) {
  return s_uri;
}
