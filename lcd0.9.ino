#include <WiFiManager.h>
#include <HTTPClient.h>
#include "nvs_flash.h"
#include <WiFiClient.h>
#include <TFT_eSPI.h>
#include <SPI.h>
#include <esp_adc_cal.h>
#include <WiFi.h>
#include "USB.h"
#include <TFT_eSPI.h> // 包含 TFT_eSPI 函式庫
#include "wifiIcon.h"
#include "batteryIcon.h"
#include <PMS.h> 
#include <SoftwareSerial.h> 

// 將Arduino Pin2設定為RX, Pin3設定為TX
SoftwareSerial SerialA(2, 3); 
#define HWSerial Serial
USBCDC USBSerial;

// 初始空氣偵測
PMS pms(SerialA);
PMS::DATA data;
String pm25Value = "0";

// api 服務
const char* apiUrl = "http://10.0.0.50:8000";
// 清除wifi 設定宣告
unsigned long buttonPressedTime = 0;
bool buttonPressed = false;

// 初始AP 
WiFiManager wifiManager;
const char* autoConnect = ".AirGuardian";
// WiFi設定頁增加額外的欄位
WiFiManagerParameter custom_text1("<p style=\"color:blue\">輸入Email為會員帳號</p><p style=\"color:red\">請確認email 為可收信, 否則Line 通知無法驗證</p>");
WiFiManagerParameter custom_email("email", "email", "", 255, "type='email' pattern='[a-z0-9._%+\-]+@[a-z0-9.\-]+\.[a-z]{2,}$'");

// ESP
const uint64_t chipid = ESP.getEfuseMac(); // 取得MAC位址的低6位元組
String chipID;

// LCD 顯示
TFT_eSPI tft = TFT_eSPI();
TFT_eSprite sprite = TFT_eSprite(&tft);
const int TFT_BL 45 // LCD 螢幕背光
// 時間顯示lcd 宣告設定值
char timeHour[3];
char timeMin[3];
char timeSec[3];
unsigned long targetTime = 0;
unsigned long currTime = 0;
int period = 900; // 時間更新週期

// 抓取電池設定值
const int PIN_BAT_VOLT 1; //電池數值角位
uint16_t adc_value = 0;
// 時間同步
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600 * 8;   // +8 GTM
const int   daylightOffset_sec = 3600;
int  checkTimeLoop = 0; // 初始檢查時間重新嘗試預設

// 設定定時器初始值
int timerSetScreen = 600000000; // 螢幕休眠 1分鐘（60000000微秒）
int timerSetOther =  10000000; // 其他 30秒（30000000微秒）
hw_timer_t * timer0 = NULL;
hw_timer_t * timer1 = NULL;
volatile bool timer0Elapsed = false;
volatile bool timer1Elapsed = false;
void IRAM_ATTR onTimer0() {
  timer0Elapsed = true;
}
void IRAM_ATTR onTimer1() {
  timer1Elapsed = true;
}

// wifi 訊號
int rssi = 0;
String oldWifiRSSI;
// 按鈕循環初始值
const int buttonPin = 0;    // 定義按鈕連接的引腳
unsigned long buttonTimer = 0; // 定義初始秒數
unsigned long longPressTimeoutMillis = 10000; // 定義長按秒數10秒
boolean isButtonActive = false; // 按鈕預設沒被點擊
boolean longPressActive = false;  // 按鈕長按是否啟用
int buttonCounter = 0; // 按鈕計數器
int backCount = 4; // +1 到設定次數返回為0 
int displayIndexSwitch = 3; // 顯示被切換畫面 switch 預設，設定3為 default

void setup() {
  USBSerial.begin(115200);
  Serial.begin(9600);
  SerialA.begin(9600);

  pinMode(45, OUTPUT);
  digitalWrite(45, HIGH);  

  // 初始化定時器
  timerSetup();
  // wifi 連線管理頁設定
  wifiConnectSetup();
  // 初始化tft 顯示
  displaySetup();
  // 同步時間
  ntpSetup();  

  // 載入初始pm2.5
  if (pms.read(data)) {
    while(pm25Value == "0") {
      pms.read(data);
      pm25Data();    
    }
  }
}

/**
* 初始化定時器
**/
void timerSetup() {
  // 初始化並啟動定時器0
  timer0 = timerBegin(0, 80, true); // 使用第一個定時器，預分頻80
  timerAttachInterrupt(timer0, &onTimer0, true);
  timerAlarmWrite(timer0, timerSetScreen, false);
  timerAlarmEnable(timer0);

  // 初始化並啟動定時器1
  timer1 = timerBegin(1, 80, true); // 使用第二個定時器，預分頻80
  timerAttachInterrupt(timer1, &onTimer1, true);
  timerAlarmWrite(timer1, timerSetOther, true); 
  timerAlarmEnable(timer1);
}

