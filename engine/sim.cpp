// Command-line front end of the Sticky core (engine/core): one command per
// line on stdin, one JSON reply per line. The web wizard's preview scripts
// drive it; the Mac simulator links the same core directly.

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "sticky_core.h"

static std::string json_str(const std::string &s) {
  std::string o = "\"";
  for (char c : s) {
    if (c == '"' || c == '\\') o += '\\';
    o += c;
  }
  return o + "\"";
}

static void collect(void *ctx, const char *call) { static_cast<std::vector<std::string> *>(ctx)->push_back(call); }

static std::string calls_json(const std::vector<std::string> &calls) {
  std::string o = "[";
  for (size_t i = 0; i < calls.size(); i++) o += (i ? "," : "") + json_str(calls[i]);
  return o + "]";
}

static void write_pbm(const std::vector<uint8_t> &px, const std::string &path) {
  std::ofstream out(path, std::ios::binary);
  out << "P4\n" << SC_WIDTH << " " << SC_HEIGHT << "\n";
  std::vector<uint8_t> row((SC_WIDTH + 7) / 8);
  for (int y = 0; y < SC_HEIGHT; y++) {
    std::fill(row.begin(), row.end(), 0);
    for (int x = 0; x < SC_WIDTH; x++)
      if (!px[y * SC_WIDTH + x]) row[x / 8] |= 0x80 >> (x % 8);  // PBM: 1 = black
    out.write((const char *) row.data(), (std::streamsize) row.size());
  }
}

int main(int argc, char **argv) {
  const std::string font_dir = argc > 1 ? argv[1] : "engine/build/fonts/inter";
  for (int i = 0; i < SC_FONT_COUNT; i++) {
    if (sc_font_load_fnt(i, (font_dir + "/" + sc_font_name(i) + ".fnt").c_str()) != 0) {
      std::cerr << "cannot load font " << sc_font_name(i) << " from " << font_dir << "\n";
      return 1;
    }
  }
  std::vector<uint8_t> px(SC_WIDTH * SC_HEIGHT);
  std::string line;
  while (std::getline(std::cin, line)) {
    std::istringstream ss(line);
    std::string cmd;
    ss >> cmd;
    std::string reply = "{\"ok\":true}";
    if (cmd == "set") {
      std::string key, value;
      ss >> key;
      std::getline(ss, value);
      if (!value.empty() && value[0] == ' ') value.erase(0, 1);
      for (size_t i = value.find("\\n"); i != std::string::npos; i = value.find("\\n", i + 1))
        value.replace(i, 2, "\n");   // one command per line: "\n" stands for a line break
      sc_set(key.c_str(), value.c_str());
    } else if (cmd == "labels") {  // labels <room>|<light 1>|<light 2>|<app button>|<player>
      std::string rest, part;
      std::getline(ss, rest);
      std::vector<std::string> parts;
      std::stringstream ps(rest.substr(rest.find_first_not_of(' ') == std::string::npos ? 0 : rest.find_first_not_of(' ')));
      while (std::getline(ps, part, '|')) parts.push_back(part);
      while (parts.size() < 5) parts.push_back("");
      sc_set_labels(parts[0].c_str(), parts[1].c_str(), parts[2].c_str(), parts[3].c_str(), parts[4].c_str());
    } else if (cmd == "poster") {  // poster <raw gray file> <w> <h> | poster none
      std::string path;
      int w = 0, h = 0;
      ss >> path >> w >> h;
      if (path == "none") {
        sc_set_poster(nullptr, 0, 0);
      } else {
        std::ifstream in(path, std::ios::binary);
        std::vector<uint8_t> d((size_t) w * h);
        in.read((char *) d.data(), (std::streamsize) d.size());
        sc_set_poster(d.data(), w, h);
      }
    } else if (cmd == "tap") {
      int x, y;
      long long now;
      ss >> x >> y >> now;
      std::vector<std::string> calls;
      const int a = sc_tap(x, y, now, collect, &calls);
      reply = std::string("{\"action\":\"") + sc_action_name(a) + "\",\"calls\":" + calls_json(calls) + "}";
    } else if (cmd == "touch") {   // touch <raw x> <raw y> <now>
      int x, y;
      long long now;
      ss >> x >> y >> now;
      std::vector<std::string> calls;
      const int a = sc_touch(x, y, now, collect, &calls);
      reply = std::string("{\"action\":\"") + sc_action_name(a) + "\",\"calls\":" + calls_json(calls) + "}";
    } else if (cmd == "release") {
      sc_release();
    } else if (cmd == "hold") {    // one hold-to-repeat tick
      std::vector<std::string> calls;
      sc_hold_tick(collect, &calls);
      reply = "{\"calls\":" + calls_json(calls) + "}";
    } else if (cmd == "calibrate") {
      sc_calibrate();
    } else if (cmd == "touchcal") {
      float v[6];
      const int ok = sc_touch_cal(v);
      char b[200];
      snprintf(b, sizeof b, "{\"valid\":%d,\"map\":[%.4f,%.4f,%.1f,%.4f,%.4f,%.1f]}", ok, v[0], v[1], v[2], v[3], v[4], v[5]);
      reply = b;
    } else if (cmd == "button") {
      std::string what;
      long long now;
      ss >> what >> now;
      std::vector<std::string> calls;
      sc_button(what == "up" ? SC_BUTTON_UP : what == "down" ? SC_BUTTON_DOWN : SC_BUTTON_AI, now, collect, &calls);
      reply = "{\"action\":\"" + what + "\",\"calls\":" + calls_json(calls) + "}";
    } else if (cmd == "tick") {
      int valid, hour, minute;
      long long now;
      ss >> valid >> hour >> minute >> now;
      sc_tick(valid, hour, minute, now);
    } else if (cmd == "frame") {
      std::string path;
      int valid, hour, minute;
      long long now;
      ss >> path >> valid >> hour >> minute >> now;
      const int r = sc_frame(valid, hour, minute, now, px.data());
      if (r != SC_REFRESH_NONE) write_pbm(px, path);
      reply = std::string("{\"refresh\":\"") + (r == SC_REFRESH_FULL ? "full" : r == SC_REFRESH_PARTIAL ? "partial" : "none") + "\"}";
    } else if (cmd == "state") {
      const int m = sc_mode();
      reply = std::string("{\"mode\":\"") + (m == SC_MODE_PLAYING ? "playing" : m == SC_MODE_IDLE ? "idle" : "off") +
              "\",\"page\":\"" + (sc_page() == SC_PAGE_NOW ? "now" : "remote") +
              "\",\"sleeping\":" + (sc_sleeping() ? "true" : "false") + "}";
    } else if (cmd == "quit") {
      break;
    } else {
      reply = "{\"error\":" + json_str("unknown command: " + cmd) + "}";
    }
    std::cout << reply << std::endl;
  }
  return 0;
}
