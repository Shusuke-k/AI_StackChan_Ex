#include <ESP32WebServer.h>
#include <nvs.h>
#include <SD.h>
#include "WebAPI.h"
#include "Avatar.h"
#include "llm/ChatGPT/ChatGPT.h"
#include "llm/ChatGPT/FunctionCall.h"
#include "Robot.h"
#include "mod/ModManager.h"
#include "mod/ImageExplain/ImageExplainMod.h"

using namespace m5avatar;
extern Avatar avatar;
extern uint8_t m5spk_virtual_channel;
extern String STT_API_KEY;

ESP32WebServer server(80);

// 画像アップロード用のグローバル変数
String g_uploadedImagePath = "";
bool g_imageUploaded = false;
String g_base64ImageBuffer = "";  // Base64エンコードされた画像データ
String g_imageQuestion = "";     // 画像に対する質問文

// C++11 multiline string constants are neato...
static const char HEAD[] PROGMEM = R"KEWL(
<!DOCTYPE html>
<html lang="ja">
<head>
  <meta charset="UTF-8">
  <title>AIｽﾀｯｸﾁｬﾝ</title>
</head>)KEWL";

static const char APIKEY_HTML[] PROGMEM = R"KEWL(
<!DOCTYPE html>
<html>
  <head>
    <meta charset="UTF-8">
    <title>APIキー設定</title>
  </head>
  <body>
    <h1>APIキー設定</h1>
    <form>
      <label for="role1">OpenAI API Key</label>
      <input type="text" id="openai" name="openai" oninput="adjustSize(this)"><br>
      <label for="role2">VoiceVox API Key</label>
      <input type="text" id="voicevox" name="voicevox" oninput="adjustSize(this)"><br>
      <label for="role3">Speech to Text API Key</label>
      <input type="text" id="sttapikey" name="sttapikey" oninput="adjustSize(this)"><br>
      <button type="button" onclick="sendData()">送信する</button>
    </form>
    <script>
      function adjustSize(input) {
        input.style.width = ((input.value.length + 1) * 8) + 'px';
      }
      function sendData() {
        // FormDataオブジェクトを作成
        const formData = new FormData();

        // 各ロールの値をFormDataオブジェクトに追加
        const openaiValue = document.getElementById("openai").value;
        if (openaiValue !== "") formData.append("openai", openaiValue);

        const voicevoxValue = document.getElementById("voicevox").value;
        if (voicevoxValue !== "") formData.append("voicevox", voicevoxValue);

        const sttapikeyValue = document.getElementById("sttapikey").value;
        if (sttapikeyValue !== "") formData.append("sttapikey", sttapikeyValue);

	    // POSTリクエストを送信
	    const xhr = new XMLHttpRequest();
	    xhr.open("POST", "/apikey_set");
	    xhr.onload = function() {
	      if (xhr.status === 200) {
	        alert("データを送信しました！");
	      } else {
	        alert("送信に失敗しました。");
	      }
	    };
	    xhr.send(formData);
	  }
	</script>
  </body>
</html>)KEWL";

static const char ROLE_HTML[] PROGMEM = R"KEWL(
<!DOCTYPE html>
<html>
<head>
	<title>ロール設定</title>
	<meta charset="UTF-8">
	<meta name="viewport" content="width=device-width, initial-scale=1.0">
	<style>
		textarea {
			width: 80%;
			height: 200px;
			resize: both;
		}
	</style>
</head>
<body>
	<h1>ロール設定</h1>
	<form onsubmit="postData(event)">
		<label for="textarea">ここにロールを記述してください。:</label><br>
		<textarea id="textarea" name="textarea"></textarea><br><br>
		<input type="submit" value="Submit">
	</form>
	<script>
		function postData(event) {
			event.preventDefault();
			const textAreaContent = document.getElementById("textarea").value.trim();
//			if (textAreaContent.length > 0) {
				const xhr = new XMLHttpRequest();
				xhr.open("POST", "/role_set", true);
				xhr.setRequestHeader("Content-Type", "text/plain;charset=UTF-8");
			// xhr.onload = () => {
			// 	location.reload(); // 送信後にページをリロード
			// };
			xhr.onload = () => {
				document.open();
				document.write(xhr.responseText);
				document.close();
			};
				xhr.send(textAreaContent);
//        document.getElementById("textarea").value = "";
				alert("Data sent successfully!");
//			} else {
//				alert("Please enter some text before submitting.");
//			}
		}
	</script>
</body>
</html>)KEWL";