/**
* 顯示wifi 強弱
**/
void wifiRssiShow() {
    sprite.fillSprite(TFT_BLACK); // 點擊按鈕清除畫面
    sprite.drawString("WiFi RSSI: " + String(rssi), 0, 0, 2);

    int arrayIndex = 0; // 無訊號

    if (rssi >= -45) {
      arrayIndex = 4;
    } else if (rssi >= -65) {
      arrayIndex = 3;
    } else if (rssi >= -85) {
      arrayIndex = 2;
    } else if (rssi >= -90) {
      arrayIndex = 1;
    }

    sprite.drawBitmap(40, 20, wifi_icon_array[arrayIndex], 64, 64, 0xffff);    
}


/**
* 運行第二定時器
* 電池量
* PM2.5 數值
**/
void timerB() {
  if (timer1Elapsed) {
    rssi = WiFi.RSSI();
    pm25Data();
    batteryData();
    timer1Elapsed = false;  
  }
}

/**
* 顯示屏初始設定
**/
void displaySetup() {
    tft.init();
    tft.setRotation(1);
    tft.fillScreen(TFT_BLACK);

    sprite.createSprite(160, 80); // 顯示屏大小 寬、高
    sprite.setSwapBytes(true);
    sprite.setTextColor(TFT_WHITE, TFT_BLACK); // 顯示字體與字體背景色 白字、黑底

    targetTime = millis() + 1000;
}

/**
* wifi 連線管理頁設定
**/
void wifiConnectSetup() {
      if (getNavFlashWifiSsid() != "") {
        WiFi.begin();
        // 等待 WiFi 啟動
        while (WiFi.status() == WL_DISCONNECTED) {
          delay(100);
        }        
      } else {
        wifiManager.setConnectTimeout(180); // 設定連線超時時間為180秒
        wifiManager.setDebugOutput(false); // 停用輸出除錯訊息
        wifiManager.setScanDispPerc(true); // 設定顯示wifi 訊號百分比
        wifiManager.setMinimumSignalQuality(10); // 設定最低訊號強度百分比
        wifiManager.addParameter(&custom_text1);  // 自訂顯示文字
        wifiManager.addParameter(&custom_email);  // email欄位
        //自訂存取點 IP 配置
        wifiManager.setAPStaticIPConfig(IPAddress(10,0,1,1), IPAddress(10,0,1,1), IPAddress(255,255,255,0));
        wifiManager.autoConnect(autoConnect);
      }

      // 獲取連線後訊號強度
      rssi = WiFi.RSSI();

    if (rssi != 0) {
      // wifiRssiShow();
      Serial.println("已連接到WiFi");
    }
}

/**
* 抓取 nav flash 已儲存wifi ssid 名稱
* 無資料時為空
**/
String getNavFlashWifiSsid() {
   // 初始化 WiFi，設定為 STA 模式
  WiFi.mode(WIFI_STA);
  wifi_config_t wifiConfig;

  // 取得目前的 WiFi 配置
  if (esp_wifi_get_config(WIFI_IF_STA, &wifiConfig) == ESP_OK) {
    return String(reinterpret_cast<char*>(wifiConfig.sta.ssid));
  }
  return String();
}

/**
* 同步網路時間
**/
void ntpSetup() {
    while(WiFi.status() != WL_CONNECTED && checkTimeLoop < 3) {
      delay(1000);
      checkTimeLoop++;
    }

    if (checkTimeLoop < 3) {
      configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
      Serial.println("NTP 時間已同步");
    } else {
      Serial.println("NTP 時間同步失敗");
    }
}

/**
* 清除WiFi設定並重啟
**/
void clearWifi() {
      Serial.println("清除WiFi設定並重啟");
      WiFi.disconnect(true);
      delay(1000);
      nvs_flash_erase();
      esp_restart();
}

/**
* 切換顯示屏 開關
**/
void switchingDisplayState() {
    int screen_state = digitalRead(TFT_BL);
    digitalWrite(TFT_BL, !screen_state);    
}

/**
* tft 進入休眠
**/
void screenSleepSwitch() {
  if (timer0Elapsed) {
    switchingDisplayState();

    timer0Elapsed = false;
  }
}

/**
* 抓取當前時間
**/
void getTime() {
  if (checkTimeLoop < 3) {
    struct tm timeinfo;
    if (!getLocalTime(&timeinfo)) {
        return;
    }

    strftime(timeHour, 3, "%H", &timeinfo);
    strftime(timeMin, 3, "%M", &timeinfo);
    strftime(timeSec, 3, "%S", &timeinfo);
  }
}

