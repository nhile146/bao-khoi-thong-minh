#include <WiFi.h>             //Thư viện cốt lõi giúp ESP32 kết nối mạng WiFi
#include <WiFiClientSecure.h> // Thư viện hỗ trợ kết nối bảo mật HTTPS (cần cho Telegram/AWS)
#include <HTTPClient.h>       // Thư viện giúp gửi các lệnh web (GET, POST, PUT)
#include <ArduinoJson.h>      // Thư viện xử lý dữ liệu dạng JSON (đóng gói và giải mã)
#include <PubSubClient.h>     // Thư viện giao thức MQTT (dùng để gửi dữ liệu lên ThingsBoard)
#include "esp_camera.h"       // Thư viện cấu hình và điều khiển Camera
#include <WiFiManager.h>      // Thư viện tạo trang web cấu hình WiFi (không cần nạp cứng mật khẩu)

//================ CẤU HÌNH LIÊN KẾT (LINK & TOKEN) ================
// Link biểu đồ ThingsBoard (Public) để gửi kèm tin nhắn Telegram
const char* DASHBOARD_LINK = "https://demo.thingsboard.io/dashboard/ce4803c0-c5f9-11f0-86b8-4d4474c0a366?publicId=bc36a8c0-c5fa-11f0-86b8-4d4474c0a366";

const char* TG_TOKEN = "8536968518:AAFSCKL5M8gwKTeDkbbifMzr--Gkktq5KXQ"; // Mã Token của Bot Telegram
const char* TG_CHAT_ID = "381744288"; // ID đoạn chat cá nhân để nhận tin nhắn

const char* TB_HOST = "demo.thingsboard.io"; // Địa chỉ Server ThingsBoard
const int   TB_PORT = 1883;                  // Cổng giao tiếp MQTT mặc định
const char* TB_TOKEN = "oxq6IxTu1BtMhhxJuzx9"; // Token xác thực thiết bị trên ThingsBoard

// Link API Gateway (AWS) để lấy đường dẫn upload ảnh tạm thời
const char* API_PRESIGN = "https://s7hnfw20uc.execute-api.ap-southeast-1.amazonaws.com/prod/presign";

//================ CẤU HÌNH CHÂN (PIN DEFINITION) ================
#define LED_PIN   14  // Chân nối đèn LED đỏ (báo trạng thái)
#define BUZ_PIN   12  // Chân nối còi báo động (Buzzer)
#define FLASH_PIN 4   // Chân đèn Flash tích hợp sẵn trên mạch ESP32-CAM (GPIO 4)

//================ CẤU HÌNH PHẦN CỨNG CAMERA (AI THINKER) ================
// Đây là sơ đồ chân mặc định của module ESP32-CAM AI Thinker
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

//================ BIẾN TOÀN CỤC (GLOBAL VARIABLES) ================
bool alarmState = false; // Trạng thái báo động: true = Có cháy, false = An toàn
int currentMQ2 = 0;      // Biến lưu giá trị nồng độ khói nhận từ Arduino

unsigned long lastAlarmPhoto = 0;     // Lưu thời điểm chụp ảnh gần nhất
const unsigned long COOLDOWN = 15000; // Thời gian nghỉ bắt buộc giữa 2 lần chụp (15 giây)
unsigned long lastTB = 0;             // Lưu thời điểm gửi dữ liệu lên ThingsBoard gần nhất

//================ KHỞI TẠO ĐỐI TƯỢNG MẠNG ================
WiFiClient netClient;         // Tạo đối tượng Client kết nối mạng
PubSubClient mqtt(netClient); // Tạo đối tượng MQTT sử dụng Client trên

