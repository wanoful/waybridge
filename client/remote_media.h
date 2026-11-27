#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "third_party/json.hpp"

// Native worker is the session boundary for MPRIS2 <-> Windows SMTC.
// The GUI never sends arbitrary JSON back to Linux, nor can a stale media card
// act on a replaced player or a subsequent connection, nor act while read-only.
class RemoteMedia {
  using Json = nlohmann::json;
  static constexpr int64_t MaxDurationUs = 86400000000000LL;
  static constexpr size_t MaxStateJsonBytes = 16 * 1024;
  static constexpr size_t MaxArtB64Bytes = 360 * 1024;
  static constexpr size_t MaxArtJsonBytes = 384 * 1024;
  static constexpr const char *MprisPrefix = "org.mpris.MediaPlayer2.";

  std::function<void(Json)> publish, transmit;
  bool negotiated = false, enabled = true, available = false, readOnly = false;
  bool hasActive = false;
  uint64_t activeEpoch = 0, activeGeneration = 0, activeArtRevision = 0;
  std::string activePlayer, activeTrackId;
  int64_t activeLengthUs = 0;
  bool canControl = false, canPlay = false, canPause = false;
  bool canGoNext = false, canGoPrevious = false, canSeek = false;
  Json lastState;

  static bool validStringList(const Json &value, size_t maxItems, size_t maxBytes) {
    if (!value.is_array() || value.size() > maxItems) return false;
    for (const auto &item : value) {
      if (!item.is_string() || item.get_ref<const std::string &>().size() > maxBytes)
        return false;
    }
    return true;
  }

  Json decorateForGui(Json state) const {
    state["read_only"] = readOnly;
    if (readOnly) {
      state["can_control"] = false;
      state["can_play"] = false;
      state["can_pause"] = false;
      state["can_go_next"] = false;
      state["can_go_previous"] = false;
      state["can_seek"] = false;
    }
    return state;
  }

public:
  RemoteMedia(std::function<void(Json)> event, std::function<void(Json)> send)
      : publish(std::move(event)), transmit(std::move(send)) {}

  void reset() {
    hasActive = false;
    activeEpoch = 0;
    activeGeneration = 0;
    activeArtRevision = 0;
    activePlayer.clear();
    activeTrackId.clear();
    activeLengthUs = 0;
    canControl = canPlay = canPause = canGoNext = canGoPrevious = canSeek = false;
    lastState = Json();
    publish({{"type", "media_reset"}});
  }

  void settings(bool value) {
    enabled = value;
    if (!enabled) reset();
    if (negotiated)
      transmit({{"type", "media_controls_enabled"}, {"enabled", enabled && available}});
  }

  void inputReadOnly(bool value) {
    if (readOnly == value) return;
    readOnly = value;
    if (hasActive && negotiated && enabled && available && lastState.is_object())
      publish(decorateForGui(lastState));
  }

  void hello(const Json &message) {
    negotiated = message.value("media_controls_v1", false);
    reset();
    if (negotiated)
      transmit({{"type", "media_controls_enabled"}, {"enabled", enabled && available}});
  }

