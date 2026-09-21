#include "spotify_connect.h"

#ifdef CONFIG_SPOTIFY_CONNECT_ENABLE

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <new>
#include <string>
#include <string_view>

#include "BellLogger.h"
#include "BellTask.h"
#include "CSpotContext.h"
#include "LoginBlob.h"
#include "MDNSService.h"
#include "SpircHandler.h"
#include "TrackPlayer.h"
#include "WrappedSemaphore.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

extern "C" {
#include "audio_output.h"
#include "display.h"
#include "source_manager.h"
}

namespace {

static const char *TAG = "spotify";

class SpotifyPlayer;
static SpotifyPlayer *s_player;
static std::atomic<bool> s_active(false);

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = (char)std::tolower((unsigned char)c);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static void url_decode_in_place(char *text) {
  char *src = text;
  char *dst = text;
  while (*src) {
    if (*src == '+') {
      *dst++ = ' ';
      ++src;
    } else if (*src == '%' && src[1] && src[2]) {
      int hi = hex_value(src[1]);
      int lo = hex_value(src[2]);
      if (hi >= 0 && lo >= 0) {
        *dst++ = (char)((hi << 4) | lo);
        src += 3;
      } else {
        *dst++ = *src++;
      }
    } else {
      *dst++ = *src++;
    }
  }
  *dst = '\0';
}

class SpotifyPlayer : public bell::Task {
 public:
  SpotifyPlayer(const char *name, httpd_handle_t server, uint16_t port)
      : bell::Task("spotify", 32 * 1024, 0, 0),
        name_(name ? name : "ESP32 Spotify"), server_(server), port_(port) {}

  bool begin() { return startTask(); }
  esp_err_t handleGet(httpd_req_t *request);
  esp_err_t handlePost(httpd_req_t *request);

 protected:
  void runTask() override;

 private:
  std::string name_;
  httpd_handle_t server_;
  uint16_t port_;
  bell::WrappedSemaphore client_connected_{1};
  std::shared_ptr<cspot::LoginBlob> blob_;
  std::shared_ptr<cspot::SpircHandler> spirc_;
  std::unique_ptr<bell::MDNSService> mdns_service_;
  std::atomic<bool> paused_{true};
  std::atomic<bool> session_running_{false};
  std::atomic<bool> first_audio_pending_{false};
  std::atomic<int> volume_{UINT16_MAX / 2};
  uint32_t start_offset_ms_ = 0;
  uint32_t duration_ms_ = 0;
  std::string last_track_id_;

  void enableZeroConf();
  void handleEvent(std::unique_ptr<cspot::SpircHandler::Event> event);
  size_t writePcm(uint8_t *pcm, size_t bytes, std::string_view track_id);
  void startAudio();
  void stopAudio();
};

extern "C" esp_err_t spotify_get_handler(httpd_req_t *request) {
  return s_player ? s_player->handleGet(request) : ESP_FAIL;
}

extern "C" esp_err_t spotify_post_handler(httpd_req_t *request) {
  return s_player ? s_player->handlePost(request) : ESP_FAIL;
}

void SpotifyPlayer::enableZeroConf() {
  httpd_uri_t uri = {};
  uri.uri = "/spotify_info";
  uri.method = HTTP_GET;
  uri.handler = spotify_get_handler;
  ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server_, &uri));

  uri.method = HTTP_POST;
  uri.handler = spotify_post_handler;
  ESP_ERROR_CHECK_WITHOUT_ABORT(httpd_register_uri_handler(server_, &uri));

  mdns_service_ = bell::MDNSService::registerService(
      blob_->getDeviceName(), "_spotify-connect", "_tcp", "", port_,
      {{"VERSION", "1.0"}, {"CPath", "/spotify_info"}, {"Stack", "SP"}});
  ESP_LOGI(TAG, "Spotify ZeroConf ready as '%s' on port %u",
           blob_->getDeviceName().c_str(), (unsigned)port_);
}

esp_err_t SpotifyPlayer::handleGet(httpd_req_t *request) {
  if (!blob_) return ESP_ERR_INVALID_STATE;
  std::string body = blob_->buildZeroconfInfo();
  httpd_resp_set_type(request, "application/json");
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_send(request, body.c_str(), body.size());
}

