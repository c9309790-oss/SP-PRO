from __future__ import annotations

import html
from pathlib import Path
from zipfile import ZipFile, ZIP_DEFLATED


ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "build" / "CTR_Keil_烧录说明_20260410.docx"


def xml_escape(text: str) -> str:
    return html.escape(text, quote=False)


def make_paragraph(text: str, bold: bool = False) -> str:
    safe = xml_escape(text)
    if not safe:
        return (
            "<w:p><w:r><w:t xml:space=\"preserve\"> </w:t></w:r></w:p>"
        )
    run_props = "<w:rPr><w:b/></w:rPr>" if bold else ""
    return (
        "<w:p>"
        "<w:r>"
        f"{run_props}"
        f"<w:t xml:space=\"preserve\">{safe}</w:t>"
        "</w:r>"
        "</w:p>"
    )


def build_document_xml(lines: list[tuple[str, bool]]) -> str:
    body = "".join(make_paragraph(text, bold) for text, bold in lines)
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
        f"<w:body>{body}{sect}</w:body>"
        "</w:document>"
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
  <dc:title>CTR Keil 烧录说明</dc:title>
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
    lines: list[tuple[str, bool]] = [
        ("CTR Keil 烧录说明", True),
        ("", False),
        ("一、30 秒烧录前检查清单", True),
        ("1. 确认 bootloader 文件使用 SP_CTR_Bootloader.hex。", False),
        ("2. 确认 app 文件使用带 2KB OTA 头的 CTR package，而不是 raw app.bin。", False),
        ("3. 你的当前 package 文件是 SP_CTR_APP_CRC_20260410_09_23_46.bin，对应的 HEX 是 SP_CTR_APP_CRC_20260410_09_23_46_at_08003800.hex。", False),
        ("4. 确认 bootloader 地址是 0x08000000。", False),
        ("5. 确认 CTR package 地址是 0x08003800。", False),
        ("6. 确认 app 工程 IROM1 链接地址是 0x08004000，Size 是 0x0003B000。", False),
        ("7. 不要把 package 当成 raw app 烧到 0x08004000。", False),
        ("8. 不要改 option bytes / read protection。", False),
        ("9. 烧录时保证供电稳定，不要中途断电。", False),
        ("10. 如果下载器不支持两个文件一起烧，就分两次烧。", False),
        ("", False),
        ("二、Keil 内如何设置", True),
        ("1. Bootloader 工程 IROM1：Start = 0x08000000，Size = 0x00003800。", False),
        ("2. CTR App 工程 IROM1：Start = 0x08004000，Size = 0x0003B000。", False),
        ("3. IROM1 是链接地址 / 运行地址，不是烧录阶段临时填写的地址。", False),
        ("4. 如果你烧录的是 HEX，地址已经在 HEX 文件内部，不需要在烧录阶段再次填写 IROM1。", False),
        ("", False),
        ("三、如何用工具把 BIN 生成 HEX", True),
        ("1. 双击 E:\\Project\\SP-Pro-ESP32\\tools\\launch_bin_to_ihex_gui.cmd。", False),
        ("2. 选择输入 BIN。", False),
        ("3. 选择输出 HEX。", False),
        ("4. 如果输入文件是 CTR Package BIN (2KB OTA header)，推荐地址必须是 0x08003800。", False),
        ("5. 如果输入文件是 Raw APP BIN，推荐地址必须是 0x08004000。", False),
        ("6. 点“生成并校验”。", False),
        ("7. 结果区需要同时满足：自动识别正确、HEX 首地址正确、0x800 向量表存在、头部长度校验成立、最终状态是绿色 SUCCESS。", False),
        ("8. 你这次已经生成好的文件是 E:\\Project\\SP-Pro-ESP32\\build\\SP_CTR_APP_CRC_20260410_09_23_46_at_08003800.hex。", False),
        ("", False),
        ("四、如何烧录", True),
        ("方式 A：如果 Keil 下载器支持附加多个 HEX。", False),
        ("1. 打开任意一个 GD32F303 工程。", False),
        ("2. 进入 Options for Target -> Utilities -> Settings。", False),
        ("3. 确认 Flash Download 算法是对应 GD32F303 的内部 Flash 算法。", False),
        ("4. 添加 bootloader 文件：E:\\Project\\SP_Bootloader\\MDK-ARM\\Objects\\SP_CTR_Bootloader.hex。", False),
        ("5. 添加 app 文件：E:\\Project\\SP-Pro-ESP32\\build\\SP_CTR_APP_CRC_20260410_09_23_46_at_08003800.hex。", False),
        ("6. 执行下载。", False),
        ("", False),
        ("方式 B：如果 Keil 下载器不支持多个 HEX，一次烧一个文件。", False),
        ("1. 先烧 E:\\Project\\SP_Bootloader\\MDK-ARM\\Objects\\SP_CTR_Bootloader.hex。", False),
        ("2. 再烧 E:\\Project\\SP-Pro-ESP32\\build\\SP_CTR_APP_CRC_20260410_09_23_46_at_08003800.hex。", False),
        ("3. 两个地址不重叠，按这个顺序烧即可。", False),
        ("", False),
        ("五、烧录后如何快速验证", True),
        ("1. 上电后设备不能卡死或持续复位。", False),
        ("2. 如果 bootloader 标志被设置，CTR 应该能进入 bootloader 并发出 C。", False),
        ("3. 如果正常跳应用，说明 0x08004000 的 app 向量表和 CRC 头已经匹配。", False),
        ("4. 若异常，优先检查：文件类型是否拿错、HEX 地址是否正确、是否误把 package 烧到 0x08004000。", False),
        ("", False),
        ("六、这次实际要用的文件", True),
        ("1. Bootloader HEX：E:\\Project\\SP_Bootloader\\MDK-ARM\\Objects\\SP_CTR_Bootloader.hex", False),
        ("2. CTR Package BIN：E:\\Project\\SP_Pro_Base_Max_Org_good_bak\\bsp\\GD32F303RCT6\\SP_CTR_APP_CRC_20260410_09_23_46.bin", False),
        ("3. 已转换的 CTR Package HEX：E:\\Project\\SP-Pro-ESP32\\build\\SP_CTR_APP_CRC_20260410_09_23_46_at_08003800.hex", False),
        ("", False),
        ("结论", True),
        ("你这次不需要 P400，也可以只用 Keil 下载器完成烧录。前提是：bootloader 用 bootloader.hex，CTR app 用带 2KB OTA 头并且地址为 0x08003800 的 package hex。", False),
    ]

    doc_xml = build_document_xml(lines)
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
