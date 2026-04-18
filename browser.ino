#include <M5Cardputer.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <HTTPClient.h>
#include <vector>
#include <algorithm>
#include <esp_heap_caps.h>

static const char* FROGFIND_BASE = "http://frogfind.com";
static const unsigned long WIFI_SCAN_INTERVAL_MS = 12000;

struct BrowserLink {
  String label;
  String url;
};

struct BrowserPage {
  String title;
  String text;
  std::vector<BrowserLink> links;
};

struct WifiNetworkItem {
  String ssid;
  int32_t rssi;
  wifi_auth_mode_t encryption;
};

enum AppMode {
  MODE_WIFI_LIST,
  MODE_WIFI_PASSWORD,
  MODE_BROWSER
};

std::vector<String> g_history;
BrowserPage g_page;
std::vector<String> g_wrappedLines;
std::vector<WifiNetworkItem> g_wifiNetworks;

AppMode g_mode = MODE_WIFI_LIST;
String g_input;
String g_status = "Start";
String g_currentUrl;
String g_selectedSsid;
int g_scrollLine = 0;
int g_wifiScroll = 0;
bool g_loading = false;
bool g_cursorVisible = true;
unsigned long g_lastBlinkMs = 0;
unsigned long g_lastWifiScanMs = 0;

String urlEncode(const String& value) {
  String encoded;
  char buf[4];
  for (size_t i = 0; i < value.length(); ++i) {
    const char c = value[i];
    if (isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else if (c == ' ') {
      encoded += '+';
    } else {
      snprintf(buf, sizeof(buf), "%%%02X", static_cast<unsigned char>(c));
      encoded += buf;
    }
  }
  return encoded;
}

String trimAndCollapseWhitespace(const String& input) {
  String out;
  out.reserve(input.length());
  bool previousWasSpace = false;
  for (size_t i = 0; i < input.length(); ++i) {
    const char c = input[i];
    const bool isSpace = c == ' ' || c == '\n' || c == '\r' || c == '\t';
    if (isSpace) {
      if (!previousWasSpace) out += ' ';
      previousWasSpace = true;
    } else {
      out += c;
      previousWasSpace = false;
    }
  }
  out.trim();
  return out;
}

String decodeHtmlEntities(String text) {
  text.replace("&amp;", "&");
  text.replace("&quot;", "\"");
  text.replace("&#39;", "'");
  text.replace("&apos;", "'");
  text.replace("&lt;", "<");
  text.replace("&gt;", ">");
  text.replace("&nbsp;", " ");
  text.replace("&#8217;", "'");
  text.replace("&#8211;", "-");
  text.replace("&#8212;", "-");
  text.replace("&#8230;", "...");
  return text;
}

String stripTags(const String& html) {
  String out;
  out.reserve(html.length());
  bool inTag = false;
  for (size_t i = 0; i < html.length(); ++i) {
    const char c = html[i];
    if (c == '<') {
      inTag = true;
      continue;
    }
    if (c == '>') {
      inTag = false;
      continue;
    }
    if (!inTag) out += c;
  }
  return decodeHtmlEntities(trimAndCollapseWhitespace(out));
}

String lowerCopy(String value) {
  value.toLowerCase();
  return value;
}

String absoluteUrl(const String& href, const String& baseUrl) {
  if (href.startsWith("http://") || href.startsWith("https://")) return href;
  if (href.startsWith("//")) return String("http:") + href;

  String base = baseUrl;
  int schemePos = base.indexOf("://");
  if (schemePos < 0) return href;
  int firstSlash = base.indexOf('/', schemePos + 3);
  String origin = firstSlash >= 0 ? base.substring(0, firstSlash) : base;

  if (href.startsWith("/")) {
    return origin + href;
  }

  int lastSlash = base.lastIndexOf('/');
  if (lastSlash < schemePos + 3) return origin + "/" + href;
  return base.substring(0, lastSlash + 1) + href;
}

String frogfindProxyUrl(String url) {
  String lower = lowerCopy(url);
  if (lower.startsWith("http://frogfind.com") || lower.startsWith("https://frogfind.com") ||
      lower.startsWith("http://www.frogfind.com") || lower.startsWith("https://www.frogfind.com")) {
    return url;
  }
  return String(FROGFIND_BASE) + "/read.php?a=" + urlEncode(url);
}

String extractTagContent(const String& html, const char* openTag, const char* closeTag) {
  String lower = lowerCopy(html);
  int start = lower.indexOf(openTag);
  if (start < 0) return "";
  start = lower.indexOf('>', start);
  if (start < 0) return "";
  ++start;
  int end = lower.indexOf(closeTag, start);
  if (end < 0) return "";
  return html.substring(start, end);
}

void appendNewline(String& text) {
  if (!text.isEmpty() && !text.endsWith("\n")) {
    text += "\n";
  }
}

void appendSpace(String& text) {
  if (!text.isEmpty() && !text.endsWith(" ") && !text.endsWith("\n")) {
    text += " ";
  }
}

BrowserPage parseHtmlPage(const String& html, const String& baseUrl) {
  BrowserPage page;
  page.title = stripTags(extractTagContent(html, "<title", "</title>"));
  if (page.title.isEmpty()) page.title = "FrogFind Browser";

  String lower = lowerCopy(html);
  String text;
  text.reserve(html.length());

  size_t i = 0;
  while (i < html.length()) {
    if (html[i] != '<') {
      text += html[i];
      ++i;
      continue;
    }

    int tagEnd = html.indexOf('>', i);
    if (tagEnd < 0) break;

    String rawTag = html.substring(i, tagEnd + 1);
    String tag = lower.substring(i, tagEnd + 1);

    String closeTag;
    if (tag.startsWith("<script")) closeTag = "</script>";
    else if (tag.startsWith("<style")) closeTag = "</style>";
    else if (tag.startsWith("<noscript")) closeTag = "</noscript>";
    else if (tag.startsWith("<svg")) closeTag = "</svg>";
    else if (tag.startsWith("<nav")) closeTag = "</nav>";
    else if (tag.startsWith("<footer")) closeTag = "</footer>";
    else if (tag.startsWith("<header")) closeTag = "</header>";
    else if (tag.startsWith("<aside")) closeTag = "</aside>";
    else if (tag.startsWith("<form")) closeTag = "</form>";
    else if (tag.startsWith("<button")) closeTag = "</button>";
    else if (tag.startsWith("<select")) closeTag = "</select>";
    else if (tag.startsWith("<textarea")) closeTag = "</textarea>";
    else if (tag.startsWith("<sup")) closeTag = "</sup>";

    if (!closeTag.isEmpty()) {
      int closePos = lower.indexOf(closeTag, tagEnd + 1);
      if (closePos < 0) {
        i = tagEnd + 1;
        continue;
      }
      i = closePos + closeTag.length();
      continue;
    }

    if (tag.startsWith("<br") || tag.startsWith("<p") || tag.startsWith("</p") ||
        tag.startsWith("<div") || tag.startsWith("</div") || tag.startsWith("<li") ||
        tag.startsWith("</li") || tag.startsWith("<h1") || tag.startsWith("</h1") ||
        tag.startsWith("<h2") || tag.startsWith("</h2") || tag.startsWith("<h3") ||
        tag.startsWith("</h3") || tag.startsWith("<h4") || tag.startsWith("</h4") ||
        tag.startsWith("<h5") || tag.startsWith("</h5") || tag.startsWith("<h6") ||
        tag.startsWith("</h6") || tag.startsWith("<article") || tag.startsWith("</article") ||
        tag.startsWith("<section") || tag.startsWith("</section") || tag.startsWith("<ul") ||
        tag.startsWith("</ul") || tag.startsWith("<ol") || tag.startsWith("</ol") ||
        tag.startsWith("<table") || tag.startsWith("</table") || tag.startsWith("<tr") ||
        tag.startsWith("</tr") || tag.startsWith("<blockquote") ||
        tag.startsWith("</blockquote")) {
      appendNewline(text);
      i = tagEnd + 1;
      continue;
    }

    if (tag.startsWith("<td") || tag.startsWith("</td") || tag.startsWith("<th") ||
        tag.startsWith("</th")) {
      appendSpace(text);
      i = tagEnd + 1;
      continue;
    }

    if (tag.startsWith("<a ")) {
      int hrefStart = tag.indexOf("href=");
      String href;
      if (hrefStart >= 0) {
        hrefStart += 5;
        if (hrefStart < rawTag.length()) {
          char quote = rawTag[hrefStart];
          if (quote == '"' || quote == '\'') {
            int hrefEnd = rawTag.indexOf(quote, hrefStart + 1);
            if (hrefEnd > hrefStart) {
              href = rawTag.substring(hrefStart + 1, hrefEnd);
            }
          } else {
            int hrefEnd = rawTag.indexOf(' ', hrefStart);
            if (hrefEnd < 0) hrefEnd = rawTag.indexOf('>', hrefStart);
            if (hrefEnd > hrefStart) href = rawTag.substring(hrefStart, hrefEnd);
          }
        }
      }

      int closePos = lower.indexOf("</a>", tagEnd + 1);
      if (closePos < 0) {
        i = tagEnd + 1;
        continue;
      }

      String anchorHtml = html.substring(tagEnd + 1, closePos);
      String label = stripTags(anchorHtml);
      href = decodeHtmlEntities(href);

      if (!label.isEmpty() && !href.isEmpty()) {
        BrowserLink link;
        link.label = label;
        link.url = absoluteUrl(href, baseUrl);
        page.links.push_back(link);

        appendNewline(text);
        text += "[";
        text += String(page.links.size());
        text += "] ";
        text += label;
        appendNewline(text);
      } else {
        text += stripTags(anchorHtml);
      }

      i = closePos + 4;
      continue;
    }

    i = tagEnd + 1;
  }

  page.text = decodeHtmlEntities(text);
  page.text.replace("\r", "");
  while (page.text.indexOf("\n\n\n") >= 0) {
    page.text.replace("\n\n\n", "\n\n");
  }
  page.text.trim();
  return page;
}

void wrapCurrentPage() {
  g_wrappedLines.clear();

  const int maxCols = 38;
  String source = g_page.text;
  source.replace("\t", " ");

  int start = 0;
  while (start < source.length()) {
    int nl = source.indexOf('\n', start);
    String paragraph;
    if (nl < 0) {
      paragraph = source.substring(start);
      start = source.length();
    } else {
      paragraph = source.substring(start, nl);
      start = nl + 1;
    }

    paragraph.trim();
    if (paragraph.isEmpty()) {
      g_wrappedLines.push_back("");
      continue;
    }

    while (paragraph.length() > maxCols) {
      int split = paragraph.lastIndexOf(' ', maxCols);
      if (split < 8) split = maxCols;
      String line = paragraph.substring(0, split);
      line.trim();
      g_wrappedLines.push_back(line);
      paragraph = paragraph.substring(split);
      paragraph.trim();
    }

    if (!paragraph.isEmpty()) {
      g_wrappedLines.push_back(paragraph);
    }
  }

  if (g_wrappedLines.empty()) {
    g_wrappedLines.push_back("(brak tresci)");
  }

  if (g_scrollLine >= static_cast<int>(g_wrappedLines.size())) {
    g_scrollLine = g_wrappedLines.size() - 1;
  }
  if (g_scrollLine < 0) g_scrollLine = 0;
}

String maskPassword(const String& input) {
  String out;
  for (size_t i = 0; i < input.length(); ++i) out += '*';
  return out;
}

String wifiSignalBars(int32_t rssi) {
  if (rssi >= -55) return "****";
  if (rssi >= -67) return "*** ";
  if (rssi >= -75) return "**  ";
  if (rssi >= -85) return "*   ";
  return ".   ";
}

int getBatteryPercent() {
  int value = M5Cardputer.Power.getBatteryLevel();
  if (value < 0) return 0;
  if (value > 100) return 100;
  return value;
}

size_t getHeapFreeBytes() {
  return heap_caps_get_free_size(MALLOC_CAP_8BIT);
}

size_t getHeapTotalBytes() {
  return heap_caps_get_total_size(MALLOC_CAP_8BIT);
}

String formatHeapUsageShort() {
  const size_t totalBytes = getHeapTotalBytes();
  const size_t freeBytes = getHeapFreeBytes();
  const size_t usedBytes = totalBytes > freeBytes ? totalBytes - freeBytes : 0;
  return "R" + String((usedBytes + 1023) / 1024) + "/" + String((totalBytes + 1023) / 1024) + "K";
}

void drawBatteryBar(int x, int y, int width, int height, int percent) {
  uint16_t fillColor = TFT_GREEN;
  if (percent <= 20) fillColor = TFT_RED;
  else if (percent <= 45) fillColor = TFT_YELLOW;

  const int capWidth = 2;
  const int innerWidth = width - 3;
  const int fillWidth = ((innerWidth - 2) * percent) / 100;

  M5Cardputer.Display.drawRect(x, y, innerWidth, height, TFT_WHITE);
  M5Cardputer.Display.fillRect(x + innerWidth, y + 2, capWidth, height - 4, TFT_WHITE);
  M5Cardputer.Display.fillRect(x + 1, y + 1, innerWidth - 2, height - 2, TFT_BLACK);
  if (fillWidth > 0) {
    M5Cardputer.Display.fillRect(x + 1, y + 1, fillWidth, height - 2, fillColor);
  }
}

String buildTopLinksLine() {
  if (g_page.links.empty()) return "Linki: brak";

  String line;
  const size_t maxLinks = std::min<size_t>(3, g_page.links.size());
  for (size_t i = 0; i < maxLinks; ++i) {
    String label = trimAndCollapseWhitespace(g_page.links[i].label);
    if (label.length() > 7) label = label.substring(0, 7);
    if (!line.isEmpty()) line += " ";
    line += "[";
    line += String(i + 1);
    line += "]";
    line += label;
  }

  if (g_page.links.size() > maxLinks) {
    line += " +";
    line += String(g_page.links.size() - maxLinks);
  }

  return line;
}

void setStatus(const String& status);
void drawUi();

void refreshWifiList() {
  setStatus("Skanuje Wi-Fi...");
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);

  const int found = WiFi.scanNetworks(false, true);
  g_wifiNetworks.clear();

  for (int i = 0; i < found; ++i) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;

    bool merged = false;
    for (size_t j = 0; j < g_wifiNetworks.size(); ++j) {
      if (g_wifiNetworks[j].ssid == ssid) {
        if (WiFi.RSSI(i) > g_wifiNetworks[j].rssi) {
          g_wifiNetworks[j].rssi = WiFi.RSSI(i);
          g_wifiNetworks[j].encryption = WiFi.encryptionType(i);
        }
        merged = true;
        break;
      }
    }

    if (!merged) {
      WifiNetworkItem item;
      item.ssid = ssid;
      item.rssi = WiFi.RSSI(i);
      item.encryption = WiFi.encryptionType(i);
      g_wifiNetworks.push_back(item);
    }
  }

  WiFi.scanDelete();

  std::sort(g_wifiNetworks.begin(), g_wifiNetworks.end(),
            [](const WifiNetworkItem& a, const WifiNetworkItem& b) {
              if (a.rssi != b.rssi) return a.rssi > b.rssi;
              return lowerCopy(a.ssid) < lowerCopy(b.ssid);
            });

  if (g_wifiScroll >= static_cast<int>(g_wifiNetworks.size())) {
    g_wifiScroll = std::max(0, static_cast<int>(g_wifiNetworks.size()) - 1);
  }

  g_lastWifiScanMs = millis();
  setStatus("Sieci: " + String(g_wifiNetworks.size()) + " | nr + ENTER");
}

