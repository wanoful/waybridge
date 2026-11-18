#pragma once
#include <functional>
#include <map>
#include <string>
#include "third_party/json.hpp"

// Native worker is the session boundary. The GUI never sends arbitrary JSON
// back to Linux, nor can an old toast act on the next worker/session.
class RemoteNotifications {
  using Json = nlohmann::json;
  std::function<void(Json)> publish, transmit;
  std::map<uint32_t, uint64_t> active;
  std::string instance, binding;
  // Only the initialized GUI notification worker can open forwarding. A TCP
  // hello alone must never race ahead of native toast registration/readiness.
  bool negotiated = false, enabled = true, available = false;
  bool readOnly = false;
  void reset() {
    active.clear(); instance.clear(); binding.clear();
    publish({{"type","notification_reset"}});
  }
public:
  void inputReadOnly(bool value) { readOnly = value; }
  // The parent normalizes notification artwork and supplies a placeholder
  // when the source notification has no application icon.
  std::function<void(Json &)> decorate;
  RemoteNotifications(std::function<void(Json)> event, std::function<void(Json)> send)
    : publish(std::move(event)), transmit(std::move(send)) {}
  void settings(bool value) {
    enabled = value;
    if (!enabled) reset();
    if (negotiated) transmit({{"type","notifications_enabled"},{"enabled",enabled && available}});
  }
  void hello(const Json &message) {
    negotiated = message.value("notifications_v1",false);
    reset();
    if (negotiated) transmit({{"type","notifications_enabled"},{"enabled",enabled && available}});
    else publish({{"type","notification_status"},{"available",false},{"message","服务端未协商桌面通知；请更新服务端"}});
  }
  bool receive(Json message) {
    std::string type = message.value("type", "");
    if (type == "notification_status") { publish(std::move(message)); return true; }
    if (type == "notification_reset") { reset(); return true; }
    if (type != "notification" && type != "notification_close") return false;
    if (!negotiated || !enabled || !available) return true;
    auto source=message.value("instance", ""), connection=message.value("binding", "");
    if (source.size()!=32 || connection.size()!=32) return true;
    if (instance!=source || binding!=connection) { reset(); instance=source; binding=connection; }
    auto id=message.value("id",0u); auto revision=message.value("revision",uint64_t(0));
    if (!id || !revision) return true;
    if (type == "notification") {
      if (active.size()>=64 && !active.count(id)) return true;
      if (message.value("summary","").size()>1024 || message.value("body","").size()>8192 ||
          !message.contains("actions") || !message["actions"].is_array() || message["actions"].size()>5 ||
          message.dump().size()>60*1024) return true;
      active[id]=revision;
      if (decorate) decorate(message);
      publish(std::move(message));
    } else if (auto found=active.find(id); found!=active.end() && found->second==revision) {
      active.erase(found); publish(std::move(message));
    }
    return true;
  }
  bool command(const Json &request) {
    auto type=request.value("type", "");
    if (type=="notification_available") {
      available=request.value("available",false);
      if (!available) reset();
      if (negotiated) transmit({{"type","notifications_enabled"},{"enabled",enabled && available}});
      return true;
    }
    if (type!="notification_feedback") return false;
    auto id=request.value("id",0u); auto found=active.find(id);
    if (!negotiated || !enabled || !available || found==active.end() ||
        request.value("instance","")!=instance || request.value("binding","")!=binding ||
        request.value("revision",uint64_t(0))!=found->second) return true;
    Json message={{"type","notification_feedback"},{"instance",instance},{"binding",binding},
      {"id",id},{"revision",found->second}};
    if (request.contains("action") && request["action"].is_string() && request["action"].get_ref<const std::string &>().size()<=256) {
      if (readOnly) message["reason"]=2u;
      else message["action"]=request["action"];
    }
    else if (request.contains("reason") && request["reason"].is_number_unsigned() &&
             (request["reason"]==2 || request["reason"]==4)) message["reason"]=request["reason"];
    else return true;
    active.erase(found); transmit(std::move(message)); return true;
  }
};
