# HA 设备面板（mihomo 综合面板）

把 **Home Assistant 设备数据**和 **mihomo 代理节点**合并到同一个网页，
顺带解决三个问题：

1. HA 的 REST API 不返回 `Access-Control-Allow-Origin`，浏览器跨域被拦
2. mihomo 启用 `secret` 后，UI 页面拿不到 `Authorization` 头
3. 令牌 / secret 不应该出现在前端源码里

---

## 架构

```
浏览器
  │  同源，无 CORS 问题
  ▼
192.168.71.74:8090   ← panel-proxy.py（systemd: panel-proxy.service）
  ├─ /ha/*      → 127.0.0.1:8123/api/*     服务端注入 HA 令牌
  ├─ /mihomo/*  → 127.0.0.1:9090/*         服务端注入 mihomo secret
  └─ /          → index.html（面板页面）
```

### 部署位置

| 文件 | 路径 |
|---|---|
| 反代服务 | `/home/orangepi/docker/panel/panel_proxy.py` |
| 密钥文件 | `/home/orangepi/docker/panel/panel-secrets.json`（`chmod 600`） |
| 面板页面 | `/home/orangepi/docker/panel/index.html` |
| systemd 单元 | `/etc/systemd/system/panel-proxy.service` |

密钥文件格式：
```json
{
  "ha_token": "<HA 长期访问令牌>",
  "mihomo_secret": "<mihomo secret>"
}
```

### 访问入口

| 入口 | 说明 |
|---|---|
| `http://192.168.71.74:8090/` | **推荐**。设备卡片 + 代理组，无需任何 token |
| `http://192.168.71.74:9090/ui/` | mihomo 自带入口。首次弹窗要 secret，存 localStorage |

---

## 面板功能

### 设备卡片（每 15 秒自动刷新）

- **卧室吸顶灯** — 开关按钮
  `POST /ha/services/switch/turn_on|turn_off` → HA → shell_command → ESP8266 → 433
- **空调** — 大号实时功率（W）+ 明细行（今日 / 本周 / 本月 / 本年）

数据来源实体：

| 显示 | 实体 |
|---|---|
| 实时功率 | `sensor.lumi_mcn02_c14b_electric_power` |
| 今日 | `sensor.lumi_mcn02_c14b_power_cost_today` |
| 本周 | `sensor.ac_energy_week_kwh` |
| 本月 | `sensor.lumi_mcn02_c14b_power_cost_month` |
| 本年 | `sensor.ac_energy_year_kwh` |
| 吸顶灯开关 | `switch.living_room_ceiling` |

### 代理组与节点

原自写面板的全部功能：延迟测试、策略组切换、主策略组标识。

---

## 页面的双入口自适应

同一份 HTML 要在 8090（反代）和 9090（mihomo 自带）都能用：

```js
const PANEL_PORT = '8090';
const API = location.port === PANEL_PORT
  ? location.origin + '/mihomo'   // 8090：走反代，secret 服务端注入
  : location.origin;              // 9090：同源直连，需要前端带 secret
const NEEDS_SECRET = location.port !== PANEL_PORT;
```

8090 上 `api()` 不加任何头；9090 上从 `localStorage` 取 secret（首次 `prompt()` 输入），
加 `Authorization: Bearer <secret>` 头。

---

## mihomo secret

在 `config.yaml` **末尾**加一行：

```yaml
secret: "<你的 secret>"
```

生成方式：

```bash
python3 -c "import base64,os;print(base64.urlsafe_b64encode(os.urandom(24)).decode().rstrip('=')[:32])"
```

### 行为（实测）

| 请求 | 加 secret 前 | 加 secret 后 |
|---|---|---|
| UI 页面 `/ui/` | 200 | **200（不变）** |
| API `/proxies` | 200 | **401** |
| API + `Authorization: Bearer <secret>` | — | 200 |
| `?token=xxx` 查询参数 | — | **401（无效）** |

**secret 只保护 API，UI 页面照样能打开**——所以自写页面必须自己带 Authorization 头。

---

## 运维命令

```bash
# 面板服务
sudo systemctl status panel-proxy
sudo systemctl restart panel-proxy
sudo journalctl -u panel-proxy -n 20 --no-pager

# 轮换 mihomo secret（改完要重启 mihomo 和面板）
#   1. python 脚本改 config.yaml 末尾的 secret: 行（不要用 printf，会丢引号）
#   2. python 脚本改 panel-secrets.json 的 mihomo_secret
#   3. sudo systemctl restart mihomo && sudo systemctl restart panel-proxy

# 验证
curl -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8090/
curl -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8090/ha/states
curl -o /dev/null -w '%{http_code}\n' http://127.0.0.1:8090/mihomo/proxies
curl -o /dev/null -w '%{http_code}\n' http://127.0.0.1:9090/proxies          # 应 401
```

---

## 踩坑记录

### 1. `printf 'secret: \"xxx\"'` 经 `sudo -S` 会失效

`sed`/`printf` 的引号在多层转义下会丢。**改用 python 脚本写文件**：

```python
p = '/home/orangepi/docker/mihomo/config.yaml'
s = open(p, encoding='utf-8').read()
lines = [l for l in s.split('\n') if not l.startswith('secret:')]
s = '\n'.join(lines).rstrip() + '\nsecret: "NEWVALUE"\n'
open(p, 'w', encoding='utf-8').write(s)
```

同理 `panel-secrets.json` 用 `json.load` / `json.dump` 改，别手拼 JSON。

### 2. `systemctl enable` 报 "Interactive authentication required"

`echo 'pw' | sudo -S -p '' cmd` 只对**单条**命令生效。
`daemon-reload`、`enable`、`restart` 要**分别**执行，每次都带 sudo 前缀。

### 3. 构建脚本自我污染

如果 UI 页面的构建脚本以「本地文件」为源，部署时又把结果写回同一个本地文件，
下次构建就会**重复注入**同样内容（`const API` 出现两次、函数重复定义）。

**做法**：始终从干净的原始版本构建。
本页面的干净源保存在 Orange Pi 的 `mihomo/ui/index.html.bak`（部署前自动备份）。

### 4. `paramiko` 里的 `%` 需要转义

```python
"HTTP %{http_code}" % x     # ValueError: unsupported format character
"HTTP %%{http_code}" % x    # 正确
```
或者干脆不用 paramiko 的格式化，直接用本地 `curl` 验证。