void drawHeader(const String& title, uint16_t color) {
  const int width = M5Cardputer.Display.width();
  const int batteryPercent = getBatteryPercent();
  const String ramText = formatHeapUsageShort();

  M5Cardputer.Display.fillRect(0, 0, width, 18, color);
  M5Cardputer.Display.setCursor(4, 5);
  String shown = title;
  if (shown.length() > 16) shown = shown.substring(0, 16);
  M5Cardputer.Display.print(shown);

  M5Cardputer.Display.setCursor(width - 116, 5);
  M5Cardputer.Display.print(ramText);

  M5Cardputer.Display.setCursor(width - 58, 5);
  M5Cardputer.Display.print(String(batteryPercent) + "%");
  drawBatteryBar(width - 30, 5, 24, 8, batteryPercent);
}

void drawBrowserLinksMenu() {
  String line = buildTopLinksLine();
  if (line.length() > 38) line = line.substring(0, 38);
  M5Cardputer.Display.fillRect(0, 18, M5Cardputer.Display.width(), 10, TFT_DARKGREY);
  M5Cardputer.Display.setCursor(2, 20);
  M5Cardputer.Display.print(line);
}

void drawFooterInput(const String& rawInput, bool mask) {
  const int width = M5Cardputer.Display.width();
  const int height = M5Cardputer.Display.height();
  M5Cardputer.Display.fillRect(0, height - 18, width, 18, TFT_NAVY);
  M5Cardputer.Display.setCursor(2, height - 16);

  String shown = "> ";
  shown += mask ? maskPassword(rawInput) : rawInput;
  if (g_cursorVisible) shown += "_";
  if (shown.length() > 38) shown = shown.substring(shown.length() - 38);
  M5Cardputer.Display.print(shown);
}

