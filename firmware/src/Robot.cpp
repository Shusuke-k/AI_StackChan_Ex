#include "Robot.h"
#include <SD.h>
#include "tts/WebVoiceVoxTTS.h"
#include "tts/ElevenLabsTTS.h"
#include "tts/OpenAITTS.h"
#include "tts/UAquesTalkTTS.h"
#include "tts/ModuleLLMTTS.h"
#include "stt/CloudSpeechClient.h"
#include "stt/Whisper.h"
#include "stt/ModuleLLMASR.h"
#include "stt/ModuleLLMWhisper.h"
#include "driver/ModuleLLM.h"
#include "llm/LLMBase.h"
#include "llm/ChatGPT/ChatGPT.h"
#include "llm/ChatGPT/RealtimeChatGPT.h"
#include "llm/ModuleLLM/ChatModuleLLM.h"
#include "llm/ModuleLLMFncl/ChatModuleLLMFncl.h"
#include "Avatar.h"

using namespace m5avatar;

extern Avatar avatar;
extern bool servo_home;

// 待機音声タスク用のグローバル変数
static volatile bool g_idlePhraseTaskRunning = false;
static TaskHandle_t g_idlePhraseTaskHandle = NULL;

void idlePhraseLoopTask(void* arg) {
  Robot* robot = (Robot*)arg;
  Serial.println("[IdlePhraseTask] Started");
  
  while(g_idlePhraseTaskRunning) {
    robot->playRandomIdlePhrase();
    
    // 1秒待機（タスク終了チェックを100ms毎に実行）
    for(int i = 0; i < 10 && g_idlePhraseTaskRunning; i++) {
      vTaskDelay(100 / portTICK_PERIOD_MS);
    }
  }
  
  Serial.println("[IdlePhraseTask] Stopped");
  g_idlePhraseTaskHandle = NULL;
  vTaskDelete(NULL);
}

//#if defined(REALTIME_API_WITH_TTS)
// TTS非同期実行用のタスク
//
void asyncTtsStreamTask(void *arg) {
  Serial.println("TTS stream task created");
  Robot* pRt = (Robot*)arg;

  while(1){
    if(pRt->llm->getOutputTextQueueSize() != 0){
      Serial.println("TTS stream task: get text form queue");
      pRt->asyncPlaying = true;
      String ttsText = pRt->llm->getOutputText();
      pRt->tts->stream(ttsText);
      pRt->asyncPlaying = false;
    }
    delay(1);
  }
}

//#endif

Robot::Robot(StackchanExConfig& config) : m_config(config)
{
  // Servo setting
  //
  servo = new ServoCustom();
  servo->begin(config.getServoInfo(AXIS_X)->pin, config.getServoInfo(AXIS_X)->start_degree,
              config.getServoInfo(AXIS_X)->offset,
              config.getServoInfo(AXIS_Y)->pin, config.getServoInfo(AXIS_Y)->start_degree,
              config.getServoInfo(AXIS_Y)->offset,
              (ServoType)config.getServoType());

  // AI service setting
  //
#if defined(REALTIME_API)
  //LLM setting
  api_keys_s* api_key = config.getAPISetting();
  llm_param_t llm_param;
  llm_param.api_key = api_key->ai_service;
  llm_param.llm_conf = config.getExConfig().llm;
  llm = new RealtimeChatGPT(llm_param);

  #if defined(REALTIME_API_WITH_TTS)
    initTTS(config);
    invokeAsyncTtsStreamTask();
  #endif

#else //REALTIME_API

#if defined(USE_LLM_MODULE)
  module_llm_param = module_llm_param_t();
#endif
  
  initLLM(config);
  initTTS(config);
  initSTT(config);

  // ModuleLLM initialize
  //
#if defined(USE_LLM_MODULE)
  module_llm_param.rxPin = config.getExConfig().moduleLLM.rxPin;
  module_llm_param.txPin = config.getExConfig().moduleLLM.txPin;
  int wakeword_type = config.getExConfig().wakeword.type;
  if(wakeword_type == WAKEWORD_TYPE_MODULE_LLM_KWS){
    module_llm_param.enableKWS = true;
    module_llm_param.wake_up_keyword = config.getExConfig().wakeword.keyword;
  }
  module_llm_setup(module_llm_param);

  // TTSがModuleLLMではなくAquesTalkの場合はTTS非同期実行用タスクを起動する
  if(!module_llm_param.enableTTS){
    invokeAsyncTtsStreamTask();
  }
#endif

#endif  //REALTIME_API

}

bool Robot::isAllOfflineService()
{
  return llm->isOfflineService && stt->isOfflineService && tts->isOfflineService;
}

