#include <Arduino.h>
/* 作者：Wei Zixiang
 * 感谢flyAkari提供的部分源码！
 * 
 */
/**********************************************************************
 * 使用说明：
 * 初次上电后，用任意设备连接热点WiFi：Clock，等待登录页弹出或浏览器输入
 * 192.168.5.1进入WiFi及时钟配置页面，输入待连接WiFi名和密码、时区(-12~12)，
 * 填全后提交。若连接成功，则开发板会记住以上配置的信息，并在下次上电时自动连接
 * WiFi并显示时间，热点和配置页面不再出现。如需更改倒数日或WiFi信息，请关闭原
 * WiFi阻止其自动连接，上电后10秒无法登录则会重新开启热点和配置页面。
***********************************************************************/

#include <TimeLib.h>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <WiFiUdp.h>
#include <SPI.h>
#include <EEPROM.h>
#include <U8g2lib.h>

//若屏幕使用SH1106，只需把SSD1306改为SH1106即可
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/U8X8_PIN_NONE);

static const char ntpServerName[] = "ntp1.aliyun.com"; //设置NTP服务器
int timeZone = 8;                                      //时区，北京时间为+8

WiFiUDP Udp;
unsigned int localPort = 8888; // 用于侦听UDP数据包的本地端口

time_t getNtpTime();
void sendNTPpacket(IPAddress &address);
void oledClockDisplay();
void sendCommand(int command, int value);
void initdisplay();

boolean isNTPConnected = false;

const unsigned char xing[] U8X8_PROGMEM = {
    0x00, 0x00, 0xF8, 0x0F, 0x08, 0x08, 0xF8, 0x0F, 0x08, 0x08, 0xF8, 0x0F, 0x80, 0x00, 0x88, 0x00,
    0xF8, 0x1F, 0x84, 0x00, 0x82, 0x00, 0xF8, 0x0F, 0x80, 0x00, 0x80, 0x00, 0xFE, 0x3F, 0x00, 0x00}; /*星*/
const unsigned char liu[] U8X8_PROGMEM = {
    0x40, 0x00, 0x80, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0xFF, 0x7F, 0x00, 0x00, 0x00, 0x00,
    0x20, 0x02, 0x20, 0x04, 0x10, 0x08, 0x10, 0x10, 0x08, 0x10, 0x04, 0x20, 0x02, 0x20, 0x00, 0x00}; /*六*/

typedef struct
{                  //存储配置结构体
    int tz;        //时间戳
} config_type;
config_type config;

void saveConfig()
{ //存储配置到"EEPROM"
    Serial.println("save config");
    EEPROM.begin(sizeof(config));
    uint8_t *p = (uint8_t *)(&config);
    for (uint i = 0; i < sizeof(config); i++)
    {
        EEPROM.write(i, *(p + i));
    }
    EEPROM.commit(); //此操作会消耗flash写入次数
}

void loadConfig()
{ //从"EEPROM"加载配置
    Serial.println("load config");
    EEPROM.begin(sizeof(config));
    uint8_t *p = (uint8_t *)(&config);
    for (uint i = 0; i < sizeof(config); i++)
    {
        *(p + i) = EEPROM.read(i);
    }
    timeZone = config.tz;
}