void drawStatusLine() {
  const int height = M5Cardputer.Display.height();
  M5Cardputer.Display.fillRect(0, height - 28, M5Cardputer.Display.width(), 10, TFT_BLACK);
  M5Cardputer.Display.setCursor(2, height - 26);
  String status = g_status;
  if (status.length() > 38) status = status.substring(0, 38);
  M5Cardputer.Display.print(status);
}

void drawWifiList() {
  const int headerH = 18;
  const int footerH = 18;
  const int lineHeight = 9;
  const int bodyY = headerH + 2;
  const int visibleLines = (M5Cardputer.Display.height() - headerH - footerH - 14) / lineHeight;

  drawHeader("Wybierz Wi-Fi", TFT_DARKGREEN);

  if (g_wifiNetworks.empty()) {
    M5Cardputer.Display.setCursor(2, bodyY);
    M5Cardputer.Display.print("Brak sieci. Czekam...");
    M5Cardputer.Display.setCursor(2, bodyY + lineHeight * 2);
    M5Cardputer.Display.print("ENTER = odswiez");
    return;
  }

  for (int i = 0; i < visibleLines; ++i) {
    const int index = g_wifiScroll + i;
    if (index < 0 || index >= static_cast<int>(g_wifiNetworks.size())) break;

    const WifiNetworkItem& item = g_wifiNetworks[index];
    String line = String(index + 1) + ". ";
    line += wifiSignalBars(item.rssi);
    line += " ";
    line += item.ssid;
    if (item.encryption == WIFI_AUTH_OPEN) {
      line += " O";
    } else {
      line += " P";
    }
    if (line.length() > 38) line = line.substring(0, 38);

    M5Cardputer.Display.setCursor(2, bodyY + i * lineHeight);
    M5Cardputer.Display.print(line);
  }
}

