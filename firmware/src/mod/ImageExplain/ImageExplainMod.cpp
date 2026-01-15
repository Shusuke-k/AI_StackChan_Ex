#include <Arduino.h>
#include <SD.h>
#include <SPIFFS.h>
#include "mod/ModManager.h"
#include "ImageExplainMod.h"
#include <Avatar.h>
#include "Robot.h"
#include <base64.h>

using namespace m5avatar;

/// 外部参照 ///
extern Avatar avatar;
extern Robot* robot;
extern bool servo_home;
extern void sw_tone();

// WebAPI.cppで定義されたグローバル変数
extern String g_uploadedImagePath;
extern bool g_imageUploaded;
extern String g_base64ImageBuffer;
extern String g_imageQuestion;
///////////////

ImageExplainMod::ImageExplainMod(bool _isOffline)
  : processing{false}, conversationMode{false}
{
  box_BtnA.setupBox(0, 100, 40, 60);
  box_BtnB.setupBox(140, 100, 40, 60);
  box_BtnC.setupBox(280, 100, 40, 60);
  box_stt.setupBox(0, 0, M5.Display.width(), 60);  // 画面上部をタッチで音声入力
}

void ImageExplainMod::init(void)
{
  avatar.setSpeechText("画像説明モード");
  avatar.setSpeechFont(&fonts::efontJA_12);
  delay(1000);
  avatar.setSpeechText("画像を送信してください");
  conversationMode = false;
  Serial.println("===========================================");
  Serial.println("Image Explain Mode");
  Serial.println("Send image via web interface");
  Serial.println("URL: http://<IP address>/image_upload_page");
  Serial.println("Touch screen to ask questions");
  Serial.println("===========================================");
}

void ImageExplainMod::pause(void)
{
  avatar.setSpeechFont(&fonts::efontJA_16);
  avatar.setSpeechText("");
}

void ImageExplainMod::btnA_pressed(void)
{
  sw_tone();
  // 最後に受信した画像を再処理
  if(lastImagePath != "" && !processing){
    avatar.setSpeechText("再処理中...");
    processImage(lastImagePath);
  }
}

void ImageExplainMod::btnB_pressed(void)
{
  Serial.printf("btnB_pressed: conversationMode=%d, processing=%d\n", conversationMode, processing);
  sw_tone();
  // 音声入力で追加質問
  if(!processing && conversationMode){
    avatar.setExpression(Expression::Happy);
    avatar.setSpeechText("御用でしょうか？");
    
    String ret = robot->listen();
    avatar.setSpeechText("");
    
    Serial.println("音声認識終了");
    if(ret != "") {
      Serial.println("追加質問: " + ret);
      // 画像を付けずに通常の会話として続ける
      robot->chat(ret, NULL);
      avatar.setSpeechText("");
      avatar.setExpression(Expression::Neutral);
    } else {
      Serial.println("音声認識失敗");
      avatar.setExpression(Expression::Sad);
      avatar.setSpeechText("聞き取れませんでした");
      delay(2000);
      avatar.setSpeechText("");
      avatar.setExpression(Expression::Neutral);
    }
  }
  else if(!processing){
    avatar.setSpeechText("画像を送信してください");
    delay(1000);
    avatar.setSpeechText("画像を送信してください");
  }
}

void ImageExplainMod::btnC_pressed(void)
{
  sw_tone();
  // 画像履歴と会話履歴のクリア
  lastImagePath = "";
  conversationMode = false;
  // 会話履歴をクリア（LLMのchatHistory）
  if(robot->llm != NULL){
    robot->llm->clear_history();
  }
  avatar.setSpeechText("履歴をクリア");
  delay(1000);
  avatar.setSpeechText("画像を送信してください");
}

void ImageExplainMod::display_touched(int16_t x, int16_t y)
{
  Serial.printf("Touch detected: x=%d, y=%d, conversationMode=%d, processing=%d\n", x, y, conversationMode, processing);
  
  if (box_stt.contain(x, y))
  {
    Serial.println("Touch in STT area");
    // 画面上部をタッチで音声入力（会話継続モード時）
    if(conversationMode && !processing){
      Serial.println("Starting voice input...");
      btnB_pressed();
    } else {
      Serial.printf("Voice input blocked: conversationMode=%d, processing=%d\n", conversationMode, processing);
    }
  }
  else if (box_BtnA.contain(x, y))
  {
    btnA_pressed();
  }
  else if (box_BtnB.contain(x, y))
  {
    btnB_pressed();
  }
  else if (box_BtnC.contain(x, y))
  {
    btnC_pressed();
  }
}

void ImageExplainMod::idle(void)
{
  // アップロードされた画像があるかチェック
  if(g_imageUploaded && !processing) {
    g_imageUploaded = false;  // フラグをクリア
    Serial.println("New image detected, processing...");
    processImage(g_uploadedImagePath);
  }
}

