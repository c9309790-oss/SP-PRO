#!/usr/bin/env python3
"""
Opus 文件下载服务器

为 ESP32 设备提供 Opus 文件下载服务，支持：
- 文件列表查询
- 分块下载（适合内存受限设备）
- 文件完整性校验（MD5）

使用方法：
    python opus_server.py [--port 8080] [--dir ./opus_files]

API 接口：
    GET  /api/list           - 获取 Opus 文件列表
    GET  /api/info/<name>    - 获取指定文件信息
    GET  /api/download/<name> - 下载文件（支持 Range 请求）
"""

import os
import sys
import json
import hashlib
import argparse
import ssl
from http.server import HTTPServer, BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse, unquote
from datetime import datetime

# 默认配置
DEFAULT_PORT = 8080
# 获取脚本所在目录的绝对路径
SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEFAULT_OPUS_DIR = os.path.join(SCRIPT_DIR, "opus_files")
CHUNK_SIZE = 65536  # 分块大小


def get_file_md5(filepath):
    """计算文件 MD5"""
    md5 = hashlib.md5()
    with open(filepath, 'rb') as f:
        while chunk := f.read(8192):
            md5.update(chunk)
    return md5.hexdigest()


def log(level, message):
    """日志输出"""
    timestamp = datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    print(f"[{timestamp}] [{level}] {message}")