void drawWifiPassword() {
  const int bodyY = 24;
  drawHeader("Haslo Wi-Fi", TFT_DARKCYAN);

  M5Cardputer.Display.setCursor(2, bodyY);
  M5Cardputer.Display.print("Siec:");
  M5Cardputer.Display.setCursor(2, bodyY + 10);
  String ssid = g_selectedSsid;
  if (ssid.length() > 36) ssid = ssid.substring(0, 36);
  M5Cardputer.Display.print(ssid);

  M5Cardputer.Display.setCursor(2, bodyY + 28);
  M5Cardputer.Display.print("Wpisz haslo i ENTER");
  M5Cardputer.Display.setCursor(2, bodyY + 38);
  M5Cardputer.Display.print("ESC/BACKSPACE do poprawy");
  M5Cardputer.Display.setCursor(2, bodyY + 48);
  M5Cardputer.Display.print("b + ENTER wraca");
}

void drawBrowser() {
  const int width = M5Cardputer.Display.width();
  const int height = M5Cardputer.Display.height();
  const int headerH = 28;
  const int footerH = 18;
  const int lineHeight = 9;
  const int visibleLines = (height - headerH - footerH - 14) / lineHeight;

  drawHeader(g_page.title.isEmpty() ? "FrogFind Browser" : g_page.title, TFT_DARKGREEN);
  drawBrowserLinksMenu();

  const int bodyY = headerH + 2;
  for (int i = 0; i < visibleLines; ++i) {
    const int lineIndex = g_scrollLine + i;
    if (lineIndex < 0 || lineIndex >= static_cast<int>(g_wrappedLines.size())) break;
    M5Cardputer.Display.setCursor(2, bodyY + i * lineHeight);
    M5Cardputer.Display.print(g_wrappedLines[lineIndex]);
  }
}

