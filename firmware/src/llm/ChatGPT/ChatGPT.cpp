#include <Arduino.h>
#include <M5Unified.h>
#include <SPIFFS.h>
#include <Avatar.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "rootCA/rootCACertificate.h"
#include <ArduinoJson.h>
#include "SpiRamJsonDocument.h"
#include "ChatGPT.h"
#include "../ChatHistory.h"
#include "FunctionCall.h"
#include "MCPClient.h"
#include "Robot.h"

using namespace m5avatar;
extern Avatar avatar;

// バッファリング+フロー制御を行うストリーミング送信クラス
// 原理: TLS送信は内部バッファ→wifiタスク(CPU0)で非同期実行されるため、
// 送信側はavailableForWrite()で空き容量を監視し、満杯時は待機してwifiタスクに処理時間を譲る。
class BufferedStreamingPrint : public Print {
public:
  explicit BufferedStreamingPrint(Client& client, size_t bufSize = 512, size_t minAvailable = 256)
    : client_(client), bufSize_(bufSize), minAvailable_(minAvailable), bufPos_(0)
  {
    buffer_ = (uint8_t*)malloc(bufSize_);
    if (!buffer_) {
      Serial.println("[BufferedStreamingPrint] malloc failed");
      bufSize_ = 0;
    }
  }

  ~BufferedStreamingPrint() {
    flush();
    if (buffer_) {
      free(buffer_);
    }
  }

  size_t write(uint8_t b) override {
    if (!buffer_ || bufSize_ == 0) {
      return 0;
    }
    buffer_[bufPos_++] = b;
    if (bufPos_ >= bufSize_) {
      flush();
    }
    return 1;
  }

  size_t write(const uint8_t* data, size_t size) override {
    if (!buffer_ || bufSize_ == 0) {
      return 0;
    }
    size_t written = 0;
    while (written < size) {
      size_t chunk = size - written;
      size_t available = bufSize_ - bufPos_;
      if (chunk > available) {
        chunk = available;
      }
      memcpy(buffer_ + bufPos_, data + written, chunk);
      bufPos_ += chunk;
      written += chunk;
      if (bufPos_ >= bufSize_) {
        flush();
      }
    }
    return written;
  }

  void flush() {
    if (bufPos_ == 0) {
      return;
    }

    size_t pos = 0;
    int stallCount = 0;
    const int MAX_STALL = 10000; // 10秒タイムアウト（1ms * 10000）

    while (pos < bufPos_) {
      // 単純に書き込み、内部でブロッキング処理される
      // availableForWrite()は期待通り動作しないケースがあるため使わない
      size_t toSend = bufPos_ - pos;
      // 一度に送信するサイズを制限（大きすぎるとバッファ詰まりを起こす）
      if (toSend > 512) {
        toSend = 512;
      }

      size_t sent = client_.write(buffer_ + pos, toSend);
      
      if (sent == 0) {
        // 送信できなかった場合は短時間待機
        delay(1);
        stallCount++;
        if (stallCount >= MAX_STALL) {
          Serial.printf("[BufferedStreamingPrint] Timeout after %d retries (sent %u/%u bytes)\n", 
                        stallCount, (unsigned)pos, (unsigned)bufPos_);
          break;
        }
        continue;
      }

      stallCount = 0; // 送信成功したらカウンタリセット
      pos += sent;
      totalSent_ += sent;

      // 送信進捗表示（約50KB毎に変更）
      if (totalSent_ / 50000 > lastReportedChunk_) {
        lastReportedChunk_ = totalSent_ / 50000;
        Serial.printf("[Streaming] %u KB sent\n", (unsigned)(totalSent_ / 1000));
      }

      // wifiタスクに定期的に処理時間を譲る（重要）
      if (sent > 0 && pos % 512 == 0) {
        delay(1); // 512バイト毎に1ms待機
      }
    }

    bufPos_ = 0;
  }

private:
  Client& client_;
  uint8_t* buffer_;
  size_t bufSize_;
  size_t minAvailable_;
  size_t bufPos_;
  size_t totalSent_ = 0;
  size_t lastReportedChunk_ = 0;
};


