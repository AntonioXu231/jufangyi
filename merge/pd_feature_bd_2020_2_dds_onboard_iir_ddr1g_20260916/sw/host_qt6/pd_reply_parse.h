#pragma once

#include <QString>
#include <QVector>

/*
 * 板端文本回复的解析，集中在这里。
 *
 * 为什么单独成模块：解析格式一旦与板端不一致，上位机的表现就是"没有数据"，
 * 而不是报错——排查成本极高。集中之后可以用真实板端日志原文做自检测试
 * （见 selftest.cpp 的 testBoardReplies），把格式契约钉死。
 *
 * 每个 parseXxx 都返回带 ok 标志的结构；ok=false 表示这一行不是该种回复，
 * 调用方必须据此跳过，不得把解析失败的默认值当成真实数值。
 *
 * 回复格式出处：sw/ps_service/tcp/pd_tcp_service.c 的 snprintf 模板，
 * 以及 2026-09-23 上板实抓的原始日志。
 */
namespace pdreply {

struct Catalog {
    bool ok = false;
    quint32 eventFirst = 0;
    quint32 eventNext = 0;
    quint32 snapFirst = 0;
    quint32 snapNext = 0;
    int state = 0;
};

struct Config {
    bool ok = false;
    quint32 api = 0;
    /*
     * 板端是否支持 SCOPE 实时取帧命令。
     * 判据是 CONFIG 回复里有没有 scope=<当前>/<上限> 这两个字段——
     * 这是唯一可靠的固件能力探测方式：若 ELF 里没有 SCOPE 命令，
     * 发 SCOPE ON 只会得到 "ERR unknown command"，波形永远出不来。
     */
    bool hasScope = false;
    quint32 scopeSamples = 0;
    quint32 scopeMaxSamples = 0;
    quint32 snapSlots = 0;
    quint32 eventSlots = 0;
};

struct EventMeta {
    bool ok = false;
    quint32 sequence = 0;
    quint32 index = 0;
    quint32 bytes = 0;
    quint32 peaks = 0;
    quint32 cycles = 0;
};

struct SnapMeta {
    bool ok = false;
    quint32 sequence = 0;
    quint32 index = 0;
    quint32 bytes = 0;
};

struct StatusInfo {
    bool ok = false;
    int state = 0;
};

struct ScopeHeader {
    bool ok = false;
    quint32 sequence = 0;
    quint32 samples = 0;
    quint32 bytes = 0;
    quint32 sampleRateHz = 0;
    quint32 crc32 = 0;
};

struct DataHeader {
    bool ok = false;
    QString kind;
    quint32 index = 0;
    quint32 offset = 0;
    quint32 bytes = 0;
    quint32 crc32 = 0;
};

struct PrpdBins {
    bool ok = false;
    int channel = 0;
    int first = 0;
    int count = 0;
    QVector<quint32> values;
};

Catalog parseCatalog(const QString &line);
EventMeta parseEventMeta(const QString &line);
SnapMeta parseSnapMeta(const QString &line);
StatusInfo parseStatus(const QString &line);
ScopeHeader parseScopeHeader(const QString &line);
DataHeader parseDataHeader(const QString &line);
Config parseConfig(const QString &line);
PrpdBins parsePrpdBins(const QString &line);

} // namespace pdreply
