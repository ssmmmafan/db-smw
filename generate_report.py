#!/usr/bin/env python3
"""Generate RMDB experiment report as docx (no external deps)."""

import zipfile
from datetime import datetime
from xml.sax.saxutils import escape

OUTPUT = "/home/smw/db-smw/RMDB实验报告.docx"

# ---- document body (plain text paragraphs / headings) ----
SECTIONS = [
    ("title", "RMDB 数据库管理系统实验报告"),
    ("meta", "实验平台：RMDB（全国大学生计算机系统能力大赛数据库管理系统赛道）"),
    ("meta", "开发语言：C++17    构建工具：CMake    操作系统：Linux (WSL2)"),
    ("meta", f"报告日期：{datetime.now().strftime('%Y年%m月%d日')}"),
    ("blank", ""),
    ("h1", "一、实验概述"),
    (
        "p",
        "本实验基于中国人民大学数据库教学团队提供的 RMDB 框架，在存储、索引、查询执行、"
        "事务并发控制与日志恢复等模块上进行实现与优化。本文选取以下三个具有代表性的任务进行总结："
        "（1）块嵌套循环连接（BNLJ）查询性能优化；（2）基于间隙锁的并发控制与幻读预防；"
        "（3）带 B+ 树索引的 WAL 崩溃恢复。三项任务分别对应查询执行、并发控制与恢复子系统，"
        "体现了关系型数据库内核的核心能力。",
    ),
    ("blank", ""),
    ("h1", "二、实验环境与工具"),
    ("p", "• 操作系统：Ubuntu / WSL2 (Linux 6.x)"),
    ("p", "• 编译器：GCC，C++17 标准"),
    ("p", "• 构建：CMake，Release 模式（-O3 优化）"),
    ("p", "• 测试：本地 Python 脚本 + 平台自动化评测（join_test、phantom_read_test、crash_recovery_index_test 等）"),
    ("p", "• 主要涉及目录：src/execution/、src/transaction/、src/recovery/、src/index/"),
    ("blank", ""),
    ("h1", "三、实验任务一：块嵌套循环连接（BNLJ）性能优化"),
    ("h2", "3.1 任务目标"),
    (
        "p",
        "在大规模等值连接场景下，朴素嵌套循环连接的时间复杂度为 O(n×m)，"
        "且逐条 I/O 开销巨大。本任务要求实现块嵌套循环连接（Block Nested Loop Join），"
        "通过批量读入外表元组块、减少内表扫描次数，并在纯等值连接条件下采用哈希探测进一步加速，"
        "使 join_test 等性能测试在时限内通过。",
    ),
    ("h2", "3.2 设计思路"),
    ("p", "（1）块缓冲：将左表（外表）元组按 JOIN_BUFFER_SIZE 分批读入内存块，对内表逐块扫描，降低重复 I/O。"),
    ("p", "（2）哈希探测优化：当连接条件均为等值连接（OP_EQ）时，对当前外表块按连接键建立哈希表，内表逐条 probe，避免 O(n²) 逐对比较。"),
    ("p", "（3）编译与日志优化：Release 模式开启 -O3；减少 WAL 每条日志强制刷盘，仅在事务提交/中止及 DML 持久化点刷盘，降低 I/O 瓶颈。"),
    ("h2", "3.3 关键实现"),
    ("p", "主要修改文件：executor_nestedloop_join.h、common/config.h、CMakeLists.txt、log_manager.cpp、transaction_manager.cpp。"),
    ("p", "• NestedLoopJoinExecutor 增加 left_block_buf_ 与 eq_hash_，在 is_pure_equi_join() 为真时启用 hash_probe 路径。"),
    ("p", "• 哈希桶耗尽后必须推进内表游标，避免在同一 Rid 上死循环（曾导致 join_test_1 服务端挂起）。"),
    ("p", "• JOIN_BUFFER_SIZE 设为 1GB，BUFFER_POOL_SIZE 256MB，在内存允许范围内最大化块大小。"),
    ("h2", "3.4 实验结果"),
    ("p", "优化前：50k×50k 等值连接耗时约 363s，勉强通过；错误实现曾出现无限循环导致 join_test_1 失败。"),
    ("p", "优化后：哈希探测 BNLJ + Release 编译 + WAL 刷盘策略优化，大规模 join 测试稳定通过，得分达到 10 分（join 相关用例全部通过）。"),
    ("h2", "3.5 小结"),
    ("p", "本任务体现了查询执行层“算法 + 工程”双重优化：算法上从 O(n×m) 降至接近 O(n+m)，工程上减少日志与编译开销，二者结合才能通过极限性能测试。"),
    ("blank", ""),
    ("h1", "四、实验任务二：并发控制与幻读预防（间隙锁）"),
    ("h2", "4.1 任务目标"),
    (
        "p",
        "实现严格两阶段封锁（2PL）与 No-Wait 死锁预防策略，并在索引范围扫描场景下防止幻读（Phantom Read）。"
        "平台测试 phantom_read_test_3/4 要求：事务 T1 在索引范围查询持有共享间隙锁时，"
        "其他事务的插入/删除/更新若冲突应立刻 abort，T1 两次查询结果保持一致。",
    ),
    ("h2", "4.2 设计思路"),
    ("p", "（1）2PL + No-Wait：加锁失败立即抛出 TransactionAbortException，返回 abort\\n，不等待、不回滚后重试。"),
    ("p", "（2）索引扫描加共享间隙锁：IndexScanExecutor 在 txn_mode=true 时对扫描范围加 IS 型 gap lock。"),
    ("p", "（3）写操作加排他间隙锁：INSERT/DELETE/UPDATE 在修改索引键对应位置加 IX 型 gap lock，与读锁冲突则 abort。"),
    ("p", "（4）读锁策略：仅显式事务（begin 后 txn_mode=true）对读加 S 锁；自动提交单语句读不加锁，保证性能与语义平衡。"),
    ("h2", "4.3 关键实现"),
    ("p", "主要修改文件：lock_manager.cpp、executor_index_scan.h、executor_utils.h、executor_insert/delete/update.h。"),
    ("p", "• lock_shared_on_gap_range()：根据索引条件下界/上界构造 key 范围，调用 lock_shared_on_gap_range。"),
    ("p", "• lock_exclusive_gap_for_record()：对每条待写记录在全部索引上申请排他间隙锁。"),
    ("p", "• DeleteExecutor 顺序：先 delete_record 再 delete_index，避免删除失败时索引已删导致数据不一致。"),
    ("p", "• fill_min/fill_max 支持 INT/FLOAT/BIGINT/DATETIME，保证 BIGINT 索引范围扫描边界正确。"),
    ("h2", "4.4 测试场景"),
    ("p", "• phantom_read_test_3：T1 查询 id∈(2,4) 得 0 行；T2 插入 id=3 应 abort；T1 再次查询仍为 0 行。"),
    ("p", "• phantom_read_test_4：多事务并发，T2 删除 / T3 插入 / T4 更新均 abort，T1 查询正常。"),
    ("h2", "4.5 实验结果"),
    ("p", "本地 test_phantom.py、test_phantom_v4.py 验证通过；平台 phantom_read_test_3/4 通过后得分由 5 分恢复至 8 分及以上。"),
    ("h2", "4.6 小结"),
    ("p", "幻读预防的本质是在逻辑范围而非单行上互斥。B+ 树索引的间隙锁将“范围查询”与“范围内写”统一纳入 2PL 框架，是 InnoDB 等工业数据库的经典做法。"),
    ("blank", ""),
    ("h1", "五、实验任务三：带索引的 WAL 崩溃恢复"),
    ("h2", "5.1 任务目标"),
    (
        "p",
        "实现基于 WAL 的 ARIES 风格崩溃恢复：系统 crash 后重启，已提交事务持久化、未提交事务回滚，"
        "且 B+ 树索引与堆表数据一致，索引范围扫描结果正确。"
        "对应平台测试 crash_recovery_index_test、crash_recovery_multi_thread_test。",
    ),
    ("h2", "5.2 设计思路"),
    ("p", "恢复三阶段：Analyze → Redo → Undo。"),
    ("p", "（1）Analyze：扫描 WAL，维护 ATT（活跃事务表）；仅对有 DML 日志的表 destroy/recreate 索引文件，避免误清空无 DML 表的已有索引。"),
    ("p", "（2）Redo：先 rollback(true) 清空脏页，再重放已提交事务的 INSERT/UPDATE/DELETE 日志。"),
    ("p", "（3）Undo：rollback(false) 回滚未提交事务；rebuild_indexes_from_table() 按堆表全量重建索引，保证索引与表一致。"),
    ("p", "（4）持久化：DML 执行后 persist_wal()；crash 命令前 flush WAL 与脏页；恢复后 set_next_lsn / set_next_txn_id 避免 ID 冲突。"),
    ("h2", "5.3 关键实现与 Bug 修复"),
    ("p", "主要修改文件：log_recovery.cpp、log_recovery.h、executor_utils.h、rmdb.cpp、ix_manager.h（close_index 清除 buffer pool 缓存页）。"),
    ("p", "• 根因1：redo 曾重放未提交事务 DML，导致 crash 后脏数据残留 → 修复为仅重放 committed_txns_。"),
    ("p", "• 根因2：analyze 曾重建所有表索引，redo 只恢复部分表 → 修复为仅 tables_with_dml_ 重建。"),
    ("p", "• 根因3：索引文件 destroy 后 buffer pool 仍缓存旧 fd 页面 → close_index 调用 remove_all_pages。"),
    ("p", "• 根因4：rebuild 前 undo 可能向索引写入脏条目 → rebuild 前先 destroy/recreate 索引再插入。"),
    ("h2", "5.4 测试场景"),
    ("p", "• 两表场景：表 A 有已提交数据，表 B 未提交插入后 crash，恢复后 A 索引仍正确、B 无脏行。"),
    ("p", "• 已提交 UPDATE 索引列 + crash：索引扫描 id=2 命中，id=1 为空。"),
    ("p", "• 未提交 DELETE 回滚：两行均保留。"),
    ("p", "• BIGINT 索引范围扫描：恢复后范围查询结果正确。"),
    ("h2", "5.5 实验结果"),
    ("p", "本地单连接 crash 场景全部通过；与并发模块联调时须注意 undo 逻辑勿改 phantom 相关代码。crash_recovery_multi_thread_test 通过后，配合其他模块可达满分 10 分。"),
    ("h2", "5.6 小结"),
    ("p", "带索引的恢复难点在于堆表与 B+ 树的一致性。本实验采用“日志重放堆表 + 全量重建索引”策略，实现简单且与课程参考实现一致，适合 RMDB 框架的评测场景。"),
    ("blank", ""),
    ("h1", "六、综合总结"),
    (
        "p",
        "三项任务覆盖了 RMDB 内核的三条主线：查询执行（BNLJ）、事务并发（2PL + 间隙锁）、"
        "可靠性（WAL 恢复）。实现过程中体会到：性能优化需算法与 I/O 策略并重；"
        "并发正确性依赖锁粒度与加锁顺序的精细设计；恢复正确性则要求 Analyze/Redo/Undo 阶段"
        "对事务状态与索引生命周期严格区分。后续可进一步探索索引日志 redo、多版本并发控制（MVCC）等方向。",
    ),
    ("blank", ""),
    ("h1", "七、参考文献"),
    ("p", "[1] Database System Concepts (7th Edition), Silberschatz et al."),
    ("p", "[2] RMDB 项目文档：RMDB使用文档.pdf、RMDB项目结构.pdf、测试说明文档.pdf"),
    ("p", "[3] 全国大学生计算机系统能力大赛数据库管理系统赛道技术规范"),
]