static bool parse_https_url(const char* url, String& host, uint16_t& port, String& path) {
  host = "";
  path = "/";
  port = 443;

  String u(url);
  if (!u.startsWith("https://")) {
    return false;
  }

  int hostStart = 8;
  int pathStart = u.indexOf('/', hostStart);
  if (pathStart < 0) {
    host = u.substring(hostStart);
    path = "/";
  } else {
    host = u.substring(hostStart, pathStart);
    path = u.substring(pathStart);
  }

  int colon = host.indexOf(':');
  if (colon > 0) {
    port = (uint16_t)host.substring(colon + 1).toInt();
    host = host.substring(0, colon);
  }

  return host.length() > 0;
}

String ChatGPT::https_post_json(const char* url, const JsonDocument& doc, const char* root_ca) {
  String payload = "";

  String host, path;
  uint16_t port = 443;
  if (!parse_https_url(url, host, port, path)) {
    Serial.println("[HTTPS] Invalid URL");
    return payload;
  }

  WiFiClientSecure* client = new WiFiClientSecure;
  if (!client) {
    Serial.println("Unable to create client");
    return payload;
  }

  client->setCACert(root_ca);
  client->setTimeout(65000);

  Serial.print("[HTTPS] begin...\n");
  if (!client->connect(host.c_str(), port)) {
    Serial.println("[HTTPS] Unable to connect");
    delete client;
    return payload;
  }

  size_t contentLen = measureJson(doc);
  Serial.printf("[HTTPS] Streaming POST: %u bytes\n", (unsigned)contentLen);

  // HTTPヘッダ送信
  client->printf("POST %s HTTP/1.1\r\n", path.c_str());
  client->printf("Host: %s\r\n", host.c_str());
  client->print("User-Agent: StackChanEx\r\n");
  client->print("Accept: application/json\r\n");
  client->print("Content-Type: application/json\r\n");
  client->printf("Authorization: Bearer %s\r\n", param.api_key.c_str());
  client->printf("Content-Length: %u\r\n", (unsigned)contentLen);
  client->print("Connection: close\r\n\r\n");

  // バッファリング+フロー制御付きストリーミング送信
  // 原理: 一定サイズずつwrite()し、定期的にdelay()でwifiタスクに処理時間を譲る
  Serial.println("[HTTPS] Starting buffered streaming transfer...");
  unsigned long startTime = millis();
  {
    BufferedStreamingPrint stream(*client, 512);
    serializeJson(doc, stream);
    // デストラクタで自動flush
  }
  unsigned long elapsed = millis() - startTime;
  Serial.printf("[HTTPS] Transfer complete (%lu ms)\n", elapsed);

  // レスポンスが届くまで少し待機
  delay(50);

  // HTTPレスポンスの読み取り
  String statusLine = "";
  unsigned long waitStart = millis();
  while (statusLine.length() == 0 && millis() - waitStart < 5000) {
    if (client->available()) {
      statusLine = client->readStringUntil('\n');
      statusLine.trim();
    } else {
      delay(10);
    }
  }

  int httpCode = 0;
  if (statusLine.startsWith("HTTP/")) {
    int firstSpace = statusLine.indexOf(' ');
    if (firstSpace > 0 && statusLine.length() >= firstSpace + 4) {
      httpCode = statusLine.substring(firstSpace + 1, firstSpace + 4).toInt();
    }
  }
  Serial.printf("[HTTPS] Response code: %d\n", httpCode);

  // ヘッダー読み飛ばし（空行まで）、chunked encoding検出
  bool isChunked = false;
  while (client->connected() || client->available()) {
    String line = client->readStringUntil('\n');
    line.trim();
    if (line.length() == 0) {
      break; // 空行を検出したらヘッダー終了
    }
    if (line.equalsIgnoreCase("Transfer-Encoding: chunked")) {
      isChunked = true;
      Serial.println("[HTTPS] Chunked encoding detected");
    }
    delay(0);
  }

  // ボディ読み取り（chunked encodingに対応）
  if (isChunked) {
    // chunked encodingをデコード
    while (client->connected() || client->available()) {
      String chunkSizeLine = client->readStringUntil('\n');
      chunkSizeLine.trim();
      if (chunkSizeLine.length() == 0) continue;
      
      // 16進数のチャンクサイズを読み取り
      int chunkSize = (int)strtol(chunkSizeLine.c_str(), NULL, 16);
      if (chunkSize == 0) break; // 最終チャンク
      
      // チャンクデータを読み取り
      char* chunk = (char*)malloc(chunkSize + 1);
      if (chunk) {
        int bytesRead = client->readBytes(chunk, chunkSize);
        chunk[bytesRead] = '\0';
        payload += String(chunk);
        free(chunk);
      }
      
      // チャンク末尾の \r\n を読み飛ばし
      client->readStringUntil('\n');
    }
  } else {
    // 通常の読み取り
    payload = client->readString();
  }

  client->stop();
  delete client;
  return payload;
}


