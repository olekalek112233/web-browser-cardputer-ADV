#include <M5Cardputer.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <SD.h>

enum State{
  WIFI_SELECT,
  WIFI_PASS,
  URL_INPUT,
  VIEW_PAGE
};

State state=WIFI_SELECT;

int wifiIndex=0;
int wifiCount=0;

String inputText="";
String pageText="";
String currentURL="";

int scrollY=0;

bool crtMode=false;

String links[20];
int linkCount=0;

unsigned long lastKeyTime=0;
unsigned long lastDeleteTime=0;

const int keyDelay=120;
const int deleteDelay=250;

bool screenDirty=true;

// ---------- KEYBOARD ----------

bool canType(){
  if(millis()-lastKeyTime>keyDelay){
    lastKeyTime=millis();
    return true;
  }
  return false;
}

char readKey(){
  auto ks=M5Cardputer.Keyboard.keysState();
  if(ks.word.size()>0)
    return ks.word[0];
  return 0;
}

// ---------- STATUS BAR ----------

void drawStatusBar(){

  int ram=ESP.getFreeHeap()/1024;
  int bat=M5.Power.getBatteryLevel();

  M5.Display.fillRect(0,0,240,12,DARKGREY);
  M5.Display.setCursor(2,2);

  if(WiFi.status()==WL_CONNECTED)
    M5.Display.print("WiFi OK ");
  else
    M5.Display.print("WiFi -- ");

  M5.Display.printf("| RAM %dk | BAT %d%%",ram,bat);
}

// ---------- FILES ----------

void saveHistory(String url){
  File f=SD.open("/history.txt",FILE_APPEND);
  if(f){f.println(url);f.close();}
}

void saveCache(){
  File f=SD.open("/cache.txt","w");
  if(f){f.print(pageText);f.close();}
}

// ---------- URL ----------

String smartURL(String input){

  if(!input.startsWith("http")){
    input.replace(" ","+");
    return "https://frogfind.com/?q="+input;
  }

  return input;
}

// ---------- HTML CLEAN ----------

String cleanHTML(String html){

  String out="";
  bool tag=false;

  for(char c:html){

    if(c=='<'){tag=true;continue;}
    if(c=='>'){tag=false;continue;}

    if(!tag) out+=c;
  }

  return out;
}

// ---------- LOAD PAGE ----------

void loadPage(String url){

  HTTPClient http;

  url=smartURL(url);

  M5.Display.setCursor(20,60);
  M5.Display.println("Loading...");

  http.begin(url);
  int code=http.GET();

  if(code>0){

    String html=http.getString();

    pageText=cleanHTML(html);

    currentURL=url;

    saveHistory(url);
    saveCache();

    scrollY=0;

    state=VIEW_PAGE;
    screenDirty=true;
  }

  http.end();
}

// ---------- WIFI SELECT ----------

void drawWiFiSelect(){

  auto key=M5Cardputer.Keyboard.keysState();

  if(key.word.size()){
    char k=key.word[0];

    if(k=='w'){wifiIndex--;screenDirty=true;}
    if(k=='s'){wifiIndex++;screenDirty=true;}
  }

  if(wifiIndex<0) wifiIndex=0;
  if(wifiIndex>=wifiCount) wifiIndex=wifiCount-1;

  if(key.enter){
    inputText="";
    state=WIFI_PASS;
    screenDirty=true;
  }

  if(!screenDirty) return;

  M5.Display.fillScreen(BLACK);
  drawStatusBar();

  M5.Display.setCursor(0,14);
  M5.Display.println("Select WiFi (W/S)");

  for(int i=0;i<wifiCount;i++){

    if(i==wifiIndex)
      M5.Display.print("> ");
    else
      M5.Display.print("  ");

    M5.Display.println(WiFi.SSID(i));
  }

  screenDirty=false;
}

// ---------- WIFI PASS ----------

void drawWiFiPass(){

  auto key=M5Cardputer.Keyboard.keysState();
  char k=readKey();

  if(k && canType()){
    inputText+=k;
    screenDirty=true;
  }

  if(key.del && inputText.length()){

    if(millis()-lastDeleteTime>deleteDelay){
      inputText.remove(inputText.length()-1);
      lastDeleteTime=millis();
      screenDirty=true;
    }
  }

  if(key.enter){

    WiFi.disconnect(true);
    delay(200);

    WiFi.begin(WiFi.SSID(wifiIndex).c_str(),inputText.c_str());

    unsigned long start=millis();

    while(WiFi.status()!=WL_CONNECTED){

      if(millis()-start>10000){

        state=WIFI_SELECT;
        screenDirty=true;
        return;
      }

      delay(300);
    }

    state=URL_INPUT;
    screenDirty=true;
  }

  if(!screenDirty) return;

  M5.Display.fillScreen(BLACK);
  drawStatusBar();

  M5.Display.setCursor(0,14);
  M5.Display.println("Password:");

  M5.Display.print(inputText);
  M5.Display.print("_");

  screenDirty=false;
}

// ---------- URL INPUT ----------

void drawURLInput(){

  auto key=M5Cardputer.Keyboard.keysState();
  char k=readKey();

  if(k && canType()){
    inputText+=k;
    screenDirty=true;
  }

  if(key.del && inputText.length()){

    if(millis()-lastDeleteTime>deleteDelay){
      inputText.remove(inputText.length()-1);
      lastDeleteTime=millis();
      screenDirty=true;
    }
  }

  if(key.enter){
    loadPage(inputText);
    inputText="";
    screenDirty=true;
  }

  if(!screenDirty) return;

  M5.Display.fillScreen(BLACK);
  drawStatusBar();

  M5.Display.setCursor(0,14);
  M5.Display.println("Enter URL/Search:");

  M5.Display.print(inputText);
  M5.Display.print("_");

  screenDirty=false;
}

// ---------- PAGE VIEW ----------

void drawPage(){

  auto key=M5Cardputer.Keyboard.keysState();

  if(key.word.size()){

    char k=key.word[0];

    if(k=='s'){scrollY+=20;screenDirty=true;}
    if(k=='w'){scrollY-=20;screenDirty=true;}
  }

  if(scrollY<0) scrollY=0;

  if(key.tab){
    state=URL_INPUT;
    screenDirty=true;
  }

  if(!screenDirty) return;

  M5.Display.fillScreen(BLACK);
  drawStatusBar();

  M5.Display.setCursor(0,14-scrollY);
  M5.Display.print(pageText);

  screenDirty=false;
}

// ---------- SETUP ----------

void setup(){

  auto cfg=M5.config();
  M5Cardputer.begin(cfg);

  M5.Display.setTextSize(1);

  SD.begin(GPIO_NUM_4);

  WiFi.mode(WIFI_STA);

  wifiCount=WiFi.scanNetworks();
}

// ---------- LOOP ----------

void loop(){

  M5Cardputer.update();

  switch(state){

    case WIFI_SELECT:
      drawWiFiSelect();
      break;

    case WIFI_PASS:
      drawWiFiPass();
      break;

    case URL_INPUT:
      drawURLInput();
      break;

    case VIEW_PAGE:
      drawPage();
      break;
  }

  delay(20);
}