static const char DRAW_HTML[] PROGMEM = R"KEWL(
<!DOCTYPE html>
<html>
<head>
	<title>お絵かき</title>
	<meta charset="UTF-8">
	<meta name="viewport" content="width=device-width, initial-scale=1.0">
	<style>
		body { font-family: Arial, sans-serif; margin: 0; padding: 10px; background: #f0f0f0; }
		.container { max-width: 1200px; margin: 0 auto; background: white; border-radius: 10px; padding: 20px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
		h1 { text-align: center; color: #333; margin: 0 0 20px 0; }
		.toolbar { display: flex; flex-wrap: wrap; gap: 10px; padding: 15px; background: #f8f8f8; border-radius: 8px; margin-bottom: 15px; align-items: center; }
		.tool-group { display: flex; align-items: center; gap: 8px; }
		label { font-weight: bold; font-size: 14px; }
		button { padding: 10px 20px; font-size: 14px; border: none; border-radius: 5px; cursor: pointer; background: #4CAF50; color: white; transition: background 0.3s; }
		button:hover { background: #45a049; }
		button:active { transform: scale(0.98); }
		.clear-btn { background: #f44336; }
		.delete-btn { background: #ff9800; }
		input[type="color"] { width: 60px; height: 40px; border: none; border-radius: 5px; cursor: pointer; }
		input[type="range"] { width: 150px; }
		input[type="number"] { width: 60px; padding: 5px; border: 1px solid #ddd; border-radius: 4px; }
		select { padding: 8px; border: 1px solid #ddd; border-radius: 4px; background: white; }
		#drawCanvas { border: 3px solid #ff6b6b; border-radius: 8px; cursor: crosshair; display: block; margin: 0 auto; background: white; touch-action: none; box-shadow: 0 0 10px rgba(0,0,0,0.2); }
		#objectCanvas { border: 3px solid transparent; border-radius: 8px; }
		.canvas-container { text-align: center; margin: 20px 0; position: relative; }
		#status { margin: 15px 0; padding: 12px; border-radius: 5px; text-align: center; font-weight: bold; }
		.success { background: #d4edda; color: #155724; }
		.error { background: #f8d7da; color: #721c24; }
		.info { background: #d1ecf1; color: #0c5460; }
		.action-buttons { display: flex; justify-content: center; gap: 15px; margin-top: 20px; }
		input[type="file"] { display: none; }
		.file-label { padding: 10px 20px; background: #2196F3; color: white; border-radius: 5px; cursor: pointer; display: inline-block; }
		.file-label:hover { background: #0b7dda; }
	</style>
</head>
<body>
	<div class="container">
		<h1>🎨 お絵かきモード</h1>
		<div class="toolbar">
			<div class="tool-group">
				<label>ツール:</label>
				<select id="tool">
					<option value="pen">ペン</option>
					<option value="eraser">消しゴム</option>
					<option value="fill">塗りつぶし</option>
					<option value="text">テキスト</option>
					<option value="select">選択</option>
				</select>
			</div>
			<div class="tool-group">
				<label>色:</label>
				<input type="color" id="color" value="#000000">
			</div>
			<div class="tool-group">
				<label>サイズ:</label>
				<input type="range" id="size" min="1" max="50" value="3">
				<input type="number" id="sizeValue" min="1" max="50" value="3" readonly>
			</div>
			<div class="tool-group">
				<label class="file-label" for="imageInput">📷 画像追加</label>
				<input type="file" id="imageInput" accept="image/*">
			</div>
			<button class="delete-btn" onclick="deleteSelected()" id="deleteBtn" style="display:none">🗑️ 選択削除</button>
			<button class="clear-btn" onclick="clearCanvas()">🆕 全クリア</button>
		</div>
		<div class="canvas-container">
			<canvas id="drawCanvas" width="800" height="600"></canvas>
			<canvas id="objectCanvas" width="800" height="600" style="position:absolute;top:50%;left:50%;transform:translate(-50%,-50%);pointer-events:none;"></canvas>
		</div>
		<div style="margin: 20px 0;">
			<label for="drawQuestionInput" style="display: block; margin-bottom: 10px; font-weight: bold;">💬 質問（オプション）:</label>
			<textarea id="drawQuestionInput" placeholder="画像について質問がある場合は入力してください（例: この絵は何を表していますか？）" style="width: 100%; height: 80px; padding: 10px; border: 1px solid #ccc; border-radius: 4px; font-size: 14px; box-sizing: border-box;"></textarea>
		</div>
		<div class="action-buttons">
			<button onclick="sendToAI()">🚀 AIに送信</button>
			<button onclick="downloadImage()">💾 画像保存</button>
		</div>
		<div id="status"></div>
	</div>
	<script>
		const drawCanvas = document.getElementById('drawCanvas');
		const objCanvas = document.getElementById('objectCanvas');
		const dCtx = drawCanvas.getContext('2d');
		const oCtx = objCanvas.getContext('2d');
		const colorPicker = document.getElementById('color');
		const sizePicker = document.getElementById('size');
		const sizeValue = document.getElementById('sizeValue');
		const toolSelect = document.getElementById('tool');
		const imageInput = document.getElementById('imageInput');
		const status = document.getElementById('status');
		const deleteBtn = document.getElementById('deleteBtn');
		let drawing = false;
		let currentTool = 'pen';
		let currentColor = '#000000';
		let currentSize = 3;
		let objects = [];
		let selectedObjs = [];
		let dragStart = null;
		function resizeCanvas() {
			const container = drawCanvas.parentElement;
			const maxWidth = Math.min(container.clientWidth - 40, 800);
			drawCanvas.style.width = maxWidth + 'px';
			drawCanvas.style.height = (maxWidth * 0.75) + 'px';
			objCanvas.style.width = maxWidth + 'px';
			objCanvas.style.height = (maxWidth * 0.75) + 'px';
		}
		window.addEventListener('resize', resizeCanvas);
		resizeCanvas();
		dCtx.fillStyle = 'white';
		dCtx.fillRect(0, 0, drawCanvas.width, drawCanvas.height);
		toolSelect.addEventListener('change', (e) => {
			currentTool = e.target.value;
			selectedObjs = [];
			deleteBtn.style.display = 'none';
			redrawObjects();
			updateCursor();
		});
		function updateCursor() {
			if (currentTool === 'text') drawCanvas.style.cursor = 'text';
			else if (currentTool === 'eraser') drawCanvas.style.cursor = 'cell';
			else if (currentTool === 'fill') drawCanvas.style.cursor = 'crosshair';
			else if (currentTool === 'select') drawCanvas.style.cursor = 'default';
			else drawCanvas.style.cursor = 'crosshair';
		}
		colorPicker.addEventListener('input', (e) => {
			currentColor = e.target.value;
			for (const obj of selectedObjs) {
				if (obj.type === 'text') obj.color = currentColor;
			}
			if (selectedObjs.length > 0) redrawAll();
		});
		sizePicker.addEventListener('input', (e) => {
			currentSize = parseInt(e.target.value);
			sizeValue.value = currentSize;
		});
		function getPos(e) {
			const rect = drawCanvas.getBoundingClientRect();
			const scaleX = drawCanvas.width / rect.width;
			const scaleY = drawCanvas.height / rect.height;
			if (e.touches && e.touches.length > 0) {
				return { x: (e.touches[0].clientX - rect.left) * scaleX, y: (e.touches[0].clientY - rect.top) * scaleY };
			}
			return { x: (e.clientX - rect.left) * scaleX, y: (e.clientY - rect.top) * scaleY };
		}
		function floodFill(x, y, fillColor) {
			const imageData = dCtx.getImageData(0, 0, drawCanvas.width, drawCanvas.height);
			const data = imageData.data;
			const startPos = (Math.floor(y) * drawCanvas.width + Math.floor(x)) * 4;
			const startR = data[startPos];
			const startG = data[startPos + 1];
			const startB = data[startPos + 2];
			const fillR = parseInt(fillColor.slice(1, 3), 16);
			const fillG = parseInt(fillColor.slice(3, 5), 16);
			const fillB = parseInt(fillColor.slice(5, 7), 16);
			if (startR === fillR && startG === fillG && startB === fillB) return;
			const stack = [[Math.floor(x), Math.floor(y)]];
			while (stack.length) {
				const [cx, cy] = stack.pop();
				if (cx < 0 || cx >= drawCanvas.width || cy < 0 || cy >= drawCanvas.height) continue;
				const pos = (cy * drawCanvas.width + cx) * 4;
				if (data[pos] === startR && data[pos + 1] === startG && data[pos + 2] === startB) {
					data[pos] = fillR; data[pos + 1] = fillG; data[pos + 2] = fillB;
					stack.push([cx + 1, cy], [cx - 1, cy], [cx, cy + 1], [cx, cy - 1]);
				}
			}
			dCtx.putImageData(imageData, 0, 0);
		}
		function startDrawing(e) {
			e.preventDefault();
			const pos = getPos(e);
			if (currentTool === 'select') {
				checkObjectClick(pos, e.ctrlKey || e.metaKey);
				return;
			}
			if (currentTool === 'fill') {
				floodFill(pos.x, pos.y, currentColor);
				return;
			}
			if (currentTool === 'text') {
				const text = prompt('テキストを入力:');
				if (text) {
					objects.push({ type: 'text', x: pos.x, y: pos.y, text, size: currentSize * 8, color: currentColor });
					redrawAll();
				}
				return;
			}
			drawing = true;
			dCtx.beginPath();
			dCtx.moveTo(pos.x, pos.y);
		}
		function draw(e) {
			if (!drawing) return;
			e.preventDefault();
			const pos = getPos(e);
			dCtx.lineWidth = currentSize;
			dCtx.lineCap = 'round';
			dCtx.lineJoin = 'round';
			dCtx.strokeStyle = currentTool === 'eraser' ? 'white' : currentColor;
			dCtx.lineTo(pos.x, pos.y);
			dCtx.stroke();
		}
		function stopDrawing(e) {
			if (!drawing) return;
			drawing = false;
			dCtx.beginPath();
		}
		function checkObjectClick(pos, ctrlKey) {
			let clickedObj = null;
			for (let i = objects.length - 1; i >= 0; i--) {
				const obj = objects[i];
				if (obj.type === 'image' && pos.x >= obj.x && pos.x <= obj.x + obj.w && pos.y >= obj.y && pos.y <= obj.y + obj.h) {
					clickedObj = obj; break;
				} else if (obj.type === 'text') {
					dCtx.font = obj.size + 'px sans-serif';
					const metrics = dCtx.measureText(obj.text);
					if (pos.x >= obj.x && pos.x <= obj.x + metrics.width && pos.y >= obj.y - obj.size && pos.y <= obj.y) {
						clickedObj = obj; break;
					}
				}
			}
			if (clickedObj) {
				if (ctrlKey) {
					const idx = selectedObjs.indexOf(clickedObj);
					if (idx >= 0) selectedObjs.splice(idx, 1); else selectedObjs.push(clickedObj);
				} else {
					if (selectedObjs.indexOf(clickedObj) < 0) selectedObjs = [clickedObj];
				}
				dragStart = pos;
			} else if (!ctrlKey) {
				selectedObjs = [];
			}
			deleteBtn.style.display = selectedObjs.length > 0 ? 'inline-block' : 'none';
			redrawObjects();
		}
		function dragObject(e) {
			if (selectedObjs.length === 0 || !dragStart) return;
			e.preventDefault();
			const pos = getPos(e);
			const dx = pos.x - dragStart.x;
			const dy = pos.y - dragStart.y;
			for (const obj of selectedObjs) {
				obj.x += dx; obj.y += dy;
			}
			dragStart = pos;
			redrawAll();
		}
		function stopDrag() {
			dragStart = null;
		}
		drawCanvas.addEventListener('mousedown', startDrawing);
		drawCanvas.addEventListener('mousemove', (e) => {
			if (currentTool === 'select') dragObject(e); else draw(e);
		});
		drawCanvas.addEventListener('mouseup', (e) => { stopDrawing(e); stopDrag(); });
		drawCanvas.addEventListener('mouseout', (e) => { stopDrawing(e); stopDrag(); });
		drawCanvas.addEventListener('touchstart', startDrawing);
		drawCanvas.addEventListener('touchmove', (e) => {
			if (currentTool === 'select') dragObject(e); else draw(e);
		});
		drawCanvas.addEventListener('touchend', (e) => { stopDrawing(e); stopDrag(); });
		drawCanvas.addEventListener('dblclick', (e) => {
			if (selectedObjs.length === 1 && selectedObjs[0].type === 'text') {
				const newText = prompt('テキスト編集:', selectedObjs[0].text);
				if (newText !== null) {
					selectedObjs[0].text = newText; redrawAll();
				}
			}
		});
		imageInput.addEventListener('change', (e) => {
			const file = e.target.files[0];
			if (file) {
				// ファイルサイズチェック（10MB以上は警告）
				if (file.size > 10 * 1024 * 1024) {
					showStatus('⚠️ 大きな画像です。処理に時間がかかります...', 'info');
				}
				
				const reader = new FileReader();
				reader.onload = (event) => {
					const img = new Image();
					img.onload = () => {
						// 画像が大きすぎる場合は自動的にリサイズ
						const maxW = drawCanvas.width * 0.5; 
						const maxH = drawCanvas.height * 0.5;
						let w = img.width, h = img.height;
						
						// メモリ効率のため、非常に大きな画像はより積極的にリサイズ
						const MAX_PIXELS = 800 * 600; // 最大ピクセル数
						if (w * h > MAX_PIXELS) {
							const scale = Math.sqrt(MAX_PIXELS / (w * h));
							w *= scale;
							h *= scale;
						}
						
						if (w > maxW || h > maxH) {
							const ratio = Math.min(maxW / w, maxH / h);
							w *= ratio; h *= ratio;
						}
						
						objects.push({ type: 'image', x: (drawCanvas.width - w) / 2, y: (drawCanvas.height - h) / 2, w, h, img, rotation: 0 });
						redrawAll(); 
						showStatus('✅ 画像を追加しました', 'success');
					};
					img.onerror = () => {
						showStatus('❌ 画像の読み込みに失敗しました', 'error');
					};
					img.src = event.target.result;
				};
				reader.onerror = () => {
					showStatus('❌ ファイルの読み込みに失敗しました', 'error');
				};
				reader.readAsDataURL(file);
			}
		});
		function redrawAll() {
			oCtx.clearRect(0, 0, objCanvas.width, objCanvas.height);
			for (const obj of objects) {
				if (obj.type === 'image') {
					oCtx.save();
					oCtx.translate(obj.x + obj.w / 2, obj.y + obj.h / 2);
					oCtx.rotate(obj.rotation || 0);
					oCtx.drawImage(obj.img, -obj.w / 2, -obj.h / 2, obj.w, obj.h);
					oCtx.restore();
				} else if (obj.type === 'text') {
					oCtx.font = obj.size + 'px sans-serif';
					oCtx.fillStyle = obj.color;
					oCtx.fillText(obj.text, obj.x, obj.y);
				}
			}
			redrawObjects();
		}
		function redrawObjects() {
			oCtx.clearRect(0, 0, objCanvas.width, objCanvas.height);
			for (const obj of objects) {
				if (obj.type === 'image') {
					oCtx.save();
					oCtx.translate(obj.x + obj.w / 2, obj.y + obj.h / 2);
					oCtx.rotate(obj.rotation || 0);
					oCtx.drawImage(obj.img, -obj.w / 2, -obj.h / 2, obj.w, obj.h);
					oCtx.restore();
				} else if (obj.type === 'text') {
					oCtx.font = obj.size + 'px sans-serif';
					oCtx.fillStyle = obj.color;
					oCtx.fillText(obj.text, obj.x, obj.y);
				}
			}
			for (const selectedObj of selectedObjs) {
				oCtx.strokeStyle = '#0088ff'; oCtx.lineWidth = 3; oCtx.setLineDash([8, 4]);
				if (selectedObj.type === 'image') {
					oCtx.strokeRect(selectedObj.x, selectedObj.y, selectedObj.w, selectedObj.h);
					oCtx.fillStyle = '#0088ff';
					oCtx.fillRect(selectedObj.x - 5, selectedObj.y - 5, 10, 10);
					oCtx.fillRect(selectedObj.x + selectedObj.w - 5, selectedObj.y - 5, 10, 10);
					oCtx.fillRect(selectedObj.x - 5, selectedObj.y + selectedObj.h - 5, 10, 10);
					oCtx.fillRect(selectedObj.x + selectedObj.w - 5, selectedObj.y + selectedObj.h - 5, 10, 10);
				} else if (selectedObj.type === 'text') {
					dCtx.font = selectedObj.size + 'px sans-serif';
					const metrics = dCtx.measureText(selectedObj.text);
					oCtx.strokeRect(selectedObj.x, selectedObj.y - selectedObj.size, metrics.width, selectedObj.size);
				}
				oCtx.setLineDash([]);
			}
		}
		function deleteSelected() {
			if (selectedObjs.length > 0) {
				objects = objects.filter(o => !selectedObjs.includes(o));
				selectedObjs = [];
				deleteBtn.style.display = 'none';
				redrawAll();
			}
		}
		function clearCanvas() {
			if (confirm('キャンバスをクリアしますか？')) {
				dCtx.fillStyle = 'white';
				dCtx.fillRect(0, 0, drawCanvas.width, drawCanvas.height);
				objects = []; selectedObjs = [];
				deleteBtn.style.display = 'none';
				redrawAll();
				showStatus('キャンバスをクリアしました', 'info');
			}
		}
		function sendToAI() {
			showStatus('AIに送信中...', 'info');
			const tmpCanvas = document.createElement('canvas');
			tmpCanvas.width = drawCanvas.width;
			tmpCanvas.height = drawCanvas.height;
			const tmpCtx = tmpCanvas.getContext('2d');
			tmpCtx.fillStyle = 'white';
			tmpCtx.fillRect(0, 0, tmpCanvas.width, tmpCanvas.height);
			tmpCtx.drawImage(drawCanvas, 0, 0);
			for (const obj of objects) {
				if (obj.type === 'image') {
					tmpCtx.save();
					tmpCtx.translate(obj.x + obj.w / 2, obj.y + obj.h / 2);
					tmpCtx.rotate(obj.rotation || 0);
					tmpCtx.drawImage(obj.img, -obj.w / 2, -obj.h / 2, obj.w, obj.h);
					tmpCtx.restore();
				} else if (obj.type === 'text') {
					tmpCtx.font = obj.size + 'px sans-serif';
					tmpCtx.fillStyle = obj.color;
					tmpCtx.fillText(obj.text, obj.x, obj.y);
				}
			}
			tmpCanvas.toBlob((blob) => {
				// サイズチェック（1MB以上の場合は品質を下げて再圧縮）
				if (blob.size > 1 * 1024 * 1024) {
					showStatus('⚠️ 画像が大きいため圧縮しています...', 'info');
					// 品質を下げて再圧縮（JPEG形式で）
					tmpCanvas.toBlob((compressedBlob) => {
						if (compressedBlob.size > 1.5 * 1024 * 1024) {
							showStatus('❌ 画像が大きすぎます。シンプルな絵にしてください', 'error');
							return;
						}
						sendCompressedImage(compressedBlob);
					}, 'image/jpeg', 0.7);
					return;
				}
				
				const formData = new FormData();
				formData.append('image', blob, 'drawing.png');
				
				// 質問文があれば追加
				const question = document.getElementById('drawQuestionInput').value.trim();
				if (question) {
					formData.append('question', question);
				}
				
				fetch('/image_upload', { method: 'POST', body: formData })
					.then(response => response.text())
					.then(data => showStatus('✅ 送信完了！スタックチャンが説明します', 'success'))
					.catch(error => showStatus('❌ 送信失敗: ' + error, 'error'));
			}, 'image/png', 0.85);
		}
		
		function sendCompressedImage(blob) {
			const formData = new FormData();
			formData.append('image', blob, 'drawing.jpg');
			
			// 質問文があれば追加
			const question = document.getElementById('drawQuestionInput')?.value.trim();
			if (question) {
				formData.append('question', question);
			}
			
			fetch('/image_upload', { method: 'POST', body: formData })
				.then(response => response.text())
				.then(data => showStatus('✅ 送信完了！スタックチャンが説明します', 'success'))
				.catch(error => showStatus('❌ 送信失敗: ' + error, 'error'));
		}
		
		function downloadImage() {
			const tmpCanvas = document.createElement('canvas');
			tmpCanvas.width = drawCanvas.width;
			tmpCanvas.height = drawCanvas.height;
			const tmpCtx = tmpCanvas.getContext('2d');
			tmpCtx.fillStyle = 'white';
			tmpCtx.fillRect(0, 0, tmpCanvas.width, tmpCanvas.height);
			tmpCtx.drawImage(drawCanvas, 0, 0);
			for (const obj of objects) {
				if (obj.type === 'image') {
					tmpCtx.save();
					tmpCtx.translate(obj.x + obj.w / 2, obj.y + obj.h / 2);
					tmpCtx.rotate(obj.rotation || 0);
					tmpCtx.drawImage(obj.img, -obj.w / 2, -obj.h / 2, obj.w, obj.h);
					tmpCtx.restore();
				} else if (obj.type === 'text') {
					tmpCtx.font = obj.size + 'px sans-serif';
					tmpCtx.fillStyle = obj.color;
					tmpCtx.fillText(obj.text, obj.x, obj.y);
				}
			}
			const link = document.createElement('a');
			link.download = 'drawing_' + Date.now() + '.png';
			link.href = tmpCanvas.toDataURL('image/png');
			link.click();
			showStatus('画像を保存しました', 'success');
		}
		function showStatus(message, type) {
			status.innerHTML = message;
			status.className = type;
			setTimeout(() => { status.innerHTML = ''; status.className = ''; }, 3000);
		}
	</script>
</body>
</html>)KEWL";

static const char IMAGE_UPLOAD_HTML[] PROGMEM = R"KEWL(
<!DOCTYPE html>
<html>
<head>
	<title>画像アップロード</title>
	<meta charset="UTF-8">
	<meta name="viewport" content="width=device-width, initial-scale=1.0">
	<style>
		body {
			font-family: Arial, sans-serif;
			max-width: 600px;
			margin: 50px auto;
			padding: 20px;
		}
		.upload-area {
			border: 2px dashed #ccc;
			border-radius: 10px;
			padding: 40px;
			text-align: center;
			margin: 20px 0;
		}
		#preview {
			max-width: 100%;
			max-height: 400px;
			margin: 20px 0;
			display: none;
		}
		button {
			background-color: #4CAF50;
			color: white;
			padding: 15px 32px;
			font-size: 16px;
			border: none;
			border-radius: 4px;
			cursor: pointer;
			margin: 10px;
		}
		button:hover {
			background-color: #45a049;
		}
		button:disabled {
			background-color: #cccccc;
			cursor: not-allowed;
		}
		#status {
			margin: 20px 0;
			padding: 10px;
			border-radius: 4px;
		}
		.success {
			background-color: #d4edda;
			color: #155724;
		}
		.error {
			background-color: #f8d7da;
			color: #721c24;
		}
		.info {
			background-color: #d1ecf1;
			color: #0c5460;
		}
	</style>
</head>
<body>
	<h1>🤖 スタックチャン画像説明</h1>
	<p>画像を選択してアップロードすると、スタックチャンが説明してくれます</p>
	
	<div class="upload-area">
		<input type="file" id="imageInput" accept="image/*" style="display: none;">
		<button onclick="document.getElementById('imageInput').click()">📷 画像を選択</button>
		<p>または、ここに画像をドロップ</p>
	</div>
	
	<img id="preview" alt="プレビュー">
	
	<div style="margin: 20px 0;">
		<label for="questionInput" style="display: block; margin-bottom: 10px; font-weight: bold;">💬 質問（オプション）:</label>
		<textarea id="questionInput" placeholder="画像について質問がある場合は入力してください（例: この写真に写っている物は何ですか？）" style="width: 100%; height: 80px; padding: 10px; border: 1px solid #ccc; border-radius: 4px; font-size: 14px; box-sizing: border-box;"></textarea>
	</div>
	
	<div style="text-align: center;">
		<button id="uploadBtn" onclick="uploadImage()" disabled>🚀 アップロード</button>
		<button onclick="clearImage()">🗑️ クリア</button>
	</div>
	
	<div id="status"></div>
	
	<script>
		let selectedFile = null;
		
		const imageInput = document.getElementById('imageInput');
		const preview = document.getElementById('preview');
		const uploadBtn = document.getElementById('uploadBtn');
		const status = document.getElementById('status');
		const uploadArea = document.querySelector('.upload-area');
		
		// ファイル選択時
		imageInput.addEventListener('change', function(e) {
			const file = e.target.files[0];
			if (file) {
				handleFile(file);
			}
		});
		
		// ドラッグ&ドロップ
		uploadArea.addEventListener('dragover', function(e) {
			e.preventDefault();
			uploadArea.style.borderColor = '#4CAF50';
		});
		
		uploadArea.addEventListener('dragleave', function(e) {
			uploadArea.style.borderColor = '#ccc';
		});
		
		uploadArea.addEventListener('drop', function(e) {
			e.preventDefault();
			uploadArea.style.borderColor = '#ccc';
			const file = e.dataTransfer.files[0];
			if (file && file.type.startsWith('image/')) {
				handleFile(file);
			}
		});
		
		function handleFile(file) {
			// ファイルサイズチェック（2MB以下）
			if (file.size > 2 * 1024 * 1024) {
				showStatus('画像サイズは2MB以下にしてください', 'error');
				return;
			}
			
			selectedFile = file;
			
			// プレビュー表示
			const reader = new FileReader();
			reader.onload = function(e) {
				preview.src = e.target.result;
				preview.style.display = 'block';
				uploadBtn.disabled = false;
				showStatus('画像を選択しました。アップロードボタンを押してください。', 'info');
			};
			reader.readAsDataURL(file);
		}
		
		function uploadImage() {
			if (!selectedFile) {
				showStatus('画像を選択してください', 'error');
				return;
			}
			
			uploadBtn.disabled = true;
			showStatus('アップロード中...', 'info');
			
			const formData = new FormData();
			formData.append('image', selectedFile);
			
			// 質問文があれば追加
			const question = document.getElementById('questionInput').value.trim();
			if (question) {
				formData.append('question', question);
			}
			
			fetch('/image_upload', {
				method: 'POST',
				body: formData
			})
			.then(response => response.text())
			.then(data => {
				showStatus('アップロード成功！スタックチャンが画像を説明します。', 'success');
				uploadBtn.disabled = false;
			})
			.catch(error => {
				showStatus('アップロード失敗: ' + error, 'error');
				uploadBtn.disabled = false;
			});
		}
		
		function clearImage() {
			selectedFile = null;
			document.getElementById('questionInput').value = '';
			preview.style.display = 'none';
			preview.src = '';
			imageInput.value = '';
			uploadBtn.disabled = true;
			status.innerHTML = '';
		}
		
		function showStatus(message, type) {
			status.innerHTML = message;
			status.className = type;
		}
	</script>
</body>
</html>)KEWL";


void handleRoot() {
  server.send(200, "text/plain", "hello from m5stack!");
}


void handleNotFound(){
  String message = "File Not Found\n\n";
  message += "URI: ";
  message += server.uri();
  message += "\nMethod: ";
  message += (server.method() == HTTP_GET)?"GET":"POST";
  message += "\nArguments: ";
  message += server.args();
  message += "\n";
  for (uint8_t i=0; i<server.args(); i++){
    message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
  }
//  server.send(404, "text/plain", message);
  server.send(404, "text/html", String(HEAD) + String("<body>") + message + String("</body>"));
}

void handle_speech() {
  String message = server.arg("say");
  String speaker = server.arg("voice");
  //if(speaker != "") {
  //  TTS_PARMS = TTS_SPEAKER + speaker;
  //}
  Serial.println(message);
  ////////////////////////////////////////
  // 音声の発声
  ////////////////////////////////////////
  //avatar.setExpression(Expression::Happy);
  robot->speech(message);
  server.send(200, "text/plain", String("OK"));
}

void handle_chat() {
  static String response = "";
  // tts_parms_no = 1;
  String text = server.arg("text");
  String speaker = server.arg("voice");
  //if(speaker != "") {
  //  TTS_PARMS = TTS_SPEAKER + speaker;
  //}

  robot->chat(text);

  server.send(200, "text/html", String(HEAD)+String("<body>")+response+String("</body>"));
}

void handle_apikey() {
  // ファイルを読み込み、クライアントに送信する
  server.send(200, "text/html", APIKEY_HTML);
}

#if 0
void handle_apikey_set() {
  // POST以外は拒否
  if (server.method() != HTTP_POST) {
    return;
  }
  // openai
  String openai = server.arg("openai");
  // voicetxt
  String voicevox = server.arg("voicevox");
  // voicetxt
  String sttapikey = server.arg("sttapikey");
 
  OPENAI_API_KEY = openai;
  VOICEVOX_API_KEY = voicevox;
  STT_API_KEY = sttapikey;
  Serial.println(openai);
  Serial.println(voicevox);
  Serial.println(sttapikey);

  uint32_t nvs_handle;
  if (ESP_OK == nvs_open("apikey", NVS_READWRITE, &nvs_handle)) {
    nvs_set_str(nvs_handle, "openai", openai.c_str());
    nvs_set_str(nvs_handle, "voicevox", voicevox.c_str());
    nvs_set_str(nvs_handle, "sttapikey", sttapikey.c_str());
    nvs_close(nvs_handle);
  }
  server.send(200, "text/plain", String("OK"));
}
#endif

void handle_role() {
  // ファイルを読み込み、クライアントに送信する
  server.send(200, "text/html", ROLE_HTML);
}


/**
 * アプリからテキスト(文字列)と共にRoll情報が配列でPOSTされてくることを想定してJSONを扱いやすい形に変更
 * 出力形式をJSONに変更
*/
void handle_role_set() {

  // ModuleLLMのLLMを使用している場合はロール設定は不可
  if(robot->m_config.getExConfig().llm.type == LLM_TYPE_MODULE_LLM){
    return;
  }

  // POST以外は拒否
  if (server.method() != HTTP_POST) {
    return;
  }
  String role = server.arg("plain");
  if (role != "") {
//    init_chat_doc(InitBuffer.c_str());
    robot->llm->init_chat_doc(json_ChatString.c_str());
    JsonArray messages = chat_doc["messages"];
    JsonObject systemMessage1 = messages.createNestedObject();
    systemMessage1["role"] = "system";
    systemMessage1["content"] = role;
//    serializeJson(chat_doc, InitBuffer);
  } else {
    robot->llm->init_chat_doc(json_ChatString.c_str());
  }
  //会話履歴をクリア
  chatHistory.clear();

#if 0  //save_role()に移動
  InitBuffer="";
  serializeJson(chat_doc, InitBuffer);
  Serial.println("InitBuffer = " + InitBuffer);
  //Role_JSON = InitBuffer;
#endif

  // JSONデータをspiffsへ出力する
  robot->llm->save_role();

  // 整形したJSONデータを出力するHTMLデータを作成する
  String html = "<html><body><pre>";
  serializeJsonPretty(chat_doc, html);
  html += "</pre></body></html>";

  // HTMLデータをシリアルに出力する
  Serial.println(html);
  server.send(200, "text/html", html);
//  server.send(200, "text/plain", String("OK"));
};

// 整形したJSONデータを出力するHTMLデータを作成する
void handle_role_get() {

  String html = "<html><body><pre>";
  serializeJsonPretty(chat_doc, html);
  html += "</pre></body></html>";

  // HTMLデータをシリアルに出力する
  Serial.println(html);
  server.send(200, "text/html", String(HEAD) + html);
};

void handle_image_upload_page() {
  server.send(200, "text/html", IMAGE_UPLOAD_HTML);
}

void handle_draw_page() {
  server.send(200, "text/html", DRAW_HTML);
}

void handle_image_upload() {
  // POST以外は拒否
  if (server.method() != HTTP_POST) {
    server.send(405, "text/plain", "Method Not Allowed");
    return;
  }

  HTTPUpload& upload = server.upload();
  static File uploadFile;
  static String uploadPath = "/app/AiStackChanEx/uploaded_image.jpg";
  static String tempQuestion = "";  // 一時的に質問を保存

  if (upload.status == UPLOAD_FILE_START) {
    Serial.printf("Upload Start: %s\n", upload.filename.c_str());
    
    // 待機音声タスクを起動（アップロード中のフィードバック）
    avatar.setSpeechText("画像を受信中...");
    robot->startIdlePhraseTask();
    
    // アップロード開始時にquestionフィールドを取得（まだ取得できる段階）
    tempQuestion = "";
    
    // SDカードの初期化確認
    if(!SD.begin(GPIO_NUM_4, SPI, 25000000)) {
      Serial.println("SD Card Mount Failed");
      server.send(500, "text/plain", "SD Card Error");
      return;
    }

    // アップロードディレクトリの作成
    if(!SD.exists("/app/AiStackChanEx")) {
      SD.mkdir("/app");
      SD.mkdir("/app/AiStackChanEx");
    }

    // ファイルを開く（上書き）
    uploadFile = SD.open(uploadPath.c_str(), FILE_WRITE);
    if (!uploadFile) {
      Serial.println("Failed to open file for writing");
      server.send(500, "text/plain", "File Open Error");
      return;
    }
  } 
  else if (upload.status == UPLOAD_FILE_WRITE) {
    // データを書き込む
    if (uploadFile) {
      uploadFile.write(upload.buf, upload.currentSize);
      Serial.printf("Writing: %d bytes\n", upload.currentSize);
    }
  } 
  else if (upload.status == UPLOAD_FILE_END) {
    if (uploadFile) {
      uploadFile.close();
      Serial.printf("Upload Complete: %d bytes\n", upload.totalSize);
      
      // 待機音声タスクを停止
      robot->stopIdlePhraseTask();
      avatar.setSpeechText("");
      
      // 質問文を取得（POSTパラメータから）
      // multipart/form-dataの場合、この時点でargが利用可能
      if (server.hasArg("question")) {
        g_imageQuestion = server.arg("question");
        Serial.println("Question from arg: " + g_imageQuestion);
      } else if (tempQuestion != "") {
        g_imageQuestion = tempQuestion;
        Serial.println("Question from temp: " + g_imageQuestion);
      } else {
        g_imageQuestion = "";
        Serial.println("No question provided");
      }
      
      // グローバル変数に画像パスを保存
      g_uploadedImagePath = uploadPath;
      g_imageUploaded = true;
      
      server.send(200, "text/plain", "OK - Image uploaded successfully");
      Serial.println("Image uploaded: " + uploadPath);
    } else {
      server.send(500, "text/plain", "Upload Error");
    }
  }
  else if (upload.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) {
      uploadFile.close();
    }
    // 待機音声タスクを停止
    robot->stopIdlePhraseTask();
    avatar.setSpeechText("");
    
    Serial.println("Upload Aborted");
    server.send(500, "text/plain", "Upload Aborted");
  }
}

// アップロード完了後のハンドラ（ここでquestionを取得）
void handle_image_upload_complete() {
  if (server.hasArg("question")) {
    g_imageQuestion = server.arg("question");
    Serial.println("Question in complete handler: " + g_imageQuestion);
  }
}

void handle_face() {
  String expression = server.arg("expression");
  expression = expression + "\n";
  Serial.println(expression);
  switch (expression.toInt())
  {
    case 0: avatar.setExpression(Expression::Neutral); break;
    case 1: avatar.setExpression(Expression::Happy); break;
    case 2: avatar.setExpression(Expression::Sleepy); break;
    case 3: avatar.setExpression(Expression::Doubt); break;
    case 4: avatar.setExpression(Expression::Sad); break;
    case 5: avatar.setExpression(Expression::Angry); break;  
  } 
  server.send(200, "text/plain", String("OK"));
}

#if 0
void handle_setting() {
  String value = server.arg("volume");
  String led = server.arg("led");
  String speaker = server.arg("speaker");
//  volume = volume + "\n";
  Serial.println(speaker);
  Serial.println(value);
  size_t speaker_no;

  if(speaker != ""){
    speaker_no = speaker.toInt();
    if(speaker_no > 60) {
      speaker_no = 60;
    }
    TTS_SPEAKER_NO = String(speaker_no);
    TTS_PARMS = TTS_SPEAKER + TTS_SPEAKER_NO;
  }

  if(value == "") value = "180";
  size_t volume = value.toInt();
  uint8_t led_onoff = 0;
  uint32_t nvs_handle;
  if (ESP_OK == nvs_open("setting", NVS_READWRITE, &nvs_handle)) {
    if(volume > 255) volume = 255;
    nvs_set_u32(nvs_handle, "volume", volume);
    if(led != "") {
      if(led == "on") led_onoff = 1;
      else  led_onoff = 0;
      nvs_set_u8(nvs_handle, "led", led_onoff);
    }
    nvs_set_u8(nvs_handle, "speaker", speaker_no);

    nvs_close(nvs_handle);
  }
  M5.Speaker.setVolume(volume);
  M5.Speaker.setChannelVolume(m5spk_virtual_channel, volume);
  server.send(200, "text/plain", String("OK"));
}
#endif

void init_web_server(void)
{

  server.on("/", handleRoot);
  server.on("/inline", [](){
    server.send(200, "text/plain", "this works as well");
  });

  // And as regular external functions:
  server.on("/speech", handle_speech);
  server.on("/face", handle_face);
  server.on("/chat", handle_chat);
  server.on("/apikey", handle_apikey);
  //server.on("/setting", handle_setting);
  //server.on("/apikey_set", HTTP_POST, handle_apikey_set);
  server.on("/role", handle_role);
  server.on("/role_set", HTTP_POST, handle_role_set);
  server.on("/role_get", handle_role_get);
  server.on("/image_upload_page", handle_image_upload_page);
  server.on("/draw", handle_draw_page);  // お絵かきページ
  server.on("/image_upload", HTTP_POST, handle_image_upload_complete, handle_image_upload);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("HTTP server started");
  M5.Lcd.println("HTTP server started");  
}

void web_server_handle_client(void)
{
  server.handleClient();
}