String json_ChatString = 
"{\"model\": \"gpt-4o-mini\","
"\"messages\": [{\"role\": \"user\", \"content\": \"\"}],"
"\"functions\": [],"
"\"function_call\":\"auto\""
"}";

ChatGPT::ChatGPT(llm_param_t param, int _promptMaxSize)
  : LLMBase(param, _promptMaxSize)
{
  M5.Lcd.println("MCP Servers:");
  for(int i=0; i<param.llm_conf.nMcpServers; i++){
    mcp_client[i] = new MCPClient(param.llm_conf.mcpServer[i].url, 
                                  param.llm_conf.mcpServer[i].port);
    
    if(mcp_client[i]->isConnected()){
      M5.Lcd.println(param.llm_conf.mcpServer[i].name);
    }
  }

  if(promptMaxSize != 0){
    load_role();
  }
  else{
    Serial.println("Prompt buffer is disabled");
  }
}


bool ChatGPT::init_chat_doc(const char *data)
{
  DeserializationError error = deserializeJson(chat_doc, data);
  if (error) {
    Serial.println("DeserializationError");

    String json_str; //= JSON.stringify(chat_doc);
    serializeJsonPretty(chat_doc, json_str);  // 文字列をシリアルポートに出力する
    Serial.println(json_str);

    return false;
  }
  String json_str; //= JSON.stringify(chat_doc);
  serializeJsonPretty(chat_doc, json_str);  // 文字列をシリアルポートに出力する
//  Serial.println(json_str);
  return true;
}

bool ChatGPT::save_role(){
  InitBuffer="";
  serializeJson(chat_doc, InitBuffer);
  Serial.println("InitBuffer = " + InitBuffer);

  // SPIFFSをマウントする
  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return false;
  }

  // JSONファイルを作成または開く
  File file = SPIFFS.open("/data.json", "w");
  if(!file){
    Serial.println("Failed to open file for writing");
    return false;
  }

  // JSONデータをシリアル化して書き込む
  serializeJson(chat_doc, file);
  file.close();
  return true;
}

void ChatGPT::load_role(){

  if(SPIFFS.begin(true)){
    File file = SPIFFS.open("/data.json", "r");
    if(file){
      DeserializationError error = deserializeJson(chat_doc, file);
      if(error){
        Serial.println("Failed to deserialize JSON. Init doc by default.");
        init_chat_doc(json_ChatString.c_str());
      }
      else{
        //const char* role = chat_doc["messages"][1]["content"];
        String role = String((const char*)chat_doc["messages"][1]["content"]);
        
        //Serial.println(role);

        if (role != "") {
          init_chat_doc(json_ChatString.c_str());
          JsonArray messages = chat_doc["messages"];
          JsonObject systemMessage1 = messages.createNestedObject();
          systemMessage1["role"] = "system";
          systemMessage1["content"] = role;
          //serializeJson(chat_doc, InitBuffer);
        } else {
          init_chat_doc(json_ChatString.c_str());
        }
      }

    } else {
      Serial.println("Failed to open file for reading");
      init_chat_doc(json_ChatString.c_str());
    }

  } else {
    Serial.println("An Error has occurred while mounting SPIFFS");
    init_chat_doc(json_ChatString.c_str());
  }


  /*
   * MCP tools listをfunctionとして挿入
   */
  for(int s=0; s<param.llm_conf.nMcpServers; s++){
    if(!mcp_client[s]->isConnected()){
      continue;
    }

    for(int t=0; t<mcp_client[s]->nTools; t++){
      chat_doc["functions"].add(mcp_client[s]->toolsListDoc["result"]["tools"][t]);
    }
  }

  /*
   * FunctionCall.cppで定義したfunctionを挿入
   */
  SpiRamJsonDocument functionsDoc(1024*10);
  DeserializationError error = deserializeJson(functionsDoc, json_Functions.c_str());
  if (error) {
    Serial.println("load_role: JSON deserialization error");
  }

  int nFuncs = functionsDoc.size();
  for(int i=0; i<nFuncs; i++){
    chat_doc["functions"].add(functionsDoc[i]);
  }

  /*
   * InitBufferを初期化
   */
  serializeJson(chat_doc, InitBuffer);
  String json_str; 
  serializeJsonPretty(chat_doc, json_str);  // 文字列をシリアルポートに出力する
  Serial.println("Initialized prompt:");
  Serial.println(json_str);
}