//================ HÀM NHÁY ĐÈN FLASH (HỖ TRỢ DEBUG) ================
void blinkSignal(int times, int duration) {
  for (int i = 0; i < times; i++) {
    digitalWrite(FLASH_PIN, HIGH); // Bật đèn
    delay(duration);               // Sáng trong bao lâu (ms)
    digitalWrite(FLASH_PIN, LOW);  // Tắt đèn
    delay(200);                    // Nghỉ 200ms trước lần nháy tiếp theo
  }
}

//================ HÀM KẾT NỐI WIFI (DÙNG WIFIMANAGER) ================
void ensureWiFi() {
  // Kiểm tra: Nếu đã có mạng rồi thì thoát hàm, không làm gì cả
  if (WiFi.status() == WL_CONNECTED) return;

  WiFiManager wm; // Khởi tạo đối tượng WiFiManager

  // wm.resetSettings(); //Bỏ chú thích dòng này nếu muốn xóa sạch WiFi cũ để test lại

  Serial.println("Dang ket noi WiFi...");
  
  // Hàm quan trọng nhất: Tự động kết nối WiFi cũ.
  // Nếu không được, nó tự phát WiFi tên "BaoKhoi_Setup" để người dùng cấu hình.
  bool res = wm.autoConnect("BaoKhoi_Setup"); 

  if(!res) { // Nếu quá thời gian mà không kết nối được
    Serial.println("Ket noi that bai. Restarting...");
    ESP.restart(); // Khởi động lại vi điều khiển để thử lại từ đầu
  } 
  else { // Nếu kết nối thành công
    Serial.println("Da ket noi WiFi!");
    blinkSignal(3, 100); // Nháy đèn 3 lần báo hiệu OK
  }
}

//================ HÀM KẾT NỐI MQTT (THINGSBOARD) ================
void ensureMQTT() {
  if (mqtt.connected()) return; // Nếu đã kết nối MQTT thì thoát
  
  mqtt.setServer(TB_HOST, TB_PORT); // Cài đặt địa chỉ server và cổng (1883)
  
  // Thử kết nối (Non-blocking: Không dùng vòng lặp chặn để tránh treo máy)
  if (!mqtt.connected()) {
      // Thử kết nối với ID là "esp32cam" và Token đã khai báo
      if (mqtt.connect("esp32cam", TB_TOKEN, NULL)) {
         blinkSignal(1, 50); // Nháy nhẹ 1 cái nếu kết nối lại thành công
      }
  }
}

//================ HÀM GỬI DỮ LIỆU LÊN THINGSBOARD ================
void tbPublish(int mq2, bool alarm, String img="") {
  if (!mqtt.connected()) return; // Nếu mất kết nối MQTT thì không gửi
  
  // Tạo chuỗi JSON thủ công: {"mq2": 123, "alarm": true, "image_url": "..."}
  String p = "{\"mq2\":"+String(mq2)+",\"alarm\":"+String(alarm?"true":"false");
  if (img!="") p += ",\"image_url\":\""+img+"\""; // Chèn link ảnh nếu có
  p += "}";
  
  // Gửi chuỗi JSON lên topic Telemetry của ThingsBoard
  mqtt.publish("v1/devices/me/telemetry", p.c_str());
}

//================ HÀM KHỞI TẠO CAMERA ================
bool initCamera() {
  camera_config_t c; // Tạo cấu trúc cấu hình
  c.ledc_channel = LEDC_CHANNEL_0; // Cấu hình kênh PWM cho camera
  c.ledc_timer   = LEDC_TIMER_0;
  // ... (Gán các chân GPIO vào cấu hình)
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM; c.pin_vsync = VSYNC_GPIO_NUM;
  c.pin_href  = HREF_GPIO_NUM; c.pin_sscb_sda = SIOD_GPIO_NUM; c.pin_sscb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  
  c.xclk_freq_hz = 20000000;       // Tần số xung nhịp camera 20MHz
  c.pixel_format = PIXFORMAT_JPEG; // Định dạng ảnh nén JPEG
  c.frame_size = FRAMESIZE_QVGA;   // Độ phân giải thấp (320x240) để gửi nhanh
  c.jpeg_quality = 12;             // Chất lượng ảnh (10-63), thấp hơn là nét hơn
  c.fb_count = 1;                  // Số lượng bộ đệm khung hình

  // Khởi tạo và kiểm tra lỗi
  if (esp_camera_init(&c) != ESP_OK) {
    blinkSignal(10, 50); // Nháy 10 lần nếu lỗi Camera
    return false;
  }
  return true; // Khởi tạo thành công
}