def p(text, style=None):
    text = escape(text)
    if style == "Title":
        return (
            f'<w:p><w:pPr><w:jc w:val="center"/><w:spacing w:after="200"/></w:pPr>'
            f'<w:r><w:rPr><w:b/><w:sz w:val="44"/><w:szCs w:val="44"/></w:rPr>'
            f"<w:t>{text}</w:t></w:r></w:p>"
        )
    if style == "Heading1":
        return (
            f'<w:p><w:pPr><w:pStyle w:val="Heading1"/><w:spacing w:before="240" w:after="120"/></w:pPr>'
            f'<w:r><w:rPr><w:b/><w:sz w:val="32"/><w:szCs w:val="32"/></w:rPr>'
            f"<w:t>{text}</w:t></w:r></w:p>"
        )
    if style == "Heading2":
        return (
            f'<w:p><w:pPr><w:spacing w:before="180" w:after="80"/></w:pPr>'
            f'<w:r><w:rPr><w:b/><w:sz w:val="28"/><w:szCs w:val="28"/></w:rPr>'
            f"<w:t>{text}</w:t></w:r></w:p>"
        )
    if style == "Meta":
        return (
            f'<w:p><w:pPr><w:jc w:val="center"/><w:spacing w:after="60"/></w:pPr>'
            f'<w:r><w:rPr><w:color w:val="666666"/><w:sz w:val="22"/></w:rPr>'
            f"<w:t>{text}</w:t></w:r></w:p>"
        )
    # body paragraph
    return (
        f'<w:p><w:pPr><w:spacing w:after="120" w:line="360" w:lineRule="auto"/>'
        f'<w:ind w:firstLine="420"/></w:pPr>'
        f'<w:r><w:rPr><w:sz w:val="24"/><w:szCs w:val="24"/></w:rPr>'
        f"<w:t xml:space=\"preserve\">{text}</w:t></w:r></w:p>"
    )


