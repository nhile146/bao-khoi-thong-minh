#include <Wire.h>               // Thư viện giao tiếp I2C
#include <LiquidCrystal_I2C.h>  // Thư viện điều khiển màn hình LCD

LiquidCrystal_I2C lcd(0x27, 16, 2); // Tạo đối tượng LCD địa chỉ 0x27, 16 cột 2 dòng

// ---- CẤU HÌNH CHÂN (PIN) ----
const int MQ2_PIN  = A0; // Chân Analog đọc cảm biến khói
const int LED_PIN  = 8;  // Chân Digital điều khiển đèn báo
const int BUZ_PIN  = 9;  // Chân Digital điều khiển còi
const int BTN_PIN  = 7;  // Chân Digital đọc nút nhấn (Reset còi)

// ---- CẤU HÌNH NGƯỠNG (THRESHOLD) ----
int TH   =300;   // Ngưỡng nồng độ khói để kích hoạt báo động
int HYST = 10;   // Biên độ trễ (Hysteresis) để chống nhiễu tín hiệu

// ---- BIẾN TRẠNG THÁI (STATE) ----
bool alarm    = false;   // True = Đang có báo động
bool silenced = false;   // True = Người dùng đã bấm nút tắt còi

// ---- BIẾN THỜI GIAN (Dùng cho đa nhiệm - millis) ----
unsigned long lastLcd   = 0; // Thời điểm cập nhật LCD lần cuối
unsigned long lastBlink = 0; // Thời điểm nháy đèn lần cuối
unsigned long lastSend  = 0; // Thời điểm gửi dữ liệu sang ESP32 lần cuối
bool ledState = false;       // Trạng thái hiện tại của đèn LED (Bật/Tắt)

// ---- BIẾN CHỐNG RUNG NÚT NHẤN (DEBOUNCE) ----
int lastBtnStable = HIGH;          // Trạng thái ổn định trước đó (Input Pullup mặc định HIGH)
unsigned long lastDebounceTime = 0;// Thời điểm tín hiệu thay đổi
const unsigned long debounceMs = 40;// Thời gian chờ ổn định tín hiệu (40ms)

// ================= KHỞI TẠO HỆ THỐNG =================
void setup() {
  pinMode(LED_PIN, OUTPUT);       // Cấu hình chân Đèn là đầu ra
  pinMode(BUZ_PIN, OUTPUT);       // Cấu hình chân Còi là đầu ra
  pinMode(BTN_PIN, INPUT_PULLUP); // Cấu hình chân Nút nhấn là đầu vào có trở kéo lên

  digitalWrite(LED_PIN, LOW);     // Tắt đèn ban đầu
  digitalWrite(BUZ_PIN, LOW);     // Tắt còi ban đầu

  lcd.init();                     // Khởi động màn hình LCD
  lcd.backlight();                // Bật đèn nền LCD
  lcd.setCursor(0,0); lcd.print("Smoke Monitor"); // In lời chào
  lcd.setCursor(0,1); lcd.print("Ready...");
  delay(700);                     // Chờ một chút cho ổn định

  Serial.begin(115200);           // Bật giao tiếp UART tốc độ 115200 để nói chuyện với ESP32
  Serial.println("SAFE");         // Gửi trạng thái an toàn ban đầu
}

// Hàm phụ: Xóa dòng 1 và in nội dung mới lên LCD
void showLine0(const char* msg) {
  lcd.setCursor(0,0);
  lcd.print("                ");  // Xóa dòng cũ bằng khoảng trắng
  lcd.setCursor(0,0);
  lcd.print(msg);                 // In thông báo mới
}

// ================= VÒNG LẶP CHÍNH =================
void loop() {
  int v = analogRead(MQ2_PIN);    // Đọc giá trị cảm biến khói (0 - 1023)

  // 1. XỬ LÝ NÚT NHẤN (CHỐNG RUNG)
  int reading = digitalRead(BTN_PIN); // Đọc trạng thái nút hiện tại

  // Nếu trạng thái thay đổi, reset bộ đếm thời gian
  if (reading != lastBtnStable && (millis() - lastDebounceTime) > debounceMs) { //đảm bảo tín hiệu ổn định ít nhất 40ms
    lastDebounceTime = millis();
    lastBtnStable = reading;

    // Nếu xác nhận nút được nhấn xuống (LOW)
    if (lastBtnStable == LOW) {
      silenced = true;            // Kích hoạt chế độ Im lặng
      alarm    = false;           // Tắt trạng thái báo động ngay lập tức
      
      digitalWrite(LED_PIN, LOW); // Tắt đèn
      digitalWrite(BUZ_PIN, LOW); // Tắt còi

      Serial.println("SILENCE");  // Gửi lệnh báo ESP32 biết
      showLine0("SILENCED");      // Hiển thị lên màn hình
    }
  }

  // 2. LOGIC CẢNH BÁO (CORE LOGIC)
  if (silenced) {
    // Đang im lặng: Chỉ tự động Reset khi khói giảm xuống mức an toàn (TH - HYST)
    if (v <= TH - HYST) {
      silenced = false;           // Thoát chế độ im lặng
      Serial.println("SAFE");     // Báo an toàn cho ESP32
      showLine0("SAFE");
    }
  } else {
    // Chế độ bình thường: Kiểm tra ngưỡng báo động
    // Vượt ngưỡng trên (TH + HYST) -> BÁO ĐỘNG
    if (!alarm && v >= TH + HYST) {
      alarm = true;               // Bật cờ báo động
      Serial.println("ALARM");    // Gửi lệnh sang ESP32 (để chụp ảnh)
      showLine0("CO KHOI!");      // Hiển thị cảnh báo
    }
    // Giảm dưới ngưỡng dưới (TH - HYST) -> TẮT BÁO ĐỘNG
    if (alarm && v <= TH - HYST) {
      alarm = false;              // Tắt cờ báo động
      Serial.println("SAFE");     // Báo an toàn
      showLine0("SAFE");
    }
  }

  // 3. ĐIỀU KHIỂN THIẾT BỊ RA (NON-BLOCKING)
  if (alarm) {
    // Nháy đèn LED mỗi 300ms mà không dùng delay()
    if (millis() - lastBlink > 300) {
      lastBlink = millis();
      ledState = !ledState;       // Đảo trạng thái đèn
      digitalWrite(LED_PIN, ledState);
    }
    digitalWrite(BUZ_PIN, HIGH);  // Còi kêu liên tục
  } else {
    digitalWrite(LED_PIN, LOW);   // Tắt đèn
    digitalWrite(BUZ_PIN, LOW);   // Tắt còi
  }

  // 4. CẬP NHẬT MÀN HÌNH LCD (Mỗi 300ms)
  if (millis() - lastLcd > 300) {
    lastLcd = millis();
    lcd.setCursor(0,1);
    lcd.print("MQ2: "); lcd.print(v);     // In giá trị khói
    lcd.print("  TH:"); lcd.print(TH);    // In ngưỡng cài đặt
    
    // Xóa các ký tự thừa cuối dòng (nếu số bị ngắn lại)
    int used = 5 + String(v).length() + 4 + String(TH).length();
    for (int i = used; i < 16; i++) lcd.print(' ');
  }

  // 5. GỬI DỮ LIỆU GIÁM SÁT SANG ESP32 (Mỗi 1000ms)
  if (millis() - lastSend > 1000) {
    lastSend = millis();
    Serial.print("MQ2:");
    Serial.println(v);            // Gửi chuỗi định dạng "MQ2:xxx" để vẽ biểu đồ
  }

  delay(20); // Nghỉ cực ngắn để ổn định hệ thống
}