# Smart Fertilizer Mixer & Soil Analyzer (ESP32 + Dashboard)

## Overview
ระบบ IoT ที่อ่านค่าดิน (N/P/K, pH) และแนะนำสูตร/ปริมาณปุ๋ยตามชนิดพืช พร้อมหน้าเว็บ Dashboard สำหรับแสดงผลแบบ real-time และสถานะการทำงาน

## Key Features
- อ่านค่า NPK และ pH จากเซนเซอร์
- แสดงผลแบบ real-time บน Dashboard
- แนะนำสูตร/ปริมาณปุ๋ยตามชนิดพืช
- แสดงสถานะการทำงาน/แจ้งเตือนเพื่อช่วยลดความผิดพลาดของผู้ใช้

## Tech Stack
ESP32, Arduino, Sensors, Wi-Fi/HTTP, HTML/CSS/JavaScript, JSON

## Workflow
1) Read sensor data → 2) Send to dashboard → 3) Process & evaluate → 4) Recommend → 5) Display status/result

## My Contribution
- ออกแบบ workflow และ state ของระบบ
- พัฒนา logic ฝั่ง ESP32 และการส่งข้อมูล
- พัฒนา/ปรับปรุง UI Dashboard และการแสดงสถานะ
- ทดสอบและปรับให้เหมาะกับการใช้งานจริง/สาธิต
