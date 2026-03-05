# Smart Fertilizer Mixer & Soil Analyzer (ESP32 + Dashboard)

ระบบ IoT สำหรับอ่านค่าดิน (N/P/K, pH) และแนะนำสูตร/ปริมาณปุ๋ยตามชนิดพืช พร้อมหน้าเว็บ Dashboard สำหรับแสดงผลแบบ real-time และสั่งงานการผสม/ปั๊มได้

---

## Live Demo (GitHub Pages)
https://tanai5688.github.io/smart-fertilizer-mixer/

> **หมายเหตุ:** GitHub Pages ทำงานใน **Demo mode** (ใช้ Manual input ได้)  
> ถ้าจะเชื่อมต่ออุปกรณ์จริง ให้ต่อ Wi-Fi ของ ESP32 แล้วเปิด: `http://192.168.4.1`

---

## Key Features
- อ่านค่า N / P / K และ pH (Real-time) และรองรับ Manual input เพื่อทดสอบ/สาธิต
- Dashboard แสดงค่าดิน + สถานะระบบ + การแจ้งเตือน
- โหมดสั่งผสมด่วน (Quick Mix) และติดตาม progress การผสม
- คำนวณ/แนะนำการใช้หัวเชื้อปุ๋ยตามความเข้ม/ขนาดถัง
- รองรับ Demo mode สำหรับนำเสนอโดยไม่ต้องต่ออุปกรณ์

---

## Tech Stack
- **MCU:** ESP32
- **Dashboard:** HTML / JavaScript
- **UI:** Tailwind (local)
- **Data:** JSON (rules/plant/fertilizer logic ตามที่พัฒนา)
- **Network:** WebSocket/HTTP (ตาม implementation)

---

## Workflow (High-level)
1) Read sensor data (N/P/K/pH) หรือ Manual input  
2) Display data on Dashboard (real-time)  
3) Process/compute recommendation (ตามชนิดพืช/สูตร/หัวเชื้อ)  
4) User confirms action (Quick Mix / Mixing)  
5) Show progress + status + notifications  

---

## Demo Mode
เมื่อเปิดผ่าน GitHub Pages ระบบจะทำงานเป็น **Demo mode**:
- ไม่พยายามเชื่อมต่ออุปกรณ์อัตโนมัติ
- สามารถใช้งาน UI และกรอกค่า Manual เพื่อสาธิตฟังก์ชันได้
- หากต้องการเชื่อมต่อจริง: ต่อ Wi-Fi ของ ESP32 → เปิด `http://192.168.4.1` → กด “เชื่อมต่อ”

---

## Screenshots
> ✅ ถ้ารูปไม่ขึ้น ให้เช็คชื่อไฟล์ในโฟลเดอร์ `images/` แล้วแก้ชื่อในลิงก์ให้ตรง 100%

![Dashboard Main](images/dashboard-main.png)
![Settings Menu](images/settings-menu.png)
![Quick Mix](images/quick-mix.png)
![Mixing Progress](images/mixing-progress.png)
![Starter Guide](images/starter-guide.png)
![Calendar QR](images/calendar-qr.png)

---

## Repository Structure
- `index.html`          : Dashboard (ใช้กับ GitHub Pages)
- `qrcode.js`           : QR / helper scripts
- `tailwind.js`         : Tailwind loader/local
- `images/`             : Screenshots
- `esp32/`              : ESP32 firmware source (upload code here)
- `web/`                : (optional) source folder เดิม/ไฟล์ซ้ำ (ถ้าไม่ได้ใช้ให้ลบทิ้งเพื่อความสะอาด)

---

## How to Run (Local / Device)
### Run with ESP32 (Recommended)
1) เปิด ESP32 และเชื่อมต่อ Wi-Fi ของอุปกรณ์
2) เปิดเบราว์เซอร์ไปที่ `http://192.168.4.1`
3) กด “เชื่อมต่อ” เพื่อรับค่าจริงและสั่งงานปั๊ม/ผสม

### Run as Demo (GitHub Pages)
1) เปิดลิงก์ Demo
2) ใช้ Manual input เพื่อสาธิตการคำนวณ/การแสดงผล/โหมดต่าง ๆ

---

## Notes
- โปรเจกต์นี้เน้นการทำงานแบบระบบจริง (ESP32 + Dashboard + logic การแนะนำ/ผสมปุ๋ย)
- ใช้สำหรับสาธิต/พอร์ตโฟลิโอ และพัฒนาต่อเป็นระบบใช้งานจริงในภาคสนาม