void ImageExplainMod::processImage(const String& imagePath)
{
  if(processing){
    Serial.println("Already processing...");
    return;
  }
  
  processing = true;
  lastImagePath = imagePath;
  
  avatar.setExpression(Expression::Doubt);
  avatar.setSpeechText("画像を解析中...");
  
  // 待機音声タスクを起動
  robot->startIdlePhraseTask();
  
  Serial.println("Processing image: " + imagePath);
  
  // SDカードから画像を読み込む
  File imageFile = SD.open(imagePath.c_str(), FILE_READ);
  if (!imageFile) {
    Serial.println("Failed to open image file");
    avatar.setExpression(Expression::Sad);
    avatar.setSpeechText("画像ファイルが開けません");
    processing = false;
    delay(2000);
    avatar.setSpeechText("画像を送信してください");
    avatar.setExpression(Expression::Neutral);
    return;
  }
  
  // ファイルサイズを取得
  size_t fileSize = imageFile.size();
  Serial.printf("Image file size: %d bytes\n", fileSize);
  
  // ファイルサイズチェック（緩和: 5MBまで許可）
  if(fileSize > 1024 * 1024 * 5) {  // 5MB以上
    Serial.println("Image file too large (>5MB)");
    avatar.setExpression(Expression::Sad);
    avatar.setSpeechText("画像が大きすぎます");
    imageFile.close();
    processing = false;
    delay(2000);
    avatar.setSpeechText("画像を送信してください");
    avatar.setExpression(Expression::Neutral);
    return;
  }
  
  // ストリーミングbase64エンコード（チャンク処理でメモリ節約）
  Serial.println("Streaming Base64 encode...");
  const size_t CHUNK_SIZE = 3072; // 3KBチャンク（base64効率のため3の倍数）
  uint8_t* chunkBuffer = (uint8_t*)malloc(CHUNK_SIZE);
  if(!chunkBuffer) {
    Serial.println("Failed to allocate chunk buffer");
    avatar.setExpression(Expression::Sad);
    avatar.setSpeechText("メモリ不足です");
    imageFile.close();
    processing = false;
    delay(2000);
    avatar.setSpeechText("画像を送信してください");
    avatar.setExpression(Expression::Neutral);
    return;
  }
  
  // Base64文字列を段階的に構築（グローバルバッファを使用）
  g_base64ImageBuffer = "";
  g_base64ImageBuffer.reserve((fileSize * 4 / 3) + 4); // 事前に容量確保
  
  size_t totalRead = 0;
  Serial.printf("File size: %d bytes, encoding in chunks...\n", fileSize);
  
  while(totalRead < fileSize) {
    size_t toRead = min((size_t)CHUNK_SIZE, fileSize - totalRead);
    size_t bytesRead = imageFile.read(chunkBuffer, toRead);
    
    if(bytesRead > 0) {
      // チャンクをbase64エンコードして追加
      String encoded = base64::encode(chunkBuffer, bytesRead);
      g_base64ImageBuffer += encoded;
      totalRead += bytesRead;
      
      // 進捗表示（10%刻み）
      if(totalRead % (fileSize / 10 + 1) == 0 || totalRead == fileSize) {
        Serial.printf("Encoded: %d%%\n", (totalRead * 100) / fileSize);
      }
    } else {
      break;
    }
  }
  
  free(chunkBuffer);
  imageFile.close();
  
  if(totalRead != fileSize) {
    Serial.printf("Encoding incomplete: %d/%d bytes\n", totalRead, fileSize);
    avatar.setExpression(Expression::Sad);
    avatar.setSpeechText("画像の読み込みエラー");
    g_base64ImageBuffer = "";
    processing = false;
    delay(2000);
    avatar.setSpeechText("画像を送信してください");
    avatar.setExpression(Expression::Neutral);
    return;
  }
  
  Serial.printf("Base64 encoded: %d bytes → %d bytes\n", fileSize, g_base64ImageBuffer.length());
  
  // 待機音声タスクを一時停止（LLM処理中に再度起動される）
  robot->stopIdlePhraseTask();
  
  // GPT-4 Visionに送信して解析
  avatar.setSpeechText("AIに問い合わせ中...");
  
  // 質問文があればそれを使用、なければデフォルトの質問
  String question = "";
  if (g_imageQuestion != "") {
    question = g_imageQuestion;
    Serial.println("Using custom question: " + question);
  } else {
    question = "この画像について感想を述べてください。";
  }
  
  Serial.printf("[ImageExplain] Question: %s\n", question.c_str());
  Serial.printf("[ImageExplain] Base64 length: %d\n", g_base64ImageBuffer.length());
  
  // Robotクラスのchat関数を使用（画像付き）
  // グローバルバッファを使用することでメモリコピーを避ける
  robot->chat(question, g_base64ImageBuffer.c_str());
  
  avatar.setExpression(Expression::Neutral);
  
  // Base64バッファと質問文をクリア（メモリ節約）
  g_base64ImageBuffer = "";
  g_imageQuestion = "";
  
  processing = false;
  conversationMode = true;  // 会話継続モードを有効化
  
  Serial.println("Image processing completed");
  Serial.println("=== Conversation mode ENABLED ===");
  Serial.println("You can now ask follow-up questions");
  avatar.setSpeechText("追加で質問できます");
  delay(2000);
  avatar.setSpeechText("画面をタッチしてください");
}