char sta_ssid[32] = {0};          //暂存WiFi名
char sta_password[64] = {0};      //暂存WiFi密码
const char *AP_NAME = "Clock"; //自定义8266AP热点名
//配网及目标日期设定html页面
const char *page_html = "\
<!DOCTYPE html>\r\n\
<html lang='en'>\r\n\
<head>\r\n\
  <meta charset='UTF-8'>\r\n\
  <meta name='viewport' content='width=device-width, initial-scale=1.0'>\r\n\
  <title>Internet Clock Setting</title>\r\n\
</head>\r\n\
<body>\r\n\
  <div style=" text-align:center;">\r\n\
  <h1>Internet Clock配置页面</h1>\r\n\
  <form name='input' action='/' method='POST'>\r\n\
    WiFi名称：\r\n\
    <input type='text' name='ssid'><br>\r\n\
    WiFi密码：\r\n\
    <input type='password' name='password'><br>\r\n\
    时区（北京时间：8）:\r\n\
    <input type='text' name='timezone' value='8'><br><br>\r\n\
    <input type='submit' value='提交'>\r\n\
    <p>注意：配置成功后可能还要等个十几分钟屏幕才可显示时间 <a href="https://www.weizixiang.com/Internet-Clock/help2.html">了解更多</a></p>\r\n\
    <p>因为ESP8266硬件性能较低，故本页不使用视觉UI，仅加载基础的HTML框架，页面较丑，敬请谅解！</p>\r\n\
    <br><br>\r\n\
    <a href="https://www.weizixiang.com/Internet-Clock/help.html">帮助页面</a> | <a href='https://www.weizixiang.com'>子翔的小站</a>\r\n\
  </form>\r\n\
</div>\r\n\
</body>\r\n\
</html>\r\n\
";
const byte DNS_PORT = 53;       //DNS端口号默认为53
IPAddress apIP(192, 168, 5, 1); //8266 APIP
DNSServer dnsServer;
ESP8266WebServer server(80);

void connectWiFi();

void handleRoot()
{
    server.send(200, "text/html", page_html);
}
void handleRootPost()
{
    Serial.println("handleRootPost");
    if (server.hasArg("ssid"))
    {
        Serial.print("ssid:");
        strcpy(sta_ssid, server.arg("ssid").c_str());
        Serial.println(sta_ssid);
    }
    else
    {
        Serial.println("[WebServer]Error, SSID not found!");
        server.send(200, "text/html", "<meta charset='UTF-8'>Error, SSID not found!"); //返回错误页面
        return;
    }
    if (server.hasArg("password"))
    {
        Serial.print("password:");
        strcpy(sta_password, server.arg("password").c_str());
        Serial.println(sta_password);
    }
    else
    {
        Serial.println("[WebServer]Password Error");
        server.send(200, "text/html", "<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><p style="text-align:center;">Password Error</p>");
        return;
    }
    if (server.hasArg("timezone"))
    {
        Serial.print("timezone:");
        char timeZone_s[4];
        strcpy(timeZone_s, server.arg("timezone").c_str());
        timeZone = atoi(timeZone_s);
        if (timeZone > 13 || timeZone < -13)
        {
            timeZone = 8;
        }
        Serial.println(timeZone);
        config.tz = timeZone;
    }
    else
    {
        Serial.println("[WebServer]TIMEZONE Error");
        server.send(200, "text/html", "<meta charset='UTF-8'>TIMEZONE Error");
        return;
    }
    if (server.hasArg("clock"))
    {
        Serial.print("isClock:");
        Serial.println(server.arg("clock"));
    }
    server.send(200, "text/html", "<meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1.0'><p style="text-align:center;">成功！</p>"); //返回保存成功页面
    delay(2000);
    //一切设定完成，连接wifi
    saveConfig();
    connectWiFi();
}

