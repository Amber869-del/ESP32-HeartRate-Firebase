#include <WiFi.h>
#include <FirebaseESP32.h>

// ================= 1. 網路與 Firebase 設定 =================
#define WIFI_SSID "OPPO Reno12"
#define WIFI_PASSWORD "darren27"
#define FIREBASE_HOST "heart-c720e-default-rtdb.firebaseio.com"
#define FIREBASE_AUTH "6ljj2VgempHdnFRTWqhUqyLYozqSBZ7qMbxLIao8" 

// ================= 2. 腳位與心率演算法參數 =================
#define SENSOR_PIN 34          // 使用 GPIO 34 腳位

const int NO_TOUCH_THRESHOLD = 15; // 判定配戴的靈敏度門檻

unsigned long sampleInterval = 20; 
unsigned long lastSampleTime = 0;

int signalValue;
int threshold = 2000;              // 會在 setup() 中自動動態校正
int peak = 2000;
int trough = 2000;

unsigned long lastBeatTime = 0;    
unsigned long secondLastBeatTime = 0;
int BPM = 0;
bool pulse = false;

int maxSignalThisPeriod = 0;
int minSignalThisPeriod = 4095;
unsigned long lastCheckWearTime = 0;
bool isWearing = false;

unsigned long lastUploadTime = 0;
const unsigned long uploadInterval = 1000;

FirebaseData firebaseData;
FirebaseConfig config;
FirebaseAuth auth;

void setup() {
  Serial.begin(115200);
  delay(1000); 
  
  pinMode(SENSOR_PIN, INPUT);

  // === 自動基線校正：開機時連續讀取 50 次取平均值 ===
  Serial.println("【系統初始化】請將手指夾好，正在進行環境電壓校正...");
  long sum = 0;
  for (int i = 0; i < 50; i++) {
    sum += analogRead(SENSOR_PIN);
    delay(20);
  }
  threshold = sum / 50; // 將平均電壓設為初始動態門檻
  peak = threshold;
  trough = threshold;
  Serial.print("校正完成！初始偵測門檻設定為: ");
  Serial.println(threshold);
  
  // 連接 Wi-Fi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("連線至 Wi-Fi ");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWi-Fi 已連線！");

  config.host = FIREBASE_HOST;
  config.signer.tokens.legacy_token = FIREBASE_AUTH;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void loop() {
  unsigned long currentTime = millis();

  // 1. 20ms 採樣一次類比訊號
  if (currentTime - lastSampleTime >= sampleInterval) {
    lastSampleTime = currentTime;
    signalValue = analogRead(SENSOR_PIN);

    // 記錄最大與最小值
    if (signalValue > maxSignalThisPeriod) maxSignalThisPeriod = signalValue;
    if (signalValue < minSignalThisPeriod) minSignalThisPeriod = signalValue;

    // 心搏波谷與波峰追蹤
    if (signalValue < threshold && signalValue < trough) {
      trough = signalValue; 
    }
    if (signalValue > threshold && signalValue > peak) {
      peak = signalValue;   
    }

    // 判定心跳觸發：當訊號越過動態門檻，且距離上一次心跳超過 300ms (防止 double counting)
    if ((currentTime - lastBeatTime > 300) && (signalValue > threshold) && (pulse == false)) {
      pulse = true;
      secondLastBeatTime = lastBeatTime;
      lastBeatTime = currentTime;

      unsigned long interval = lastBeatTime - secondLastBeatTime;
      // 限制在合理的成人生理心率區間：35 ~ 200 BPM 
      if (interval > 300 && interval < 1700) { 
        BPM = 60000 / interval;
      }
    }

    // 當訊號降回門檻以下，重置波峰波谷並動態更新門檻 (Auto-threshold)
    if ((signalValue < threshold) && (pulse == true)) {
      pulse = false;
      threshold = (peak + trough) / 2; // 動態校正門檻
      peak = threshold;
      trough = threshold;
    }
  }

  // 2. 每 2 秒檢查一次是否真的有夾著手指
  if (currentTime - lastCheckWearTime >= 2000) {
    lastCheckWearTime = currentTime;
    int signalAmplitude = maxSignalThisPeriod - minSignalThisPeriod;

    if (signalAmplitude < NO_TOUCH_THRESHOLD) {
      isWearing = false;
      BPM = 0; 
    } else {
      isWearing = true;
    }

    maxSignalThisPeriod = 0;
    minSignalThisPeriod = 4095;
  }

  // 3. 定時儲存至 Firebase (每 1 秒存一筆，以 history 形式累加)
  if (currentTime - lastUploadTime >= uploadInterval) {
    lastUploadTime = currentTime;

    if (!isWearing) {
      if (Firebase.pushInt(firebaseData, "/heartRateHistory", 0)) {
        Serial.println("狀態: 未配戴感測器。已儲存 [0 BPM] 至歷史紀錄。");
      }
    } else {
      // 只有在算出合理的 BPM 時才儲存
      if (BPM >= 40 && BPM <= 180) {
        if (Firebase.pushInt(firebaseData, "/heartRateHistory", BPM)) {
          Serial.print("狀態: 監測中... 當前心率: ");
          Serial.print(BPM);
          Serial.println(" BPM。已成功儲存至歷史資料庫。");
        } else {
          Serial.println("儲存失敗，請檢查網路或密鑰設定。");
        }
      } else {
        // 還沒算出穩定心率前暫存 0
        Firebase.pushInt(firebaseData, "/heartRateHistory", 0);
        Serial.println("狀態: 訊號校正中... 暫存 0");
      }
    }
  }
}
