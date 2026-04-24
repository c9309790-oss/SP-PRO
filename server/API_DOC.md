# Opus 音频分发服务 API 文档

**版本**: 1.0.0  
**最后更新**: 2025-12-29  
**协议**: HTTP/1.1  

## 1. 概述

本服务旨在为嵌入式设备（如 ESP32）提供轻量级的 Opus 音频文件分发功能。它支持文件列表检索、元数据查询（包含 MD5 校验）以及支持断点续传的文件下载功能。

### 1.1 服务地址

*   **默认端口**: `8080`
*   **基础路径**: `/api`
*   **示例地址**: `http://localhost:8080/api`

### 1.2 通用规范

*   **字符编码**: 所有请求和响应均使用 `UTF-8` 编码。
*   **时间格式**: ISO 8601 (例如 `2025-12-29T10:30:00.123456`)。
*   **错误响应**: 所有非 2xx 响应均返回 JSON 格式的错误详情。

---

## 2. API 接口详情

### 2.1 获取文件列表

获取服务器上所有可用的 Opus 音频文件列表。

*   **Endpoint**: `/api/list`
*   **Method**: `GET`
*   **Description**: 返回服务器 `opus_files` 目录下所有 `.opus` 文件的摘要列表。

#### 请求参数
无

#### 响应 (200 OK)

| 字段 | 类型 | 说明 |
| :--- | :--- | :--- |
| `files` | Array | 文件对象列表（按文件名排序） |
| `total` | Integer | 文件总数 |
| `server_time` | String | 服务器当前时间 (ISO 8601) |

**File 对象结构**:

| 字段 | 类型 | 说明 |
| :--- | :--- | :--- |
| `name` | String | 文件名 |
| `size` | Integer | 文件大小 (字节) |
| `modified` | String | 最后修改时间 |

#### 示例
**Request**:
```http
GET /api/list HTTP/1.1
Host: localhost:8080
```

**Response**:
```json
{
  "files": [
    {
      "name": "bgm_01.opus",
      "size": 1048576,
      "modified": "2025-01-15T08:00:00.000000"
    },
    {
      "name": "welcome.opus",
      "size": 524288,
      "modified": "2025-01-16T09:30:00.000000"
    }
  ],
  "total": 2,
  "server_time": "2025-12-29T12:00:00.123456"
}
```

---

### 2.2 获取文件详细信息

获取指定文件的详细元数据，特别是用于完整性校验的 MD5 值。

*   **Endpoint**: `/api/info/<filename>`
*   **Method**: `GET`
*   **Description**: 计算并返回指定文件的 MD5 哈希值及其他元数据。

#### URL 参数

| 参数 | 类型 | 必填 | 说明 |
| :--- | :--- | :--- | :--- |
| `filename` | String | 是 | 目标文件名 (必须以 `.opus` 结尾) |

#### 响应 (200 OK)

| 字段 | 类型 | 说明 |
| :--- | :--- | :--- |
| `name` | String | 文件名 |
| `size` | Integer | 文件大小 (字节) |
| `md5` | String | 32位十六进制 MD5 哈希值 |
| `modified` | String | 最后修改时间 |

#### 错误响应

*   **400 Bad Request**: 请求的文件名不是 `.opus` 后缀。
*   **404 Not Found**: 请求的文件不存在。

#### 示例
**Request**:
```http
GET /api/info/welcome.opus HTTP/1.1
Host: localhost:8080
```

**Response**:
```json
{
  "name": "welcome.opus",
  "size": 524288,
  "md5": "e10adc3949ba59abbe56e057f20f883e",
  "modified": "2025-01-16T09:30:00.000000"
}
```

---

### 2.3 下载文件

下载指定的 Opus 音频文件。

*   **Endpoint**: `/api/download/<filename>`
*   **Method**: `GET`
*   **Description**: 流式传输文件内容。支持 HTTP Range 头，允许客户端进行分块下载或断点续传。

#### URL 参数

| 参数 | 类型 | 必填 | 说明 |
| :--- | :--- | :--- | :--- |
| `filename` | String | 是 | 目标文件名 |

#### 请求头 (Header)

| 字段 | 说明 | 示例 |
| :--- | :--- | :--- |
| `Range` | (可选) 指定请求的字节范围 | `bytes=0-1023` |

#### 响应头 (Header)

| 字段 | 说明 |
| :--- | :--- |
| `Content-Type` | `audio/ogg` |
| `Content-Length` | 响应体的字节长度 |
| `Content-Disposition` | `attachment; filename="<filename>"` |
| `Accept-Ranges` | `bytes` |
| `Content-Range` | (仅 206 响应) 当前片段的范围，格式 `bytes start-end/total` |

#### 响应状态码

*   **200 OK**: 返回完整文件。
*   **206 Partial Content**: 返回请求范围内的部分内容。
*   **416 Range Not Satisfiable**: 请求的范围无效（如超出文件大小）。
*   **404 Not Found**: 文件不存在。

#### 示例 (分块下载)

**Request**:
```http
GET /api/download/welcome.opus HTTP/1.1
Host: localhost:8080
Range: bytes=0-1023
```

**Response**:
```http
HTTP/1.1 206 Partial Content
Content-Type: audio/ogg
Content-Length: 1024
Content-Range: bytes 0-1023/524288
Accept-Ranges: bytes

[Binary Data...]
```

---

## 3. 错误处理

当请求发生错误时，服务器将返回 JSON 格式的错误响应。

**错误响应结构**:
```json
{
  "error": "错误描述信息",
  "status": HTTP状态码
}
```

**常见错误码**:
*   `400`: 请求参数错误（如非法文件后缀）。
*   `404`: 资源未找到。
*   `416`: Range 范围无效。
*   `500`: 服务器内部错误。