void drawUi() {
  M5Cardputer.Display.startWrite();
  M5Cardputer.Display.fillScreen(TFT_BLACK);
  M5Cardputer.Display.setTextFont(1);
  M5Cardputer.Display.setTextSize(1);
  M5Cardputer.Display.setTextColor(TFT_WHITE, TFT_BLACK);

  if (g_mode == MODE_WIFI_LIST) {
    drawWifiList();
    drawStatusLine();
    drawFooterInput(g_input, false);
  } else if (g_mode == MODE_WIFI_PASSWORD) {
    drawWifiPassword();
    drawStatusLine();
    drawFooterInput(g_input, true);
  } else {
    drawBrowser();
    drawStatusLine();
    drawFooterInput(g_input, false);
  }

  M5Cardputer.Display.endWrite();
}

void setStatus(const String& status) {
  g_status = status;
  drawUi();
}

bool connectWifi(const String& ssid, const String& password) {
  setStatus("Laczenie z " + ssid);
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true, false);
  delay(100);
  WiFi.begin(ssid.c_str(), password.c_str());

  const unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    M5Cardputer.update();
    if (millis() - g_lastBlinkMs > 400) {
      g_lastBlinkMs = millis();
      g_cursorVisible = !g_cursorVisible;
    }
    drawUi();
    delay(50);
  }

  if (WiFi.status() == WL_CONNECTED) {
    setStatus("Wi-Fi OK: " + WiFi.localIP().toString());
    return true;
  }

  WiFi.disconnect(true, false);
  setStatus("Nie udalo sie polaczyc");
  return false;
}