  bool receive(Json message) {
    std::string type = message.value("type", "");
    if (type == "media_reset") {
      reset();
      return true;
    }
    if (type != "media_state" && type != "media_art") return false;
    if (!negotiated || !enabled || !available) return true;

    if (type == "media_state") {
      if (message.dump().size() > MaxStateJsonBytes) return true;
      uint64_t epoch = message.value("epoch", uint64_t(0));
      uint64_t generation = message.value("generation", uint64_t(0));
      std::string player = message.value("player", "");
      std::string status = message.value("status", "");
      if (!epoch || !generation || player.size() > 255 ||
          player.rfind(MprisPrefix, 0) != 0)
        return true;
      if (status != "Playing" && status != "Paused" && status != "Stopped")
        return true;
      std::string identity = message.value("identity", "");
      std::string desktopEntry = message.value("desktop_entry", "");
      std::string title = message.value("title", "");
      std::string album = message.value("album", "");
      std::string trackId = message.value("track_id", "");
      if (identity.size() > 256 || desktopEntry.size() > 255 || title.size() > 512 ||
          album.size() > 512 || trackId.size() > 512)
        return true;
      Json artists = message.contains("artists") ? message["artists"] : Json::array();
      Json albumArtists = message.contains("album_artists") ? message["album_artists"] : Json::array();
      if (!validStringList(artists, 8, 256) || !validStringList(albumArtists, 8, 256))
        return true;
      int64_t lengthUs = message.value("length_us", int64_t(0));
      if (lengthUs < 0 || lengthUs > MaxDurationUs) lengthUs = 0;
      bool hasPosition = message.contains("position_us") && message["position_us"].is_number_integer();
      int64_t positionUs = hasPosition ? message["position_us"].get<int64_t>() : 0;
      if (positionUs < 0 || positionUs > MaxDurationUs) hasPosition = false;
      double rate = message.value("rate", 1.0);
      if (!(rate > 0.0 && rate <= 16.0)) rate = 1.0;

      bool ctrl = message.value("can_control", false);
      bool play = ctrl && message.value("can_play", false);
      bool pause = ctrl && message.value("can_pause", false);
      bool next = ctrl && message.value("can_go_next", false);
      bool prev = ctrl && message.value("can_go_previous", false);
      bool seek = ctrl && message.value("can_seek", false);
      uint64_t artRevision = message.value("art_revision", uint64_t(0));

      hasActive = true;
      activeEpoch = epoch;
      activeGeneration = generation;
      activePlayer = player;
      activeTrackId = trackId;
      activeLengthUs = lengthUs;
      activeArtRevision = artRevision;
      canControl = ctrl;
      canPlay = play;
      canPause = pause;
      canGoNext = next;
      canGoPrevious = prev;
      canSeek = seek;

      Json normalized = {
          {"type", "media_state"},
          {"epoch", epoch},
          {"player", player},
          {"generation", generation},
          {"identity", identity},
          {"desktop_entry", desktopEntry},
          {"status", status},
          {"title", title},
          {"artists", artists},
          {"album", album},
          {"album_artists", albumArtists},
          {"track_id", trackId},
          {"length_us", lengthUs},
          {"rate", rate},
          {"can_control", ctrl},
          {"can_play", play},
          {"can_pause", pause},
          {"can_go_next", next},
          {"can_go_previous", prev},
          {"can_seek", seek},
          {"art_revision", artRevision},
      };
      if (hasPosition) normalized["position_us"] = positionUs;
      lastState = normalized;
      publish(decorateForGui(std::move(normalized)));
      return true;
    }

    // media_art
    if (!hasActive || message.dump().size() > MaxArtJsonBytes) return true;
    if (message.value("epoch", uint64_t(0)) != activeEpoch ||
        message.value("generation", uint64_t(0)) != activeGeneration ||
        message.value("player", "") != activePlayer ||
        message.value("revision", uint64_t(0)) != activeArtRevision ||
        activeArtRevision == 0)
      return true;
    std::string mime = message.value("mime", "");
    if (mime != "image/png" && mime != "image/jpeg" && mime != "image/webp")
      return true;
    if (!message.contains("data") || !message["data"].is_string()) return true;
    const auto &encoded = message["data"].get_ref<const std::string &>();
    if (encoded.empty() || encoded.size() > MaxArtB64Bytes) return true;
    publish({{"type", "media_art"},
             {"epoch", activeEpoch},
             {"player", activePlayer},
             {"generation", activeGeneration},
             {"revision", activeArtRevision},
             {"mime", mime},
             {"data", encoded}});
    return true;
  }

  bool command(const Json &request) {
    std::string type = request.value("type", "");
    if (type == "media_controls_available") {
      available = request.value("available", false);
      if (!available) reset();
      if (negotiated)
        transmit({{"type", "media_controls_enabled"}, {"enabled", enabled && available}});
      return true;
    }
    if (type != "media_control") return false;
    if (!negotiated || !enabled || !available || readOnly || !hasActive || !canControl)
      return true;
    if (request.value("epoch", uint64_t(0)) != activeEpoch ||
        request.value("generation", uint64_t(0)) != activeGeneration ||
        request.value("player", "") != activePlayer)
      return true;
    std::string action = request.value("action", "");
    Json out = {{"type", "media_control"},
                {"epoch", activeEpoch},
                {"player", activePlayer},
                {"generation", activeGeneration},
                {"action", action}};
    if (action == "play") {
      if (!canPlay) return true;
    } else if (action == "pause") {
      if (!canPause) return true;
    } else if (action == "play_pause") {
      if (!(canPlay || canPause)) return true;
    } else if (action == "stop") {
      // Stop requires only canControl (already verified).
    } else if (action == "next") {
      if (!canGoNext) return true;
    } else if (action == "previous") {
      if (!canGoPrevious) return true;
    } else if (action == "set_position") {
      if (!canSeek || activeTrackId.empty()) return true;
      if (request.value("track_id", "") != activeTrackId) return true;
      if (!request.contains("position_us") || !request["position_us"].is_number_integer())
        return true;
      int64_t positionUs = request["position_us"].get<int64_t>();
      if (positionUs < 0 || (activeLengthUs > 0 && positionUs > activeLengthUs))
        return true;
      out["track_id"] = activeTrackId;
      out["position_us"] = positionUs;
    } else {
      return true;
    }
    transmit(std::move(out));
    return true;
  }
};