//================ HÀM LẤY LINK UPLOAD TỪ AWS (CLOUD) ================
bool getPresign(String &uploadURL, String &fileURL) {
  WiFiClientSecure cli; cli.setInsecure(); // Tạo client HTTPS, bỏ qua check chứng chỉ SSL
  HTTPClient http;
  http.begin(cli, API_PRESIGN); // Kết nối đến API Gateway
  
  int code = http.GET(); // Gửi yêu cầu GET
  String body = http.getString(); // Lấy nội dung phản hồi
  http.end(); // Đóng kết nối
  
  if (code != 200) return false; // Nếu lỗi (khác 200 OK) thì thoát
  
  // Giải mã JSON để lấy 2 đường link: Link để upload (uploadURL) và Link để xem (fileURL)
  StaticJsonDocument<2048> root; deserializeJson(root, body);// giải mã gói tin dạng chuỗi
  StaticJsonDocument<2048> inner; deserializeJson(inner, root["body"].as<String>());//giải chỗi thành đối tuong JSON
  uploadURL = inner["upload_url"].as<String>();
  fileURL   = inner["file_url"].as<String>();
  return true;
}

//================ HÀM UPLOAD ẢNH LÊN S3 ================
bool uploadImage(String url, uint8_t* buf, size_t len) {
  WiFiClientSecure cli; cli.setInsecure();// bỏ qua chứng chỉ ssl(ổ khóa bảo mật của server)
  HTTPClient http;
  http.begin(cli, url); // Kết nối đến link Upload vừa lấy được
  http.addHeader("Content-Type", "image/jpeg"); // Báo loại file là ảnh JPEG
  
  int code = http.PUT(buf, len); // Gửi dữ liệu ảnh (PUT request)
  http.end();
  return (code == 200 || code == 204); // Trả về true nếu thành công
}

//================ HÀM GỬI TIN NHẮN TELEGRAM ================
void sendTelegram(String link) {
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient http;
  // Tạo đường dẫn API gửi ảnh của Telegram
  String ep = "https://api.telegram.org/bot"+String(TG_TOKEN)+"/sendPhoto";
  
  http.begin(cli, ep);
  http.addHeader("Content-Type","application/json"); // Định dạng dữ liệu gửi là JSON
  
  // Soạn nội dung tin nhắn: Cảnh báo + Link Dashboard
  String message = "🔥 PHÁT HIỆN KHÓI!\\n📈 Xem biểu đồ: " + String(DASHBOARD_LINK);
  
  // Đóng gói JSON: Chat ID, Link ảnh, Caption 
  String payload = "{\"chat_id\":\""+String(TG_CHAT_ID)+"\",\"photo\":\""+link+"\",\"caption\":\"" + message + "\"}";
  
  http.POST(payload); // Gửi đi
  http.end();
}

