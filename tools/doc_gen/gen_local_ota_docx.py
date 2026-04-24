from __future__ import annotations

import html
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED


ROOT = Path(r"E:\Project\SP-Pro-ESP32")
OUT = ROOT / "build" / "本地模拟OTA说明_20260410.docx"


def x(text: str) -> str:
    return html.escape(text, quote=False)


def p(text: str, bold: bool = False) -> str:
    if not text:
        return "<w:p><w:r><w:t xml:space=\"preserve\"> </w:t></w:r></w:p>"
    run_props = "<w:rPr><w:b/></w:rPr>" if bold else ""
    return (
        "<w:p><w:r>"
        f"{run_props}<w:t xml:space=\"preserve\">{x(text)}</w:t>"
        "</w:r></w:p>"
    )


def build_doc(lines: list[tuple[str, bool]]) -> str:
    body = "".join(p(text, bold) for text, bold in lines)
    sect = (
        "<w:sectPr>"
        "<w:pgSz w:w=\"11906\" w:h=\"16838\"/>"
        "<w:pgMar w:top=\"1440\" w:right=\"1440\" w:bottom=\"1440\" "
        "w:left=\"1440\" w:header=\"708\" w:footer=\"708\" w:gutter=\"0\"/>"
        "<w:cols w:space=\"708\"/>"
        "<w:docGrid w:linePitch=\"360\"/>"
        "</w:sectPr>"
    )
    return (
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<w:document xmlns:wpc=\"http://schemas.microsoft.com/office/word/2010/wordprocessingCanvas\" "
        "xmlns:mc=\"http://schemas.openxmlformats.org/markup-compatibility/2006\" "
        "xmlns:o=\"urn:schemas-microsoft-com:office:office\" "
        "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\" "
        "xmlns:m=\"http://schemas.openxmlformats.org/officeDocument/2006/math\" "
        "xmlns:v=\"urn:schemas-microsoft-com:vml\" "
        "xmlns:wp14=\"http://schemas.microsoft.com/office/word/2010/wordprocessingDrawing\" "
        "xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\" "
        "xmlns:w10=\"urn:schemas-microsoft-com:office:word\" "
        "xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\" "
        "xmlns:w14=\"http://schemas.microsoft.com/office/word/2010/wordml\" "
        "xmlns:wpg=\"http://schemas.microsoft.com/office/word/2010/wordprocessingGroup\" "
        "xmlns:wpi=\"http://schemas.microsoft.com/office/word/2010/wordprocessingInk\" "
        "xmlns:wne=\"http://schemas.microsoft.com/office/word/2006/wordml\" "
        "xmlns:wps=\"http://schemas.microsoft.com/office/word/2010/wordprocessingShape\" "
        "mc:Ignorable=\"w14 wp14\">"
        f"<w:body>{body}{sect}</w:body></w:document>"
    )


CONTENT_TYPES = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
  <Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
  <Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>
</Types>
"""

ROOT_RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/>
</Relationships>
"""

DOC_RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships"/>
"""

CORE_XML = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties"
 xmlns:dc="http://purl.org/dc/elements/1.1/"
 xmlns:dcterms="http://purl.org/dc/terms/"
 xmlns:dcmitype="http://purl.org/dc/dcmitype/"
 xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <dc:title>本地模拟OTA说明</dc:title>
  <dc:creator>Codex</dc:creator>
  <cp:lastModifiedBy>Codex</cp:lastModifiedBy>
  <dcterms:created xsi:type="dcterms:W3CDTF">2026-04-10T00:00:00Z</dcterms:created>
  <dcterms:modified xsi:type="dcterms:W3CDTF">2026-04-10T00:00:00Z</dcterms:modified>
</cp:coreProperties>
"""

APP_XML = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties"
 xmlns:vt="http://schemas.openxmlformats.org/officeDocument/2006/docPropsVTypes">
  <Application>Codex</Application>