#define MAX_REQUEST_COUNT  (10)
void ChatGPT::chat(String text, const char *base64_buf) {
  static String response = "";
  String calledFunc = "";
  //String funcCallMode = "auto";
  bool image_flag = false;
  
  // 画像URL文字列をchat()関数のスコープで保持（forループ内で作成すると、serializeJson時に無効になる）
  String image_url_str = "";
  if(base64_buf != NULL){
    image_url_str = String("data:image/jpeg;base64,") + String(base64_buf);
  }

  //Serial.println(InitBuffer);
  //init_chat_doc(InitBuffer.c_str());

  // 質問をチャット履歴に追加
  if(base64_buf == NULL){
    chatHistory.push_back(String("user"), String(""), text);
  }
  else{
    //画像が入力された場合は第2引数を"image"にして識別する
    chatHistory.push_back(String("user"), String("image"), text);
  }

  // functionの実行が要求されなくなるまで繰り返す
  for (int reqCount = 0; reqCount < MAX_REQUEST_COUNT; reqCount++)
  {
    init_chat_doc(InitBuffer.c_str());

    //if(reqCount == (MAX_REQUEST_COUNT - 1)){
    //  funcCallMode = String("none");
    //}

    for (int i = 0; i < chatHistory.get_size(); i++)
    {
      JsonArray messages = chat_doc["messages"];
      JsonObject systemMessage1 = messages.createNestedObject();

      if (systemMessage1.isNull()) {
        Serial.println("[ChatGPT] ERROR: createNestedObject() failed (out of memory?)");
        break;
      }

      if(chatHistory.get_role(i).equals(String("function"))){
        //Function Callingの場合
        systemMessage1["role"] = chatHistory.get_role(i);
        systemMessage1["name"] = chatHistory.get_funcName(i);
        systemMessage1["content"] = chatHistory.get_content(i);
      }
      else if(chatHistory.get_funcName(i).equals(String("image"))){
        //画像がある場合
        //このようなJSONを作成する
        // messages=[
        //      {"role": "user", "content": [
        //          {"type": "text", "text": "この三角形の面積は？"},
        //          {"type": "image_url", "image_url": {"url": f"data:image/png;base64,{base64_image}"}}
        //      ]}
        //  ],

        systemMessage1["role"] = chatHistory.get_role(i);
        JsonObject content_text = systemMessage1["content"].createNestedObject();
        content_text["type"] = "text";
        content_text["text"] = chatHistory.get_content(i);
        JsonObject content_image = systemMessage1["content"].createNestedObject();
        content_image["type"] = "image_url";
        // image_url_strは関数スコープで定義されているため、serializeJson()時も有効
        content_image["image_url"]["url"] = image_url_str;

        //次回以降は画像の埋め込みをしないよう、識別用の文字列"image"を消す
        chatHistory.set_funcName(i, "");
      }
      else{
        systemMessage1["role"] = chatHistory.get_role(i);
        systemMessage1["content"] = chatHistory.get_content(i);
      }

    }

    if (chat_doc.overflowed()) {
      Serial.printf("[ChatGPT] WARNING: chat_doc overflowed. capacity=%u, measured=%u\n",
                    (unsigned)chat_doc.capacity(), (unsigned)measureJson(chat_doc));
    }

    // 巨大JSONはString化せず、必要な情報だけ表示
    Serial.println("====================");
    Serial.printf("[ChatGPT] doc.measured=%u, doc.capacity=%u, overflowed=%d\n",
            (unsigned)measureJson(chat_doc), (unsigned)chat_doc.capacity(), (int)chat_doc.overflowed());
    // urlフィールドの存在確認（nullならここで確定）
    const char* url = chat_doc["messages"][1]["content"][1]["image_url"]["url"];
    if (!url) {
        Serial.println("[ChatGPT] image_url.url is NULL in document");
      } else {
        Serial.printf("[ChatGPT] image_url.url len=%u\n", (unsigned)strlen(url));
        if (strncmp(url, "data:image/", 11) != 0) {
          Serial.println("[ChatGPT] WARNING: image_url.url does not start with data:image/");
        }
    }
    Serial.println("====================");

    response = execChatGpt(chat_doc, calledFunc);


    if(calledFunc == ""){   // Function Callなし ／ Function Call繰り返しの完了
      chatHistory.push_back(String("assistant"), String(""), response);   // 返答をチャット履歴に追加
      robot->speech(response);
      break;
    }
    else{   // Function Call繰り返し中。ループを継続
      chatHistory.push_back(String("function"), calledFunc, response);   // 返答をチャット履歴に追加   
    }

  }

  //チャット履歴の容量を圧迫しないように、functionロールを削除する
  chatHistory.clean_function_role();
}