/**
* 屏幕顯示時間
**/
void showNowDate() {
    // 更新時間檢查超過三次不顯示
    if (checkTimeLoop < 3) { 
      sprite.drawString(String(timeHour) + " : " + String(timeMin) + " : " + timeSec, 0, 0, 2);
      sprite.pushSprite(0, 0);
    }
}

/**
* 屏幕顯示 PM2.5
**/
void pm25DataShow() {
    sprite.fillSprite(TFT_BLACK); // 點擊按鈕清除畫面
    if (pm25Value == "0") {
      sprite.drawString("Loading...", 0, 40, 4);  
    } else {
      sprite.drawString(pm25Value +" ug/m3", 40, 40, 4);  
    }
}

/**
* 抓取PM2.5 iot 裝置數值
**/
void pm25Data() {
    pm25Value = String(data.PM_AE_UG_2_5);
}

/**
* 屏幕顯示電池icon
* Beta 數值不完全正確，測試中
**/
void batteryShow() {
    sprite.fillSprite(TFT_BLACK); // 點擊按鈕清除畫面

    // 防呆 若數值為零執行更新
    if (adc_value == 0) {
      batteryData();
    }

    int arrayIndex = 0; // 默认为最低充电状态

    if (adc_value >= 99) {
      // 充电中
      arrayIndex = 4;
    } else if (adc_value >= 90) {
      arrayIndex = 3;
    } else if (adc_value >= 60) {
      arrayIndex = 2;
    } else if (adc_value >= 30) {
      arrayIndex = 1;
    }

    sprite.drawBitmap(10, -20, battery_icon[arrayIndex], 128, 128, 0xffff);
}

/**
* 抓取電池電壓
**/
void batteryData() {
    adc_value = analogRead(PIN_BAT_VOLT);
    adc_value = voltageToPercent(float (adc_value/8191.0*3.3-0.34)*3);
}

/**
* 電壓轉換電量
**/
float voltageToPercent(float voltage) {
    float minVoltage = 3.2; // 最低電壓
    float maxVoltage = 4.2; // 最高電壓

    // 確保電壓不超出預定範圍
    if (voltage < minVoltage) {
        return 0;
    } else if (voltage > maxVoltage) {
        return 100;
    }

    // 計算百分比
    float percent = (voltage - minVoltage) / (maxVoltage - minVoltage) * 100;
    return percent;
}

/**
* 點擊按鈕觸發切換畫面
**/
void buttonLoopActive() {
  int buttonState = digitalRead(buttonPin);

  // 檢測按鈕是否被按下
  if (buttonState == LOW && !isButtonActive) {
    isButtonActive = true;
    buttonTimer = millis();
  }

  // 檢測按鈕是否被釋放
  if (buttonState == HIGH && isButtonActive) {
    if (!longPressActive) {
      sprite.fillSprite(TFT_BLACK); // 點擊按鈕清除畫面
      sprite.setTextColor(TFT_WHITE, TFT_BLACK); // 顯示字體與字體背景色 白字、黑底
      displayIndex();
    }
    isButtonActive = false;
    longPressActive = false;
  }

  // 檢測長按
  if (isButtonActive && (millis() - buttonTimer > longPressTimeoutMillis)) {
    longPressActive = true;
    // 清除wifi 設定值
    clearWifi();
  }
}

/**
* 點擊按鈕觸發 displayIndexSwitch + 1
* 回
**/
void displayIndex() {
  displayIndexSwitch = buttonCounter % backCount;
  buttonCounter++;
}

/**
* 顯示被切換畫面
* 每按一次按鈕 displayIndexSwitch + 1 切換畫面
**/
void displaySwitch() {
    switch (displayIndexSwitch) {
      // 初始畫面
      default:
        // 如果螢幕關閉切換為顯示
        if (digitalRead(TFT_BL) == LOW) {
          switchingDisplayState();
        }
        pm25DataShow();
        showNowDate();
        break;      
      // wifi 訊號
      case 0:
        wifiRssiShow();
        break;
      // 電量
      case 1:
        batteryShow();
        break;
      // 關閉顯示
      case 2:
        switchingDisplayState();
        break;          
    }        
}

void loop() {
    pms.read(data);
    // 迴圈抓取時間
    if (millis() > currTime + period) { //更新時間，並在每個“週期”時間選擇下一幀
        currTime = millis(); 
        getTime();
    }
    // 更新定時
    timerB(); 
    // 點擊按鈕觸發切換畫面
    buttonLoopActive();
    // 顯示被切換畫面
    displaySwitch();
    // 更新畫面
    sprite.pushSprite(0, 0);
}