void connectWiFi()
{
    WiFi.mode(WIFI_STA);       //切换为STA模式
    WiFi.setAutoConnect(true); //设置自动连接
    WiFi.begin(sta_ssid, sta_password);
    Serial.println("");
    Serial.print("Connect WiFi");
    int count = 0;
    while (WiFi.status() != WL_CONNECTED)
    {
        delay(500);
        count++;
        if (count > 20)
        { //10秒过去依然没有自动连上，开启Web配网功能，可视情况调整等待时长
            Serial.println("Timeout! AutoConnect failed");
            WiFi.mode(WIFI_AP); //开热点
            WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
            if (WiFi.softAP(AP_NAME))
            {
                Serial.println("ESP8266 SoftAP is on");
            }
            server.on("/", HTTP_GET, handleRoot);      //设置主页回调函数
            server.onNotFound(handleRoot);             //设置无法响应的http请求的回调函数
            server.on("/", HTTP_POST, handleRootPost); //设置Post请求回调函数
            server.begin();                            //启动WebServer
            Serial.println("WebServer started!");
            if (dnsServer.start(DNS_PORT, "*", apIP))
            { //判断将所有地址映射到esp8266的ip上是否成功
                Serial.println("start dnsserver success.");
            }
            else
                Serial.println("开启DNS服务器失败！请参阅 帮助");
            Serial.println("请重新配置WiFi");
            Serial.println("连接名为Clock的WiFi，配置页面将自动弹出，如果没有，请使用浏览器访问192.168.5.1");
            break; //启动WebServer后便跳出while循环，回到loop
        }
        Serial.print(".");
        if (WiFi.status() == WL_CONNECT_FAILED)
        {
            Serial.print("password:");
            Serial.print(WiFi.psk().c_str());
            Serial.println(" is incorrect");
        }
        if (WiFi.status() == WL_NO_SSID_AVAIL)
        {
            Serial.print("configured SSID:");
            Serial.print(WiFi.SSID().c_str());
            Serial.println(" cannot be reached");
        }
    }
    Serial.println("");
    if (WiFi.status() == WL_CONNECTED)
    {
        Serial.println("Finish");
        Serial.print("IP address: ");
        Serial.println(WiFi.localIP());
        server.stop();
        dnsServer.stop();
        //WiFi连接成功后，热点便不再开启，无法再次通过web配网
        //若WiFi连接断开，ESP8266会自动尝试重新连接，直至连接成功，无需代码干预
        //如需要更换WiFi，请在关闭原WiFi后重启ESP8266，否则上电后会自动连接原WiFi，也就无法进入配网页面
    }
}

void setup()
{
    Serial.begin(115200);
    while (!Serial)
        continue;
    Serial.println("NTP Clock oled version v1.1");
    Serial.println("Designed by flyAkari");
    initdisplay();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_unifont_t_chinese2);
    u8g2.setCursor(0, 14);
    u8g2.print("Waiting for WiFi");
    u8g2.setCursor(0, 30);
    u8g2.print("connection...");
    u8g2.setCursor(0, 47);
    u8g2.print("flyAkari");
    u8g2.setCursor(0, 64);
    u8g2.print("192.168.5.1");
    u8g2.sendBuffer();
    Serial.println("OLED Ready");
    Serial.print("Connecting WiFi...");
    loadConfig();
    Serial.print("Connecting WiFi...");
    WiFi.hostname("Smart-ESP8266");
    connectWiFi();
    Serial.println("Starting UDP");
    Udp.begin(localPort);
    Serial.print("Local port: ");
    Serial.println(Udp.localPort());
    Serial.println("waiting for sync");
    setSyncProvider(getNtpTime);
    setSyncInterval(300); //每300秒同步一次时间
}

time_t prevDisplay = 0; //当时钟已经显示

void loop()
{
    server.handleClient();
    dnsServer.processNextRequest();
    if (timeStatus() != timeNotSet)
    {
        if (now() != prevDisplay)
        { //时间改变时更新显示
            prevDisplay = now();
            oledClockDisplay();
        }
    }
}

void initdisplay()
{
    u8g2.begin();
    u8g2.enableUTF8Print();
}