String ChatGPT::execChatGpt(const JsonDocument& doc, String& calledFunc) {
  String response = "";
  avatar.setExpression(Expression::Doubt);
  avatar.setSpeechFont(&fonts::efontJA_16);
  avatar.setSpeechText("考え中…");
  
  // Robotクラスの待機音声タスクを起動
  robot->startIdlePhraseTask();
  
  String ret = https_post_json("https://api.openai.com/v1/chat/completions", doc, root_ca_openai);
  
  // 待機音声タスクを停止
  robot->stopIdlePhraseTask();
  
  avatar.setExpression(Expression::Neutral);
  avatar.setSpeechText("");
  
  // デバッグ: レスポンスの先頭200文字を確認
  Serial.printf("[execChatGpt] Response length: %d bytes\n", ret.length());
  if(ret.length() > 0) {
    String prefix = ret.substring(0, min(200, (int)ret.length()));
    Serial.println("[execChatGpt] Response prefix:");
    Serial.println(prefix);
    
    // JSONの開始位置を検索
    int jsonStart = ret.indexOf('{');
    if(jsonStart > 0) {
      Serial.printf("[execChatGpt] WARNING: JSON starts at position %d (not 0). Removing HTTP headers.\n", jsonStart);
      ret = ret.substring(jsonStart);
    }
  }
  
  if(ret != ""){
    SpiRamJsonDocument retDoc(200 * 1024);  // 画像を含むGPT-4oの長い応答に対応
    DeserializationError error = deserializeJson(retDoc, ret.c_str());
    if (error) {
      Serial.print(F("deserializeJson() failed: "));
      Serial.println(error.f_str());
      avatar.setExpression(Expression::Sad);
      avatar.setSpeechText("エラーです");
      response = "エラーです";
      delay(1000);
      avatar.setSpeechText("");
      avatar.setExpression(Expression::Neutral);
    }else{
      // OpenAI errorレスポンス
      const char* errMsg = retDoc["error"]["message"]; 
      if (errMsg && errMsg[0] != '\0') {
        Serial.println(errMsg);
        avatar.setExpression(Expression::Sad);
        avatar.setSpeechText("エラーです");
        response = "エラーです";
        delay(1000);
        avatar.setSpeechText("");
        avatar.setExpression(Expression::Neutral);
        calledFunc = String("");
        return response;
      }

      const char* data = retDoc["choices"][0]["message"]["content"];
      
      // content = nullならfunction call
      if(data == 0){
        const char* name = retDoc["choices"][0]["message"]["function_call"]["name"];
        const char* args = retDoc["choices"][0]["message"]["function_call"]["arguments"];

        //avatar.setSpeechFont(&fonts::efontJA_12);
        //avatar.setSpeechText(name);
        response = exec_calledFunc(name, args);
      }
      else{
        Serial.println(data);
        response = String(data);
        std::replace(response.begin(),response.end(),'\n',' ');
        calledFunc = String("");
      }
    }
  } else {
    avatar.setExpression(Expression::Sad);
    avatar.setSpeechFont(&fonts::efontJA_16);
    avatar.setSpeechText("わかりません");
    response = "わかりません";
    delay(1000);
    avatar.setSpeechText("");
    avatar.setExpression(Expression::Neutral);
  }
  return response;
}