def build_document_xml():
    parts = [
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
        '<w:document xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">',
        "<w:body>",
    ]
    for kind, text in SECTIONS:
        if kind == "title":
            parts.append(p(text, "Title"))
        elif kind == "h1":
            parts.append(p(text, "Heading1"))
        elif kind == "h2":
            parts.append(p(text, "Heading2"))
        elif kind == "meta":
            parts.append(p(text, "Meta"))
        elif kind == "blank":
            parts.append("<w:p/>")
        else:
            parts.append(p(text))
    parts.append(
        '<w:sectPr><w:pgSz w:w="11906" w:h="16838"/>'
        '<w:pgMar w:top="1440" w:right="1440" w:bottom="1440" w:left="1800" w:header="720" w:footer="720" w:gutter="0"/>'
        "</w:sectPr></w:body></w:document>"
    )
    return "".join(parts)


CONTENT_TYPES = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/word/document.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml"/>
  <Override PartName="/word/styles.xml" ContentType="application/vnd.openxmlformats-officedocument.wordprocessingml.styles+xml"/>
  <Override PartName="/docProps/core.xml" ContentType="application/vnd.openxmlformats-package.core-properties+xml"/>
  <Override PartName="/docProps/app.xml" ContentType="application/vnd.openxmlformats-officedocument.extended-properties+xml"/>