void Robot::initLLM(StackchanExConfig& config){
  int llm_type = config.getExConfig().llm.type;
  api_keys_s* api_key = config.getAPISetting();

  llm_param_t llm_param;
  llm_param.api_key = api_key->ai_service;
  llm_param.llm_conf = config.getExConfig().llm;

  switch(llm_type){
  case LLM_TYPE_CHATGPT:
    llm = new ChatGPT(llm_param);
    break;
  case LLM_TYPE_MODULE_LLM:
#if defined(USE_LLM_MODULE)
    llm = new ChatModuleLLM(llm_param);
    module_llm_param.enableLLM = true;
    module_llm_param.m5llm_config = m5_module_llm::ApiLlmSetupConfig_t(); //default setting
    if(llm_param.llm_conf.model != ""){
      module_llm_param.m5llm_config.model = llm_param.llm_conf.model;
    }
    module_llm_param.m5llm_config.max_token_len = 511;
#else
    Serial.println("ModuleLLM is not enabled. Please setup in platformio.ini");
    llm = nullptr;
#endif
    break;
  case LLM_TYPE_MODULE_LLM_FNCL:
#if defined(USE_LLM_MODULE)
    llm = new ChatModuleLLMFncl(llm_param);
    module_llm_param.enableLLM = true;
    module_llm_param.m5llm_config.model = "SmolLM-360M-Instruct-fncl";
    module_llm_param.m5llm_config.max_token_len = 511;
#else
    Serial.println("ModuleLLM is not enabled. Please setup in platformio.ini");
    llm = nullptr;
#endif
    break;
  default:
    Serial.printf("Error: undefined STT type %d\n", llm_type);
    llm = nullptr;
  }
}

void Robot::initSTT(StackchanExConfig& config){
  int stt_type = config.getExConfig().stt.type;
  api_keys_s* api_key = config.getAPISetting();

  stt_param_t stt_param;
  stt_param.api_key = api_key->stt;
  stt_param.stt_conf = config.getExConfig().stt;
  
  switch(stt_type){
  case STT_TYPE_GOOGLE:
    stt = new CloudSpeechClient(stt_param);
    break;
  case STT_TYPE_OPENAI_WHISPER:
    stt = new Whisper(stt_param);
    break;
  case STT_TYPE_MODULE_LLM_ASR:
#if defined(USE_LLM_MODULE)
    stt = new ModuleLLMASR();
    module_llm_param.enableASR = true;
#else
    Serial.println("ModuleLLM is not enabled. Please setup in platformio.ini");
    stt = nullptr;
#endif
    break;
  case STT_TYPE_MODULE_LLM_WHISPER:
#if defined(USE_LLM_MODULE)
    stt = new ModuleLLMWhisper();
    module_llm_param.enableWhisper = true;
    if(stt_param.stt_conf.model != ""){
      module_llm_param.whisper_model = stt_param.stt_conf.model;
    }
#else
    Serial.println("ModuleLLM is not enabled. Please setup in platformio.ini");
    stt = nullptr;
#endif
    break;
  default:
    Serial.printf("Error: undefined STT type %d\n", stt_type);
    stt = nullptr;
  }

}

void Robot::initTTS(StackchanExConfig& config){
  int tts_type = config.getExConfig().tts.type;
  api_keys_s* api_key = config.getAPISetting();

  tts_param_t tts_param;
  tts_param.api_key = api_key->tts;
  tts_param.model = config.getExConfig().tts.model;
  tts_param.voice = config.getExConfig().tts.voice;
  switch(tts_type){
  case TTS_TYPE_WEB_VOICEVOX:
    tts = new WebVoiceVoxTTS(tts_param);
    break;
  case TTS_TYPE_ELEVENLABS:
    tts = new ElevenLabsTTS(tts_param);
    break;
  case TTS_TYPE_OPENAI:
    tts_param.api_key = api_key->ai_service;    //API KeyはChatGPTと共通
    tts = new OpenAITTS(tts_param);
    break;
  case TTS_TYPE_AQUESTALK:
#if defined(USE_AQUESTALK)
    tts = new UAquesTalkTTS();
#else
    Serial.println("AquesTalk is not enabled. Please define USE_AQUESTALK.");
    tts = nullptr;
#endif
    break;
  case TTS_TYPE_MODULE_LLM:
#if defined(USE_LLM_MODULE)
    tts = new ModuleLLMTTS();
    module_llm_param.enableTTS = true;
    module_llm_param.model = tts_param.model;
#else
    Serial.println("ModuleLLM is not enabled. Please setup in platformio.ini");
    tts = nullptr;
#endif
    break;
  default:
    Serial.printf("Error: undefined TTS type %d\n", tts_type);
    tts = nullptr;
  }

}

void Robot::speech(String text)
{
  if(text != ""){
    servo_home = false;
    avatar.setExpression(Expression::Happy);

    tts->stream(text);

    avatar.setExpression(Expression::Neutral);
    servo_home = true;
  }
}