String httpGet(const String& url) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);

  if (!http.begin(client, url)) {
    setStatus("Nie moge otworzyc URL");
    return "";
  }

  const int code = http.GET();
  if (code <= 0) {
    setStatus("HTTP error: " + String(code));
    http.end();
    return "";
  }

  if (code != HTTP_CODE_OK) {
    setStatus("HTTP status: " + String(code));
    http.end();
    return "";
  }

  String body = http.getString();
  http.end();
  return body;
}

void openUrl(const String& url, bool pushHistory = true) {
  g_loading = true;
  if (pushHistory && !g_currentUrl.isEmpty()) {
    g_history.push_back(g_currentUrl);
  }

  g_currentUrl = url;
  setStatus("Pobieram...");

  String body = httpGet(url);
  if (body.isEmpty()) {
    g_loading = false;
    drawUi();
    return;
  }

  g_page = parseHtmlPage(body, url);
  g_scrollLine = 0;
  wrapCurrentPage();
  g_loading = false;
  setStatus("Linki: " + String(g_page.links.size()) + " | wpisz numer");
}

void searchFrogFind(const String& query) {
  String url = String(FROGFIND_BASE) + "/?q=" + urlEncode(query);
  openUrl(url);
}

void goBack() {
  if (g_history.empty()) {
    setStatus("Brak historii");
    return;
  }
  String previous = g_history.back();
  g_history.pop_back();
  openUrl(previous, false);
}