esp_err_t SpotifyPlayer::handlePost(httpd_req_t *request) {
  source_manager_source_t owner = source_manager_current();
  if (owner != SOURCE_MANAGER_NONE && owner != SOURCE_MANAGER_SPOTIFY) {
    httpd_resp_set_type(request, "application/json");
    httpd_resp_set_status(request, "409 Conflict");
    return httpd_resp_sendstr(
        request,
        "{\"status\":202,\"statusString\":\"ERROR-SPOTIFY-BUSY\","
        "\"spotifyError\":0}");
  }

  if (request->content_len == 0 || request->content_len > 16384) {
    return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST,
                               "Invalid Spotify request");
  }

  char *body = static_cast<char *>(calloc(1, request->content_len + 1));
  if (!body) return ESP_ERR_NO_MEM;
  size_t received = 0;
  while (received < request->content_len) {
    int n = httpd_req_recv(request, body + received,
                           request->content_len - received);
    if (n <= 0) {
      free(body);
      return ESP_FAIL;
    }
    received += (size_t)n;
  }
  url_decode_in_place(body);

  std::map<std::string, std::string> query;
  char *save = nullptr;
  for (char *part = strtok_r(body, "&", &save); part;
       part = strtok_r(nullptr, "&", &save)) {
    char *value = strchr(part, '=');
    if (!value) continue;
    *value++ = '\0';
    query[part] = value;
  }

  bool complete = query.count("userName") && query.count("blob") &&
                  query.count("clientKey");
  if (complete) {
    blob_->loadZeroconfQuery(query);
    client_connected_.give();
  }
  free(body);

  httpd_resp_set_type(request, "application/json");
  if (!complete) {
    httpd_resp_set_status(request, "400 Bad Request");
    return httpd_resp_sendstr(
        request,
        "{\"status\":203,\"statusString\":\"ERROR-INVALID-PARAM\","
        "\"spotifyError\":0}");
  }
  return httpd_resp_sendstr(
      request,
      "{\"status\":101,\"statusString\":\"OK\",\"spotifyError\":0}");
}

void SpotifyPlayer::startAudio() {
  if (s_active.load()) return;
  if (!source_manager_acquire(SOURCE_MANAGER_SPOTIFY)) {
    ESP_LOGW(TAG, "Spotify could not acquire the audio output");
    if (spirc_) spirc_->setPause(true);
    return;
  }

  audio_output_stop();
  audio_output_set_sample_rate(44100);
  s_active = true;
  display_notify_spotify_active(true);
  display_notify_playback(false);
  ESP_LOGI(TAG, "Spotify owns the audio output");
}

void SpotifyPlayer::stopAudio() {
  if (!s_active.exchange(false)) return;
  source_manager_release(SOURCE_MANAGER_SPOTIFY);
  display_notify_spotify_active(false);
  display_notify_stopped();
  audio_output_start();
  ESP_LOGI(TAG, "Spotify released the audio output");
}

size_t SpotifyPlayer::writePcm(uint8_t *pcm, size_t bytes,
                               std::string_view track_id) {
  if (!s_active.load() || !pcm || bytes < 4) return bytes;

  if (last_track_id_ != track_id) {
    last_track_id_.assign(track_id.data(), track_id.size());
    first_audio_pending_ = true;
  }

  int16_t *samples = reinterpret_cast<int16_t *>(pcm);
  size_t sample_count = bytes / sizeof(int16_t);
  int32_t gain_q15 = std::clamp(volume_.load() / 2, 0, 32767);
  for (size_t i = 0; i < sample_count; ++i) {
    samples[i] = (int16_t)(((int32_t)samples[i] * gain_q15) >> 15);
  }

  esp_err_t err = audio_output_write_external_pcm(
      samples, sample_count / 2, portMAX_DELAY);
  if (err != ESP_OK) return 0;

  if (first_audio_pending_.exchange(false) && spirc_) {
    spirc_->notifyAudioReachedPlayback();
  }
  return bytes;
}

void SpotifyPlayer::handleEvent(
    std::unique_ptr<cspot::SpircHandler::Event> event) {
  switch (event->eventType) {
  case cspot::SpircHandler::EventType::PLAYBACK_START:
    start_offset_ms_ = (uint32_t)std::max(std::get<int>(event->data), 0);
    last_track_id_.clear();
    first_audio_pending_ = false;
    startAudio();
    break;

  case cspot::SpircHandler::EventType::PLAY_PAUSE:
    paused_ = std::get<bool>(event->data);
    if (!paused_) startAudio();
    if (s_active.load()) display_notify_playback(paused_);
    break;

  case cspot::SpircHandler::EventType::TRACK_INFO: {
    cspot::TrackInfo info = std::get<cspot::TrackInfo>(event->data);
    duration_ms_ = info.duration;
    display_notify_metadata(info.name.c_str(), info.artist.c_str(),
                            info.album.c_str(), duration_ms_ / 1000,
                            start_offset_ms_ / 1000);
    ESP_LOGI(TAG, "Track: %s — %s", info.name.c_str(), info.artist.c_str());
    break;
  }

  case cspot::SpircHandler::EventType::SEEK:
    if (s_active.load()) {
      uint32_t pos_ms = (uint32_t)std::max(std::get<int>(event->data), 0);
      display_notify_metadata(nullptr, nullptr, nullptr, duration_ms_ / 1000,
                              pos_ms / 1000);
    }
    break;

  case cspot::SpircHandler::EventType::VOLUME:
    volume_ = std::clamp(std::get<int>(event->data), 0, (int)UINT16_MAX);
    break;

  case cspot::SpircHandler::EventType::FLUSH:
  case cspot::SpircHandler::EventType::NEXT:
  case cspot::SpircHandler::EventType::PREV:
    last_track_id_.clear();
    first_audio_pending_ = false;
    break;

  case cspot::SpircHandler::EventType::DISC:
  case cspot::SpircHandler::EventType::DEPLETED:
    session_running_ = false;
    stopAudio();
    break;

  default:
    break;
  }
}

