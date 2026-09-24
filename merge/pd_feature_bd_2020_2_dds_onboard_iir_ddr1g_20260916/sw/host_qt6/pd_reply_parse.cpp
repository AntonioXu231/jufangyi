#include "pd_reply_parse.h"

#include <QRegularExpression>

namespace pdreply {

namespace {

/* 所有正则都一次性构造好。QRegularExpression 的构造有编译开销，
   放在这里避免每行回复都重新编译一遍模式。 */
const QRegularExpression &catalogExpr()
{
    static const QRegularExpression e(QStringLiteral(
        "^CATALOG event_seq=\\[(\\d+),(\\d+)\\) slots=\\d+ "
        "snap_seq=\\[(\\d+),(\\d+)\\) slots=\\d+ state=(\\d+)"));
    return e;
}

const QRegularExpression &eventMetaExpr()
{
    /* 板端格式：EVENT seq=N index=N addr=XXXXXXXX bytes=N peaks=N cycles=N
       注意 peaks/cycles 是必需字段——少了它们就无法判断这条记录里有没有峰值事件。 */
    static const QRegularExpression e(QStringLiteral(
        "^EVENT seq=(\\d+) index=(\\d+) addr=[0-9a-fA-F]+ bytes=(\\d+) peaks=(\\d+) cycles=(\\d+)"));
    return e;
}

const QRegularExpression &snapMetaExpr()
{
    /* 两种头：序号模式 "SNAP seq=" 与槽下标模式 "SNAP index="。 */
    static const QRegularExpression e(QStringLiteral(
        "^SNAP (?:seq=(\\d+) index=(\\d+)|index=(\\d+) seq=(\\d+)) "
        "hw_slot=\\d+ src=[0-9a-fA-F]+ dst=[0-9a-fA-F]+ bytes=(\\d+)"));
    return e;
}

const QRegularExpression &statusExpr()
{
    static const QRegularExpression e(QStringLiteral("^STATUS state=(\\d+)"));
    return e;
}

const QRegularExpression &scopeExpr()
{
    static const QRegularExpression e(QStringLiteral(
        "^SCOPE V1 seq=(\\d+) samples=(\\d+) bytes=(\\d+) fs=(\\d+) crc32=([0-9a-fA-F]{8})"));
    return e;
}

const QRegularExpression &dataExpr()
{
    static const QRegularExpression e(QStringLiteral(
        "^DATA V2 kind=(\\w+) index=(\\d+) offset=(\\d+) bytes=(\\d+) crc32=([0-9a-fA-F]{8})"));
    return e;
}

const QRegularExpression &prpdBinsExpr()
{
    static const QRegularExpression e(QStringLiteral(
        "^PRPD_BINS seq=\\[\\d+,\\d+\\) ch=(\\d+) first=(\\d+) count=(\\d+) values=([0-9,]*)$"));
    return e;
}

quint32 toU32(const QString &text)
{
    bool ok = false;
    const quint32 value = text.toUInt(&ok);
    return ok ? value : 0U;
}

} // namespace

Catalog parseCatalog(const QString &line)
{
    Catalog result;
    const auto match = catalogExpr().match(line);
    if (!match.hasMatch()) return result;
    result.ok = true;
    result.eventFirst = toU32(match.captured(1));
    result.eventNext = toU32(match.captured(2));
    result.snapFirst = toU32(match.captured(3));
    result.snapNext = toU32(match.captured(4));
    result.state = match.captured(5).toInt();
    return result;
}

EventMeta parseEventMeta(const QString &line)
{
    EventMeta result;
    const auto match = eventMetaExpr().match(line);
    if (!match.hasMatch()) return result;
    result.ok = true;
    result.sequence = toU32(match.captured(1));
    result.index = toU32(match.captured(2));
    result.bytes = toU32(match.captured(3));
    result.peaks = toU32(match.captured(4));
    result.cycles = toU32(match.captured(5));
    return result;
}

SnapMeta parseSnapMeta(const QString &line)
{
    SnapMeta result;
    const auto match = snapMetaExpr().match(line);
    if (!match.hasMatch()) return result;
    result.ok = true;
    /* 分支 1 是 "seq= index="，分支 2 是 "index= seq="。 */
    if (!match.captured(1).isEmpty()) {
        result.sequence = toU32(match.captured(1));
        result.index = toU32(match.captured(2));
    } else {
        result.index = toU32(match.captured(3));
        result.sequence = toU32(match.captured(4));
    }
    result.bytes = toU32(match.captured(5));
    return result;
}

StatusInfo parseStatus(const QString &line)
{
    StatusInfo result;
    const auto match = statusExpr().match(line);
    if (!match.hasMatch()) return result;
    result.ok = true;
    result.state = match.captured(1).toInt();
    return result;
}

ScopeHeader parseScopeHeader(const QString &line)
{
    ScopeHeader result;
    const auto match = scopeExpr().match(line);
    if (!match.hasMatch()) return result;
    result.ok = true;
    result.sequence = toU32(match.captured(1));
    result.samples = toU32(match.captured(2));
    result.bytes = toU32(match.captured(3));
    result.sampleRateHz = toU32(match.captured(4));
    result.crc32 = match.captured(5).toUInt(nullptr, 16);
    return result;
}

DataHeader parseDataHeader(const QString &line)
{
    DataHeader result;
    const auto match = dataExpr().match(line);
    if (!match.hasMatch()) return result;
    result.ok = true;
    result.kind = match.captured(1);
    result.index = toU32(match.captured(2));
    result.offset = toU32(match.captured(3));
    result.bytes = toU32(match.captured(4));
    result.crc32 = match.captured(5).toUInt(nullptr, 16);
    return result;
}

Config parseConfig(const QString &line)
{
    Config result;
    if (!line.startsWith(QStringLiteral("CONFIG api="))) return result;
    result.ok = true;

    static const QRegularExpression apiExpr(QStringLiteral("^CONFIG api=(\\d+)"));
    const auto apiMatch = apiExpr.match(line);
    if (apiMatch.hasMatch()) result.api = toU32(apiMatch.captured(1));

    /* scope 字段存在与否决定固件有没有 SCOPE 命令。 */
    static const QRegularExpression scopeExpr(
        QStringLiteral("\\bscope=(\\d+)/(\\d+)"));
    const auto scopeMatch = scopeExpr.match(line);
    if (scopeMatch.hasMatch()) {
        result.hasScope = true;
        result.scopeSamples = toU32(scopeMatch.captured(1));
        result.scopeMaxSamples = toU32(scopeMatch.captured(2));
    }
    /* 两个槽数分别查找，不假设字段顺序：板端 CONFIG 里 event_slots 排在
       snap_slots 之前，写成一条按序正则会被自检直接抓出来（确实抓到过一次）。 */
    static const QRegularExpression snapSlotsExpr(QStringLiteral("\\bsnap_slots=(\\d+)"));
    static const QRegularExpression eventSlotsExpr(QStringLiteral("\\bevent_slots=(\\d+)"));
    const auto snapSlotsMatch = snapSlotsExpr.match(line);
    if (snapSlotsMatch.hasMatch()) result.snapSlots = toU32(snapSlotsMatch.captured(1));
    const auto eventSlotsMatch = eventSlotsExpr.match(line);
    if (eventSlotsMatch.hasMatch()) result.eventSlots = toU32(eventSlotsMatch.captured(1));
    return result;
}

PrpdBins parsePrpdBins(const QString &line)
{
    PrpdBins result;
    const auto match = prpdBinsExpr().match(line);
    if (!match.hasMatch()) return result;
    result.channel = match.captured(1).toInt();
    result.first = match.captured(2).toInt();
    result.count = match.captured(3).toInt();
    const QStringList parts = match.captured(4).split(',');
    /* count 与实际数值个数必须一致，否则宁可判定为解析失败——
       半截的桶数组画出来的直方图是错的，比没有数据更糟。 */
    if (parts.size() != result.count) return result;
    result.values.reserve(parts.size());
    for (const QString &part : parts) result.values.append(toU32(part));
    result.ok = true;
    return result;
}

} // namespace pdreply