bool isNumberOnly(const String& text) {
  if (text.isEmpty()) return false;
  for (size_t i = 0; i < text.length(); ++i) {
    if (!isDigit(static_cast<unsigned char>(text[i]))) return false;
  }
  return true;
}

void showWelcomePage() {
  g_page.title = "FrogFind Browser";
  g_page.text =
    "Lekka przegladarka tekstowa dla M5Cardputer.\n\n"
    "Start:\n"
    "1. Wybierz Wi-Fi z listy\n"
    "2. Wpisz haslo\n"
    "3. Wpisz zapytanie i ENTER\n\n"
    "Przyklady:\n"
    "wikipedia polska\n"
    "retro computers\n"
    "u wikipedia.org\n\n"
    "Obsluga:\n"
    "numer + ENTER otwiera link\n"
    "b wraca\n"
    "r odswieza\n"
    "; oraz . przewijaja";
  g_page.links.clear();
  wrapCurrentPage();
}

void handleBrowserCommand(String cmd) {
  cmd.trim();
  if (cmd.isEmpty()) return;

  if (cmd == "b" || cmd == "back") {
    goBack();
    return;
  }

  if (cmd == "r" || cmd == "reload") {
    if (g_currentUrl.isEmpty()) {
      setStatus("Brak strony do odswiezenia");
      return;
    }
    openUrl(g_currentUrl, false);
    return;
  }

  if (cmd == "help") {
    g_page.title = "Pomoc";
    g_page.text =
      "Wpisz tekst i ENTER = szukaj w FrogFind\n\n"
      "Wpisz numer i ENTER = otworz link\n\n"
      "b = wstecz\n"
      "r = odswiez\n"
      "u <url> = otworz adres przez FrogFind\n"
      "; oraz . = przewijanie\n"
      "G0 = szybkie odswiezenie";
    g_page.links.clear();
    wrapCurrentPage();
    setStatus("Pomoc");
    return;
  }

  if (cmd.startsWith("u ")) {
    String rawUrl = cmd.substring(2);
    rawUrl.trim();
    if (!rawUrl.startsWith("http://") && !rawUrl.startsWith("https://")) {
      rawUrl = "https://" + rawUrl;
    }
    openUrl(frogfindProxyUrl(rawUrl));
    return;
  }

  if (isNumberOnly(cmd)) {
    int index = cmd.toInt();
    if (index <= 0 || index > static_cast<int>(g_page.links.size())) {
      setStatus("Nie ma takiego linku");
      return;
    }
    BrowserLink link = g_page.links[index - 1];
    openUrl(frogfindProxyUrl(link.url));
    return;
  }

  searchFrogFind(cmd);
}