void Robot::invokeAsyncTtsStreamTask(void)
{
  asyncPlaying = false;

  xTaskCreate(asyncTtsStreamTask, /* Function to implement the task */
            "asyncTtsStreamTask", /* Name of the task */
            5*1024,               /* Stack size in words */
            this,                 /* Task input parameter */
            2,                    /* Priority of the task */
            NULL);                /* Task handle. */
}

String Robot::listen()
{
  // 即座にフィードバック音を鳴らす
  servo_home = false;
  avatar.setExpression(Expression::Happy);
  avatar.setSpeechText("聞いています...");
  
  String ret = stt->speech_to_text();
  
  avatar.setSpeechText("");
  avatar.setExpression(Expression::Neutral);
  servo_home = true;
  
  return ret;
}

void Robot::chat(String text, const char *base64_buf)
{
  llm->chat(text, base64_buf);
}

// 待機中音声キャッシュ機能
//
void Robot::initIdlePhraseCache()
{
  idle_phrases_s idle_phrases = m_config.getExConfig().idle_phrases;
  
  if(idle_phrases.count == 0){
    Serial.println("[IdlePhrase] No phrases configured");
    return;
  }

  Serial.printf("[IdlePhrase] Initializing cache for %d phrases\n", idle_phrases.count);
  
  // キャッシュディレクトリ作成
  if(!SD.exists("/cache")){
    SD.mkdir("/cache");
  }

  // 各フレーズをTTSで生成してキャッシュ
  for(int i=0; i<idle_phrases.count; i++){
    String cache_path = "/cache/idle_" + String(i) + ".mp3";
    
    // キャッシュが既に存在する場合はスキップ
    if(SD.exists(cache_path)){
      Serial.printf("[IdlePhrase] Cache exists: %s\n", cache_path.c_str());
      continue;
    }

    Serial.printf("[IdlePhrase] Generating: %s -> %s\n", 
                  idle_phrases.phrases[i].c_str(), cache_path.c_str());
    
    // TTSでMP3生成してSDに保存
    bool success = tts->save_to_file(idle_phrases.phrases[i], cache_path);
    
    if(success){
      Serial.printf("[IdlePhrase] Saved: %s\n", cache_path.c_str());
    } else {
      Serial.printf("[IdlePhrase] Failed to save: %s\n", cache_path.c_str());
    }
    
    delay(100);
  }
  
  Serial.println("[IdlePhrase] Cache initialization completed");
}

void Robot::playRandomIdlePhrase()
{
  idle_phrases_s idle_phrases = m_config.getExConfig().idle_phrases;
  
  if(idle_phrases.count == 0){
    return;
  }

  // ランダムにフレーズを選択
  int idx = random(idle_phrases.count);
  String cache_path = "/cache/idle_" + String(idx) + ".mp3";
  
  // キャッシュファイルが存在するか確認
  if(!SD.exists(cache_path)){
    Serial.printf("[IdlePhrase] Cache not found: %s\n", cache_path.c_str());
    // キャッシュがない場合は直接TTSで再生
    speech(idle_phrases.phrases[idx]);
    return;
  }

  // キャッシュから再生
  Serial.printf("[IdlePhrase] Playing: %s (%s)\n", 
                idle_phrases.phrases[idx].c_str(), cache_path.c_str());
  
  tts->play_from_file(cache_path);
}

void Robot::startIdlePhraseTask()
{
  // 既に起動中の場合は何もしない
  if(g_idlePhraseTaskRunning) {
    return;
  }
  
  // キャッシュが存在しない場合は起動しない
  if(!idlePhraseCacheExists()) {
    Serial.println("[IdlePhraseTask] Cache not found, task not started");
    return;
  }
  
  g_idlePhraseTaskRunning = true;
  if(g_idlePhraseTaskHandle == NULL) {
    xTaskCreatePinnedToCore(
      idlePhraseLoopTask,
      "idlePhraseTask",
      4096,
      this,
      1,
      &g_idlePhraseTaskHandle,
      APP_CPU_NUM
    );
    Serial.println("[IdlePhraseTask] Task started");
  }
}

void Robot::stopIdlePhraseTask()
{
  if(!g_idlePhraseTaskRunning) {
    return;
  }
  
  g_idlePhraseTaskRunning = false;
  
  // タスクが完全に終了するまで少し待機
  int waitCount = 0;
  while(g_idlePhraseTaskHandle != NULL && waitCount < 50) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
    waitCount++;
  }
  
  Serial.println("[IdlePhraseTask] Task stopped");
}

bool Robot::idlePhraseCacheExists()
{
  idle_phrases_s idle_phrases = m_config.getExConfig().idle_phrases;
  
  if(idle_phrases.count == 0){
    return false;
  }

  // すべてのキャッシュが存在するか確認
  for(int i=0; i<idle_phrases.count; i++){
    String cache_path = "/cache/idle_" + String(i) + ".mp3";
    if(!SD.exists(cache_path)){
      return false;
    }
  }
  
  return true;
}