//================ HÀM CHỤP VÀ XỬ LÝ ẢNH ================
void takePhoto() {
  // Kiểm tra thời gian nghỉ (Cooldown) để tránh spam tin nhắn
  if (millis()-lastAlarmPhoto < COOLDOWN) return;
  
  // Bật đèn Flash 1 giây để trợ sáng trước khi chụp
  digitalWrite(FLASH_PIN, HIGH); 
  delay(1000);
  
  // 1. Lấy link upload từ AWS
  String up, link;
  if (!getPresign(up, link)) { digitalWrite(FLASH_PIN, LOW); return; }

  // 2. Chụp ảnh lưu vào bộ đệm (fb)
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { digitalWrite(FLASH_PIN, LOW); return; }

  // 3. Upload ảnh lên AWS S3
  bool ok = uploadImage(up, fb->buf, fb->len);
  
  digitalWrite(FLASH_PIN, LOW); // Upload xong tắt đèn ngay
  esp_camera_fb_return(fb);     // Giải phóng bộ nhớ camera

  // 4. Nếu upload thành công -> Gửi Telegram và cập nhật ThingsBoard
  if (!ok) return;
  sendTelegram(link);
  tbPublish(currentMQ2, true, link);
  lastAlarmPhoto = millis(); // Cập nhật thời gian chụp lần cuối
}

//================ HÀM SETUP (CHẠY 1 LẦN) ================
void setup() {
  Serial.begin(115200); // Khởi động giao tiếp Serial
  
  // Cấu hình các chân làm đầu ra (Output)
  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZ_PIN, OUTPUT);
  pinMode(FLASH_PIN, OUTPUT); 
  
  blinkSignal(1, 200); // Nháy đèn báo hiệu đã cấp nguồn

  // 1. Kết nối WiFi (Sẽ tự phát WiFi nếu chưa cấu hình)
  ensureWiFi(); 

  // 2. Khởi động Camera
  initCamera();
  
  // 3. Kết nối MQTT
  ensureMQTT();
}

//================ HÀM LOOP (CHẠY LẶP LẠI) ================
void loop() {
  // Kiểm tra kết nối WiFi, nếu mất thì (ở đây để trống, có thể thêm code reconnect)
  if (WiFi.status() != WL_CONNECTED) {
     // Tùy chọn: Có thể thêm lệnh reset nếu mất mạng quá lâu
  }

  // --- ĐỌC DỮ LIỆU TỪ ARDUINO GỬI SANG ---
  if (Serial.available() > 0) {
    String data = Serial.readStringUntil('\n'); // Đọc 1 dòng dữ liệu
    data.trim(); // Xóa khoảng trắng thừa
    
    // Nháy đèn cực nhanh để báo hiệu có nhận được tín hiệu (Debug)
    digitalWrite(FLASH_PIN, HIGH); delay(10); digitalWrite(FLASH_PIN, LOW);

    // Phân tích nội dung tin nhắn
    if (data.startsWith("MQ2:")) { // Nếu là dữ liệu khói (MQ2:xxx)
      String valStr = data.substring(4); //lấy kí tự "mq2:"
      currentMQ2 = valStr.toInt(); // Chuyển thành số và lưu lại
    }
    else if (data == "ALARM") { // Nếu là lệnh Báo động
      alarmState = true;
    }
    else if (data == "SAFE" || data == "SILENCE" || data == "SILENCED") { // Nếu là lệnh An toàn
      alarmState = false;
    }
  }

  // --- XỬ LÝ TRẠNG THÁI BÁO ĐỘNG ---
  if (alarmState) {
    digitalWrite(LED_PIN, HIGH); // Bật đèn LED đỏ
    digitalWrite(BUZ_PIN, HIGH); // Bật còi
    takePhoto(); // Thực hiện quy trình chụp ảnh và gửi tin
  } else {
    digitalWrite(LED_PIN, LOW);  // Tắt đèn
    digitalWrite(BUZ_PIN, LOW);  // Tắt còi
  }

  // --- DUY TRÌ KẾT NỐI MQTT ---
  ensureMQTT();
  mqtt.loop(); // Hàm duy trì kết nối MQTT
  
  // --- GỬI DỮ LIỆU ĐỊNH KỲ (Mỗi 2 giây) ---
  if (millis() - lastTB > 2000) { 
    tbPublish(currentMQ2, alarmState); // Gửi MQ2 lên ThingsBoard
    lastTB = millis();
  }
}