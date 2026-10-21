#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
从 HA recorder 的 statistics 表计算空调电量的周/月/年聚合。

关键点（实测得出，别改）：
  1. 实体名在 statistics_meta.**statistic_id** 列，不是 name 列
  2. 累计型实体（power_cost_today/month）读数在 statistics.**state** 列；
     均值型实体（electric_power）在 **mean** 列（state 为 NULL）
  3. power_cost_today 是「当日累计读数」，每天 0 点归零
     -> 每天的耗电 = 当天最后一个小时的 state 值
     -> 周/月/年 = 累加各自包含的每日耗电
  4. statistics 表是 hourly 粒度（每小时一行），实测 24 条/天
  5. 不能用 sum 列累加（它是读数的累加，会得到荒谬的 36 万 kWh）

部署：Orange Pi 上由 panel-proxy 挂载调用，或独立 cron。
只读，不修改数据库。
"""
import datetime
import json
import os
import sqlite3
import sys
from collections import defaultdict

# 宿主机路径（容器内是 /config/...）
DB_CANDIDATES = [
    os.environ.get("HA_DB", ""),
    "/home/orangepi/docker/home_assistant/config/home-assistant_v2.db",
    "/config/home-assistant_v2.db",
]
ENTITIES = {
    "today": "sensor.lumi_mcn02_c14b_power_cost_today",
    "month": "sensor.lumi_mcn02_c14b_power_cost_month",
    "power": "sensor.lumi_mcn02_c14b_electric_power",
}


def find_db():
    for p in DB_CANDIDATES:
        if p and os.path.isfile(p):
            return p
    raise SystemExit("找不到 HA 数据库")


def fetch_daily(con, entity_id):
    """取每天最后一个 state 值 = 当天耗电 kWh"""
    cur = con.cursor()
    cur.execute(
        "SELECT s.start_ts, s.state FROM statistics s "
        "JOIN statistics_meta m ON m.id = s.metadata_id "
        "WHERE m.statistic_id = ? ORDER BY s.start_ts",
        (entity_id,),
    )
    daily = {}
    for ts, state in cur.fetchall():
        if state is None:
            continue
        d = datetime.datetime.fromtimestamp(ts)
        key = d.strftime("%Y-%m-%d")
        # 同一天保留时间最晚的那条
        if key not in daily or ts > daily[key][0]:
            daily[key] = (ts, float(state))
    return {k: v[1] for k, v in daily.items()}


def fetch_hourly(con, entity_id):
    """取每天的小时级读数序列：{日期: [24 个值, 缺失为 null]}

    用于「日报表」按小时展示曲线。
    """
    cur = con.cursor()
    cur.execute(
        "SELECT s.start_ts, s.state FROM statistics s "
        "JOIN statistics_meta m ON m.id = s.metadata_id "
        "WHERE m.statistic_id = ? ORDER BY s.start_ts",
        (entity_id,),
    )
    hourly = {}
    for ts, state in cur.fetchall():
        d = datetime.datetime.fromtimestamp(ts)
        key = d.strftime("%Y-%m-%d")
        hour = d.hour
        hourly.setdefault(key, [None] * 24)[hour] = float(state) if state is not None else None
    return hourly


def aggregate(daily, mode):
    """按 week / month / year 聚合"""
    out = defaultdict(float)
    for d, v in daily.items():
        dt = datetime.datetime.strptime(d, "%Y-%m-%d")
        if mode == "day":
            key = d
        elif mode == "week":
            iso = dt.isocalendar()
            key = "%d-W%02d" % (iso[0], iso[1])
        elif mode == "month":
            key = d[:7]
        elif mode == "year":
            key = d[:4]
        else:
            raise ValueError("bad mode: " + mode)
        out[key] += v
    return dict(sorted(out.items()))


def main():
    db = find_db()
    con = sqlite3.connect("file:" + db + "?mode=ro", uri=True)

    daily = fetch_daily(con, ENTITIES["today"])
    hourly = fetch_hourly(con, ENTITIES["today"])
    result = {
        "source": db,
        "days": len(daily),
        "range": [min(daily) if daily else None, max(daily) if daily else None],
        "daily": {k: round(v, 4) for k, v in sorted(daily.items())},
    }
    for mode in ("day", "week", "month", "year"):
        agg = aggregate(daily, mode)
        result[mode] = {k: round(v, 4) for k, v in agg.items()}

    # 小时级明细（仅日报视图用；只有被查询到的那天才需要，故按需带出）
    result["hourly"] = {
        k: [None if v is None else round(v, 4) for v in vals]
        for k, vals in sorted(hourly.items())
    }

    # 实时功率：power 实体是「均值型」，值在 mean 列（state 为 NULL）
    cur = con.cursor()
    cur.execute(
        "SELECT s.start_ts, s.mean FROM statistics s "
        "JOIN statistics_meta m ON m.id = s.metadata_id "
        "WHERE m.statistic_id = ? ORDER BY s.start_ts DESC LIMIT 1",
        (ENTITIES["power"],),
    )
    row = cur.fetchone()
    if row:
        result["power_now"] = row[1]
        result["power_at"] = datetime.datetime.fromtimestamp(row[0]).strftime(
            "%Y-%m-%d %H:%M")

    json.dump(result, sys.stdout, ensure_ascii=False, separators=(",", ":"))
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