</Types>"""

RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="word/document.xml"/>
  <Relationship Id="rId2" Type="http://schemas.openxmlformats.org/package/2006/relationships/metadata/core-properties" Target="docProps/core.xml"/>
  <Relationship Id="rId3" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/extended-properties" Target="docProps/app.xml"/>
</Relationships>"""

DOC_RELS = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/styles" Target="styles.xml"/>
</Relationships>"""

STYLES = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<w:styles xmlns:w="http://schemas.openxmlformats.org/wordprocessingml/2006/main">
  <w:docDefaults><w:rPrDefault><w:rPr><w:rFonts w:ascii="宋体" w:hAnsi="宋体" w:eastAsia="宋体"/><w:sz w:val="24"/></w:rPr></w:rPrDefault></w:docDefaults>
  <w:style w:type="paragraph" w:styleId="Heading1"><w:name w:val="heading 1"/></w:style>
</w:styles>"""

CORE = f"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<cp:coreProperties xmlns:cp="http://schemas.openxmlformats.org/package/2006/metadata/core-properties"
  xmlns:dc="http://purl.org/dc/elements/1.1/" xmlns:dcterms="http://purl.org/dc/terms/"
  xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance">
  <dc:title>RMDB实验报告</dc:title>
  <dc:creator>RMDB实验</dc:creator>
  <dcterms:created xsi:type="dcterms:W3CDTF">{datetime.now().strftime('%Y-%m-%dT%H:%M:%S')}</dcterms:created>
</cp:coreProperties>"""

APP = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Properties xmlns="http://schemas.openxmlformats.org/officeDocument/2006/extended-properties">
  <Application>Python</Application></Properties>"""


def main():
    with zipfile.ZipFile(OUTPUT, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("[Content_Types].xml", CONTENT_TYPES)
        zf.writestr("_rels/.rels", RELS)
        zf.writestr("word/document.xml", build_document_xml())
        zf.writestr("word/_rels/document.xml.rels", DOC_RELS)
        zf.writestr("word/styles.xml", STYLES)
        zf.writestr("docProps/core.xml", CORE)
        zf.writestr("docProps/app.xml", APP)
    print(f"Generated: {OUTPUT}")


if __name__ == "__main__":
    main()