void oledClockDisplay()
{
    int years, months, days, hours, minutes, seconds, weekdays;
    years = year();
    months = month();
    days = day();
    hours = hour();
    minutes = minute();
    seconds = second();
    weekdays = weekday();
    Serial.printf("%d/%d/%d %d:%d:%d Weekday:%d\n", years, months, days, hours, minutes, seconds, weekdays);
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_unifont_t_chinese2);
    u8g2.setCursor(0, 14);
    if (isNTPConnected)
    {
        if(timeZone>=0)
        {
            u8g2.print("当前时间（UTC+");
            u8g2.print(timeZone);
            u8g2.print(")");
        }
        else
        {
            u8g2.print("当前时间(UTC");
            u8g2.print(timeZone);
            u8g2.print(")");
        }
    }
    else
        u8g2.print("无网络!"); //如果上次对时失败，则会显示无网络
    String currentTime = "";
    if (hours < 10)
        currentTime += 0;
    currentTime += hours;
    currentTime += ":";
    if (minutes < 10)
        currentTime += 0;
    currentTime += minutes;
    currentTime += ":";
    if (seconds < 10)
        currentTime += 0;
    currentTime += seconds;
    String currentDay = "";
    currentDay += years;
    currentDay += "/";
    if (months < 10)
        currentDay += 0;
    currentDay += months;
    currentDay += "/";
    if (days < 10)
        currentDay += 0;
    currentDay += days;

    u8g2.setFont(u8g2_font_logisoso24_tr);
    u8g2.setCursor(0, 44);
    u8g2.print(currentTime);
    u8g2.setCursor(0, 61);
    u8g2.setFont(u8g2_font_unifont_t_chinese2);
    u8g2.print(currentDay);
    u8g2.drawXBM(80, 48, 16, 16, xing);
    u8g2.setCursor(95, 62);
    u8g2.print("期");
    if (weekdays == 1)
        u8g2.print("日");
    else if (weekdays == 2)
        u8g2.print("一");
    else if (weekdays == 3)
        u8g2.print("二");
    else if (weekdays == 4)
        u8g2.print("三");
    else if (weekdays == 5)
        u8g2.print("四");
    else if (weekdays == 6)
        u8g2.print("五");
    else if (weekdays == 7)
        u8g2.drawXBM(111, 49, 16, 16, liu);
    u8g2.sendBuffer();
}

/*-------- NTP 代码 ----------*/

const int NTP_PACKET_SIZE = 48;     // NTP时间在消息的前48个字节里
byte packetBuffer[NTP_PACKET_SIZE]; // 输入输出包的缓冲区

time_t getNtpTime()
{
    IPAddress ntpServerIP; // NTP服务器的地址

    while (Udp.parsePacket() > 0)
        ; // 丢弃以前接收的任何数据包
    Serial.println("Transmit NTP Request");
    // 从池中获取随机服务器
    WiFi.hostByName(ntpServerName, ntpServerIP);
    Serial.print(ntpServerName);
    Serial.print(": ");
    Serial.println(ntpServerIP);
    sendNTPpacket(ntpServerIP);
    uint32_t beginWait = millis();
    while (millis() - beginWait < 1500)
    {
        int size = Udp.parsePacket();
        if (size >= NTP_PACKET_SIZE)
        {
            Serial.println("Receive NTP Response");
            isNTPConnected = true;
            Udp.read(packetBuffer, NTP_PACKET_SIZE); // 将数据包读取到缓冲区
            unsigned long secsSince1900;
            // 将从位置40开始的四个字节转换为长整型，只取前32位整数部分
            secsSince1900 = (unsigned long)packetBuffer[40] << 24;
            secsSince1900 |= (unsigned long)packetBuffer[41] << 16;
            secsSince1900 |= (unsigned long)packetBuffer[42] << 8;
            secsSince1900 |= (unsigned long)packetBuffer[43];
            Serial.println(secsSince1900);
            Serial.println(secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR);
            return secsSince1900 - 2208988800UL + timeZone * SECS_PER_HOUR;
        }
    }
    Serial.println("No NTP Response :-("); //无NTP响应
    isNTPConnected = false;
    return 0; //如果未得到时间则返回0
}

// 向给定地址的时间服务器发送NTP请求
void sendNTPpacket(IPAddress &address)
{
    memset(packetBuffer, 0, NTP_PACKET_SIZE);
    packetBuffer[0] = 0b11100011; // LI, Version, Mode
    packetBuffer[1] = 0;          // Stratum, or type of clock
    packetBuffer[2] = 6;          // Polling Interval
    packetBuffer[3] = 0xEC;       // Peer Clock Precision
    // 8 bytes of zero for Root Delay & Root Dispersion
    packetBuffer[12] = 49;
    packetBuffer[13] = 0x4E;
    packetBuffer[14] = 49;
    packetBuffer[15] = 52;
    Udp.beginPacket(address, 123); //NTP需要使用的UDP端口号为123
    Udp.write(packetBuffer, NTP_PACKET_SIZE);
    Udp.endPacket();
}
