#include "harness.hpp"

#include <blackbox/assets.hpp>

#include <string>
#include <string_view>

BB_TEST(ui_assets_are_embedded_and_typed) {
  const auto assets = bb::assets::all();
  CHECK_EQ(assets.size(), 3u);
  const bb::assets::Asset* html = bb::assets::find("/index.html");
  CHECK(html != nullptr);
  if (html != nullptr) {
    CHECK_EQ(html->content_type, std::string_view("text/html; charset=utf-8"));
    CHECK(html->bytes.size() > 10);
    const std::string text(reinterpret_cast<const char*>(html->bytes.data()),
                           html->bytes.size());
    CHECK(text.find("<!doctype html>") != std::string::npos);
  }
  const bb::assets::Asset* css = bb::assets::find("/app.css");
  CHECK(css != nullptr);
  if (css != nullptr) {
    CHECK(css->content_type.find("text/css") != std::string_view::npos);
  }
  const bb::assets::Asset* js = bb::assets::find("/app.js");
  CHECK(js != nullptr);
  if (js != nullptr) {
    CHECK(js->content_type.find("javascript") != std::string_view::npos);
  }
  CHECK(bb::assets::find("/missing.js") == nullptr);
  CHECK(bb::assets::find("index.html") == nullptr);
}