String ChatGPT::exec_calledFunc(const char* name, const char* args){
  String response = "";

  Serial.println(name);
  Serial.println(args);

  DynamicJsonDocument argsDoc(256);
  DeserializationError error = deserializeJson(argsDoc, args);
  if (error) {
    Serial.print(F("deserializeJson(arguments) failed: "));
    Serial.println(error.f_str());
    avatar.setExpression(Expression::Sad);
    avatar.setSpeechText("エラーです");
    response = "エラーです";
    delay(1000);
    avatar.setSpeechText("");
    avatar.setExpression(Expression::Neutral);
  }else{

    //関数名がいずれかのMCPサーバに属するかを検索し、ヒットしたらリクエストを送信する
    for(int s=0; s<param.llm_conf.nMcpServers; s++){
      if(mcp_client[s]->search_tool(String(name))){
        DynamicJsonDocument tool_params(512);
        tool_params["name"] = String(name);
        tool_params["arguments"] = argsDoc;
        response = mcp_client[s]->mcp_call_tool(tool_params);
        goto END;
      }
    }


    if(strcmp(name, "timer") == 0){
      const int time = argsDoc["time"];
      const char* action = argsDoc["action"];
      Serial.printf("time:%d\n",time);
      Serial.println(action);

      response = timer(time, action);
    }
    else if(strcmp(name, "timer_change") == 0){
      const int time = argsDoc["time"];
      response = timer_change(time);    
    }
    else if(strcmp(name, "get_date") == 0){
      response = get_date();    
    }
    else if(strcmp(name, "get_time") == 0){
      response = get_time();    
    }
    else if(strcmp(name, "get_week") == 0){
      response = get_week();    
    }
#if defined(USE_EXTENSION_FUNCTIONS)
    else if(strcmp(name, "reminder") == 0){
      const int hour = argsDoc["hour"];
      const int min = argsDoc["min"];
      const char* text = argsDoc["text"];
      response = reminder(hour, min, text);
    }
    else if(strcmp(name, "ask") == 0){
      const char* text = argsDoc["text"];
      Serial.println(text);
      response = ask(text);
    }
    else if(strcmp(name, "save_note") == 0){
      const char* text = argsDoc["text"];
      Serial.println(text);
      response = save_note(text);
    }
    else if(strcmp(name, "read_note") == 0){
      response = read_note();    
    }
    else if(strcmp(name, "delete_note") == 0){
      response = delete_note();    
    }
    else if(strcmp(name, "get_bus_time") == 0){
      const int nNext = argsDoc["nNext"];
      Serial.printf("nNext:%d\n",nNext);   
      response = get_bus_time(nNext);    
    }
    else if(strcmp(name, "send_mail") == 0){
      const char* text = argsDoc["message"];
      Serial.println(text);
      response = send_mail(text);
    }
    else if(strcmp(name, "read_mail") == 0){
      response = read_mail();    
    }
#if defined(ARDUINO_M5STACK_CORES3)
    else if(strcmp(name, "register_wakeword") == 0){
      response = register_wakeword();    
    }
    else if(strcmp(name, "wakeword_enable") == 0){
      response = wakeword_enable();    
    }
    else if(strcmp(name, "delete_wakeword") == 0){
      const int idx = argsDoc["idx"];
      Serial.printf("idx:%d\n",idx);   
      response = delete_wakeword(idx);    
    }
#endif  //defined(ARDUINO_M5STACK_CORES3)
#if !defined(MCP_BRAVE_SEARCH)
    else if(strcmp(name, "get_news") == 0){
      response = get_news();    
    }
#endif
    else if(strcmp(name, "get_weathers") == 0){
      response = get_weathers();    
    }
#endif  //if defined(USE_EXTENSION_FUNCTIONS)

  }

END:
  return response;
}