void handleWifiListCommand(String cmd) {
  cmd.trim();
  if (cmd.isEmpty()) {
    refreshWifiList();
    return;
  }

  if (cmd == "r" || cmd == "scan") {
    refreshWifiList();
    return;
  }

  if (!isNumberOnly(cmd)) {
    setStatus("Wpisz numer sieci");
    return;
  }

  int index = cmd.toInt() - 1;
  if (index < 0 || index >= static_cast<int>(g_wifiNetworks.size())) {
    setStatus("Brak takiej sieci");
    return;
  }

  g_selectedSsid = g_wifiNetworks[index].ssid;
  g_input = "";
  g_mode = MODE_WIFI_PASSWORD;
  if (g_wifiNetworks[index].encryption == WIFI_AUTH_OPEN) {
    if (connectWifi(g_selectedSsid, "")) {
      g_mode = MODE_BROWSER;
      showWelcomePage();
      searchFrogFind("wikipedia");
    } else {
      g_mode = MODE_WIFI_LIST;
      refreshWifiList();
    }
    return;
  }

  setStatus("Haslo dla: " + g_selectedSsid);
}

void handleWifiPasswordCommand(String cmd) {
  if (cmd == "b" || cmd == "back") {
    g_input = "";
    g_mode = MODE_WIFI_LIST;
    setStatus("Wrocilem do listy Wi-Fi");
    return;
  }

  if (connectWifi(g_selectedSsid, cmd)) {
    g_mode = MODE_BROWSER;
    g_input = "";
    showWelcomePage();
    searchFrogFind("wikipedia");
  }
}

void handleKeyboardInputChars(const std::vector<char>& chars, bool allowScrollKeys) {
  for (char c : chars) {
    if (allowScrollKeys && (c == ';' || c == ',')) {
      if (g_mode == MODE_BROWSER) {
        g_scrollLine -= 3;
        if (g_scrollLine < 0) g_scrollLine = 0;
      } else if (g_mode == MODE_WIFI_LIST) {
        g_wifiScroll -= 3;
        if (g_wifiScroll < 0) g_wifiScroll = 0;
      }
      drawUi();
      continue;
    }

    if (allowScrollKeys && c == '.') {
      if (g_mode == MODE_BROWSER) {
        g_scrollLine += 3;
        if (g_scrollLine >= static_cast<int>(g_wrappedLines.size())) {
          g_scrollLine = std::max(0, static_cast<int>(g_wrappedLines.size()) - 1);
        }
      } else if (g_mode == MODE_WIFI_LIST) {
        g_wifiScroll += 3;
        if (g_wifiScroll >= static_cast<int>(g_wifiNetworks.size())) {
          g_wifiScroll = std::max(0, static_cast<int>(g_wifiNetworks.size()) - 1);
        }
      }
      drawUi();
      continue;
    }

    if (isPrintable(static_cast<unsigned char>(c))) {
      g_input += c;
    }
  }
}

void handleKeyboard() {
  if (!M5Cardputer.Keyboard.isChange() || !M5Cardputer.Keyboard.isPressed()) return;

  Keyboard_Class::KeysState state = M5Cardputer.Keyboard.keysState();
  handleKeyboardInputChars(state.word, true);

  if (state.del && !g_input.isEmpty()) {
    g_input.remove(g_input.length() - 1);
  }

  if (state.enter) {
    String cmd = g_input;
    g_input = "";

    if (g_mode == MODE_WIFI_LIST) {
      handleWifiListCommand(cmd);
    } else if (g_mode == MODE_WIFI_PASSWORD) {
      handleWifiPasswordCommand(cmd);
    } else {
      handleBrowserCommand(cmd);
    }
  }

  drawUi();
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setTextFont(1);
  M5Cardputer.Display.setTextSize(1);

  showWelcomePage();
  setStatus("Start skanowania Wi-Fi");
  refreshWifiList();
}

void loop() {
  M5Cardputer.update();

  if (g_mode == MODE_WIFI_LIST && millis() - g_lastWifiScanMs > WIFI_SCAN_INTERVAL_MS) {
    refreshWifiList();
  }

  if (g_mode == MODE_BROWSER && M5Cardputer.BtnA.wasPressed() && !g_currentUrl.isEmpty()) {
    openUrl(g_currentUrl, false);
  }

  handleKeyboard();

  if (millis() - g_lastBlinkMs > 400) {
    g_lastBlinkMs = millis();
    g_cursorVisible = !g_cursorVisible;
    drawUi();
  }
}