</Properties>
"""


def main() -> None:
    lines = [
        ("本地模拟 OTA 说明", True),
        ("", False),
        ("一、用途", True),
        ("本文档用于本地联调 ESP32 侧 OTA，不依赖正式云端下发任务。目标是验证：HTTP 下载、包识别、HMI 界面确认、CTR/ESP32 执行链路。", False),
        ("", False),
        ("二、当前本地测试相关代码开关", True),
        ("1. 文件：E:\\Project\\SP-Pro-ESP32\\main\\ota_ctr_esp32\\ota_ctr.c", False),
        ("2. OTA_LOCAL_HTTP_TEST_ENABLE：控制是否启用本地 HTTP OTA 注入。", False),
        ("3. OTA_LOCAL_SIM_TEST：控制是否启用本地模拟 confirm / 发 C / 模拟 YMODEM 成功 / 模拟 apply 成功。", False),
        ("4. 做真实 CTR 联调时，应关闭 OTA_LOCAL_SIM_TEST。", False),
        ("5. 当前本地 HMI 手动确认模式下，autoUp=0，用户需要点击屏幕 WIFI icon 才会继续升级。", False),
        ("", False),
        ("三、本地 HTTP 服务怎么起", True),
        ("1. 打开 PowerShell。", False),
        ("2. 切到 OTA 包目录，例如：Set-Location 'E:\\Project\\SP-Pro-ESP32\\build\\ota_bundle'。", False),
        ("3. 启动 HTTP 服务：", False),
        ("   & 'd:\\Tools\\esp\\python_env\\idf5.2_py3.11_env\\Scripts\\python.exe' -m http.server 8000", False),
        ("4. 正常情况下会看到：Serving HTTP on ... port 8000", False),
        ("5. 这个窗口不要关闭。", False),
        ("", False),
        ("四、如何确认 URL 可访问", True),
        ("1. 先查电脑 IP：ipconfig", False),
        ("2. 假设电脑 IP 是 192.168.39.148，文件名是 ota_hmi_manual_local.bin。", False),
        ("3. 那么设备使用的 URL 是：http://192.168.39.148:8000/ota_hmi_manual_local.bin", False),
        ("4. 先在电脑浏览器访问：http://127.0.0.1:8000/ota_hmi_manual_local.bin", False),
        ("5. 如果浏览器能下载，说明本地 HTTP 服务是正常的。", False),
        ("6. ESP32 必须和电脑在同一个网段，例如都在 192.168.39.x。", False),
        ("", False),
        ("五、本地 OTA 触发时机", True),
        ("1. 当前代码已经改成：只在 WIFI ALL DONE 之后才启动本地 OTA probe。", False),
        ("2. 所以日志顺序应类似：", False),
        ("   WIFI: ALL DONE", False),
        ("   Start local HTTP OTA test: ...", False),
        ("   OTA start download ...", False),
        ("3. 如果在 WIFI 尚未 ready 时发 probe，容易出现 HTTP connect timeout。当前代码已经规避这一点。", False),
        ("", False),
        ("六、三种本地测试场景", True),
        ("1. ESP32 only：bundle 中只有 ESP32 payload，用于验证 HMI/ESP32 OTA 写 OTA 分区并重启。", False),
        ("2. CTR only：bundle 中只有 CTR payload，用于验证 CTR 包下载、CRC 校验、YMODEM、CTR 版本切换。", False),
        ("3. CTR + ESP32：先升 CTR，再升 ESP32，用于验证双 payload 串行路径。", False),
        ("", False),
        ("七、HMI 手动确认模式", True),
        ("1. autoUpdateFlag=0 时，不自动升级。", False),
        ("2. OTA 到 WAIT_CONFIRM 后，界面应进入 OTA 提醒态。", False),
        ("3. 用户单击 WIFI icon，才继续升级。", False),
        ("4. 当前这条本地链路已经验证过：界面交互符合预期。", False),
        ("", False),
        ("八、真实 CTR 联调前要做的事", True),
        ("1. 关闭 OTA_LOCAL_SIM_TEST。", False),
        ("2. 保留真实的 boot 命令：123@OTA@NULL#123。", False),
        ("3. 观察 CTR 是否真的进入 bootloader，并发出真实字符 C。", False),
        ("4. 如果 CTR 真实发出 C，ESP32 侧再继续走真实 YMODEM。", False),
        ("", False),
        ("九、本地调试时重点看哪些日志", True),
        ("1. HTTP_DOWNLOAD: OTA start download", False),
        ("2. Merged OTA bundle detected", False),
        ("3. Bundle CTR entry / Bundle ESP32 entry", False),
        ("4. CRC16 check passed", False),
        ("5. Controller bootloader handshake byte 'C'", False),
        ("6. CTR OTA SUCCESS!!!!!!", False),
        ("7. HMI OTA SUCCESS!!!!!!", False),
        ("", False),
        ("十、常见问题", True),
        ("1. HTTP connect timeout：通常是本地 HTTP 服务没起、IP 变了、设备和电脑不在同网段，或 OTA probe 早于 WIFI ready。", False),
        ("2. Bootloader 一直打印 state0:C：通常说明 CTR 端没进入应用，或 app/package 没烧进去，或 CRC 校验没通过。", False),
        ("3. 看到 0x08003800 后面很多 0：这是正常的 2KB OTA 头填充，不是异常。", False),
        ("4. Keil 点普通 Load 只会烧当前工程的 AXF，不会自动烧你外部现成的 package HEX。", False),
        ("", False),
        ("十一、当前你本地环境里常用的关键文件", True),
        ("1. HTTP OTA 打包工具：E:\\Project\\SP-Pro-ESP32\\tools\\launch_ota_bundle_gui.cmd", False),
        ("2. BIN 转 HEX 工具：E:\\Project\\SP-Pro-ESP32\\tools\\launch_bin_to_ihex_gui.cmd", False),
        ("3. Bootloader HEX：E:\\Project\\SP_Bootloader\\MDK-ARM\\Objects\\SP_CTR_Bootloader.hex", False),
        ("4. CTR package HEX 示例：E:\\Project\\SP_Pro_Base_Max_Org_good_bak\\bsp\\GD32F303RCT6\\build\\SP-PRO_CTR_APP.hex", False),
        ("", False),
        ("结论", True),
        ("本地模拟 OTA 的最小闭环是：起本地 HTTP 服务 -> 设备连同网 WiFi -> WIFI ALL DONE 后开始 probe -> 用户在 HMI 上确认（如果 autoUp=0）-> 继续执行 OTA。真实 CTR 联调前，只需要把本地模拟发 C 的逻辑关掉。", False),
    ]

    doc_xml = build_doc(lines)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(OUT, "w", ZIP_DEFLATED) as zf:
        zf.writestr("[Content_Types].xml", CONTENT_TYPES)
        zf.writestr("_rels/.rels", ROOT_RELS)
        zf.writestr("word/document.xml", doc_xml)
        zf.writestr("word/_rels/document.xml.rels", DOC_RELS)
        zf.writestr("docProps/core.xml", CORE_XML)
        zf.writestr("docProps/app.xml", APP_XML)
    print(OUT)


if __name__ == "__main__":
    main()