class OpusRequestHandler(BaseHTTPRequestHandler):
    """HTTP 请求处理器"""
    
    opus_dir = DEFAULT_OPUS_DIR
    
    def log_message(self, format, *args):
        """覆盖默认日志"""
        log("INFO", f"{self.address_string()} - {format % args}")
    
    def send_json_response(self, data, status=200):
        """发送 JSON 响应"""
        content = json.dumps(data, ensure_ascii=False).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json; charset=utf-8')
        self.send_header('Content-Length', len(content))
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        self.wfile.write(content)
    
    def send_error_response(self, message, status=400):
        """发送错误响应"""
        self.send_json_response({'error': message, 'status': status}, status)
    
    def do_GET(self):
        """处理 GET 请求"""
        parsed = urlparse(self.path)
        path = unquote(parsed.path)
        
        try:
            if path == '/api/list':
                self.handle_list()
            elif path.startswith('/api/info/'):
                filename = path[10:]  # 去掉 '/api/info/'
                self.handle_info(filename)
            elif path.startswith('/api/download/'):
                filename = path[14:]  # 去掉 '/api/download/'
                self.handle_download(filename)
            elif path == '/':
                self.handle_index()
            else:
                self.send_error_response('Not Found', 404)
        except Exception as e:
            log("ERROR", f"Request error: {e}")
            self.send_error_response(str(e), 500)
    
    def handle_index(self):
        """首页 - 显示简单说明"""
        html = """
<!DOCTYPE html>
<html>
<head>
    <title>Opus Download Server</title>
    <meta charset="utf-8">
    <style>
        body { font-family: Arial, sans-serif; margin: 40px; background: #f5f5f5; }
        .container { background: white; padding: 30px; border-radius: 8px; max-width: 800px; margin: 0 auto; }
        h1 { color: #333; }
        .api { background: #f0f0f0; padding: 10px; margin: 10px 0; border-radius: 4px; font-family: monospace; }
        .files { margin-top: 20px; }
        .file-item { padding: 8px; border-bottom: 1px solid #eee; }
        .file-size { color: #666; font-size: 0.9em; }
    </style>
</head>
<body>
    <div class="container">
        <h1>Opus Download Server</h1>
        <h2>API Endpoints:</h2>
        <div class="api">GET /api/list - 获取文件列表</div>
        <div class="api">GET /api/info/&lt;filename&gt; - 获取文件信息</div>
        <div class="api">GET /api/download/&lt;filename&gt; - 下载文件</div>
        
        <div class="files">
            <h2>Available Files:</h2>
            <div id="file-list">Loading...</div>
        </div>
    </div>
    <script>
        fetch('/api/list')
            .then(r => r.json())
            .then(data => {
                const list = document.getElementById('file-list');
                if (data.files && data.files.length > 0) {
                    list.innerHTML = data.files.map(f => 
                        `<div class="file-item">
                            <a href="/api/download/${f.name}">${f.name}</a>
                            <span class="file-size">(${(f.size/1024).toFixed(1)} KB)</span>
                        </div>`
                    ).join('');
                } else {
                    list.innerHTML = '<p>No Opus files found</p>';
                }
            })
            .catch(e => {
                document.getElementById('file-list').innerHTML = '<p>Error loading files</p>';
            });
    </script>
</body>
</html>
"""
        content = html.encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'text/html; charset=utf-8')
        self.send_header('Content-Length', len(content))
        self.end_headers()
        self.wfile.write(content)
    
    def handle_list(self):
        """获取 Opus 文件列表"""
        files = []
        
        if os.path.exists(self.opus_dir):
            for name in os.listdir(self.opus_dir):
                if name.lower().endswith('.opus'):
                    filepath = os.path.join(self.opus_dir, name)
                    if os.path.isfile(filepath):
                        stat = os.stat(filepath)
                        files.append({
                            'name': name,
                            'size': stat.st_size,
                            'modified': datetime.fromtimestamp(stat.st_mtime).isoformat()
                        })
        
        # 按名称排序
        files.sort(key=lambda x: x['name'])
        
        log("INFO", f"List request: {len(files)} files found")
        self.send_json_response({
            'files': files,
            'total': len(files),
            'server_time': datetime.now().isoformat()
        })
    
    def handle_info(self, filename):
        """获取文件详细信息"""
        filepath = os.path.join(self.opus_dir, filename)
        
        if not os.path.exists(filepath):
            self.send_error_response(f'File not found: {filename}', 404)
            return
        
        if not filename.lower().endswith('.opus'):
            self.send_error_response('Only Opus files are allowed', 400)
            return
        
        stat = os.stat(filepath)
        md5 = get_file_md5(filepath)
        
        log("INFO", f"Info request: {filename}, size={stat.st_size}, md5={md5}")
        
        self.send_json_response({
            'name': filename,
            'size': stat.st_size,
            'md5': md5,
            'modified': datetime.fromtimestamp(stat.st_mtime).isoformat()
        })
    
    def handle_download(self, filename):
        """下载文件（支持 Range 请求）"""
        filepath = os.path.join(self.opus_dir, filename)
        
        if not os.path.exists(filepath):
            self.send_error_response(f'File not found: {filename}', 404)
            return
        
        if not filename.lower().endswith('.opus'):
            self.send_error_response('Only Opus files are allowed', 400)
            return
        
        file_size = os.path.getsize(filepath)
        
        # 处理 Range 请求（分块下载）
        range_header = self.headers.get('Range')
        start = 0
        end = file_size - 1
        
        if range_header:
            # 解析 Range: bytes=start-end
            try:
                range_spec = range_header.replace('bytes=', '')
                if '-' in range_spec:
                    parts = range_spec.split('-')
                    if parts[0]:
                        start = int(parts[0])
                    if parts[1]:
                        end = int(parts[1])
                    end = min(end, file_size - 1)
            except ValueError:
                self.send_error_response('Invalid Range header', 416)
                return
            
            if start >= file_size or start > end:
                self.send_error_response('Range not satisfiable', 416)
                return
            
            # 发送部分内容
            self.send_response(206)
            self.send_header('Content-Range', f'bytes {start}-{end}/{file_size}')
        else:
            self.send_response(200)
        
        content_length = end - start + 1
        
        self.send_header('Content-Type', 'audio/ogg')
        self.send_header('Content-Length', content_length)
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Disposition', f'attachment; filename="{filename}"')
        self.send_header('Access-Control-Allow-Origin', '*')
        self.end_headers()
        
        # 发送文件内容
        with open(filepath, 'rb') as f:
            f.seek(start)
            remaining = content_length
            while remaining > 0:
                chunk = f.read(min(CHUNK_SIZE, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)
        
        log("INFO", f"Download: {filename}, bytes {start}-{end}/{file_size}")


def main():
    parser = argparse.ArgumentParser(description='Opus Download Server')
    parser.add_argument('--port', '-p', type=int, default=DEFAULT_PORT,
                        help=f'Server port (default: {DEFAULT_PORT})')
    parser.add_argument('--dir', '-d', type=str, default=DEFAULT_OPUS_DIR,
                        help=f'Opus files directory (default: {DEFAULT_OPUS_DIR})')
    args = parser.parse_args()
    
    # 设置 Opus 目录
    OpusRequestHandler.opus_dir = os.path.abspath(args.dir)
    
    # 创建目录（如果不存在）
    if not os.path.exists(args.dir):
        os.makedirs(args.dir)
        log("INFO", f"Created Opus directory: {args.dir}")
    
    # 统计现有文件
    opus_count = len([f for f in os.listdir(args.dir) if f.lower().endswith('.opus')])
    
    # 启动服务器
    server = HTTPServer(('0.0.0.0', args.port), OpusRequestHandler)
    
    protocol = "http"
    
    log("INFO", "=" * 50)
    log("INFO", "Opus Download Server Started")
    log("INFO", f"Port: {args.port}")
    log("INFO", f"Protocol: {protocol}")
    log("INFO", f"Opus Directory: {OpusRequestHandler.opus_dir}")
    log("INFO", f"Available Opus files: {opus_count}")
    log("INFO", "=" * 50)
    log("INFO", f"Access: {protocol}://localhost:{args.port}")
    log("INFO", "Press Ctrl+C to stop")
    
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("INFO", "Server stopped")
        server.shutdown()


if __name__ == '__main__':
    main()