void SpotifyPlayer::runTask() {
  blob_ = std::make_shared<cspot::LoginBlob>(name_);
  enableZeroConf();

  for (;;) {
    ESP_LOGI(TAG, "Waiting for selection in the Spotify app");
    client_connected_.wait();

    try {
      auto ctx = cspot::Context::createFromBlob(blob_);
      ctx->config.clientId = CONFIG_SPOTIFY_CLIENT_ID;
      ctx->config.clientSecret = CONFIG_SPOTIFY_CLIENT_SECRET;
#if CONFIG_SPOTIFY_CONNECT_BITRATE == 320
      ctx->config.audioFormat = AudioFormat_OGG_VORBIS_320;
#elif CONFIG_SPOTIFY_CONNECT_BITRATE == 96
      ctx->config.audioFormat = AudioFormat_OGG_VORBIS_96;
#else
      ctx->config.audioFormat = AudioFormat_OGG_VORBIS_160;
#endif

      ctx->session->connectWithRandomAp();
      ctx->config.authData = ctx->session->authenticate(blob_);
      if (ctx->config.authData.empty()) {
        ESP_LOGE(TAG, "Spotify authentication was rejected");
        continue;
      }

      spirc_ = std::make_shared<cspot::SpircHandler>(ctx);
      spirc_->getTrackPlayer()->setDataCallback(
          [this](uint8_t *data, size_t bytes, std::string_view track_id) {
            return writePcm(data, bytes, track_id);
          });
      spirc_->setEventHandler(
          [this](std::unique_ptr<cspot::SpircHandler::Event> event) {
            handleEvent(std::move(event));
          });

      session_running_ = true;
      ctx->session->startTask();
      while (session_running_.load()) {
        ctx->session->handlePacket();
      }

      spirc_->disconnect();
      spirc_.reset();
      stopAudio();
    } catch (const std::exception &e) {
      ESP_LOGE(TAG, "Spotify session failed: %s", e.what());
      spirc_.reset();
      stopAudio();
      vTaskDelay(pdMS_TO_TICKS(2000));
    } catch (...) {
      ESP_LOGE(TAG, "Spotify session failed with an unknown error");
      spirc_.reset();
      stopAudio();
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
  }
}

}  // namespace

extern "C" esp_err_t spotify_connect_start(httpd_handle_t server,
                                             uint16_t port,
                                             const char *device_name) {
  if (s_player) return ESP_ERR_INVALID_STATE;
  if (!server || !device_name || !device_name[0]) return ESP_ERR_INVALID_ARG;
  if (strlen(CONFIG_SPOTIFY_CLIENT_ID) == 0 ||
      strlen(CONFIG_SPOTIFY_CLIENT_SECRET) == 0) {
    ESP_LOGW(TAG,
             "Spotify disabled: set SPOTIFY_CLIENT_ID and "
             "SPOTIFY_CLIENT_SECRET build secrets");
    return ESP_ERR_NOT_FOUND;
  }

  bell::setDefaultLogger();
  s_player = new (std::nothrow) SpotifyPlayer(device_name, server, port);
  if (!s_player) return ESP_ERR_NO_MEM;
  if (!s_player->begin()) {
    delete s_player;
    s_player = nullptr;
    return ESP_ERR_NO_MEM;
  }
  return ESP_OK;
}

extern "C" bool spotify_connect_is_active(void) { return s_active.load(); }

#else

extern "C" esp_err_t spotify_connect_start(httpd_handle_t server,
                                             uint16_t port,
                                             const char *device_name) {
  (void)server;
  (void)port;
  (void)device_name;
  return ESP_ERR_NOT_SUPPORTED;
}

extern "C" bool spotify_connect_is_active(void) { return false; }

#endif
