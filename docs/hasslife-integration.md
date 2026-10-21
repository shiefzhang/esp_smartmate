# HassLife 接入 ESP8266（ESP SmartMate）

## 结论先说

**可以，但需要中间层 Home Assistant。**

HassLife 只支持 Home Assistant，不支持 Blinker/易微联。所以链路是：

```
天猫精灵 → HassLife 云 → Home Assistant → HTTP → ESP8266 → 433 → 吸顶灯
```

这反而是个优势：**绕开了 Blinker 与天猫精灵的关联问题**（你当前遇到的那个），
天猫侧只和 HassLife 打交道，ESP8266 走局域网页面接口，稳定可控。

## 前置条件

- Home Assistant 部署完成且可访问（内网或公网隧道）
- ESP8266 与 HA 在同一局域网，本机实测地址 `http://192.168.71.4`
- 吸顶灯码位已录入 ESP8266 的 433 slot 1（2/3 号位同理）

## 第一步：确认 ESP8266 接口可用

HA 要调用的接口（已实测返回 HTTP 200 并触发 `RF TX source=HTTP`）：

```text
GET http://192.168.71.4/send?slot=1
```

slot 取值 1-6，对应固件里保存的 433 码位。先在浏览器里点一下确认灯会亮，
**这一步成功再往下做**。

> 接口执行成功后返回 `302` 重定向到 `/#rf`（网页界面）。
> 用 `curl` 调试时加 `-L` 跟随跳转，或直接看设备日志确认：
> `RF TX source=HTTP slot=1 ...` 就是发射成功了。
> 用 Python `urllib` 时会抛 `HTTPError 404`——**这是重定向导致的，不是失败**，
> 实际已发射。HA 的 `rest_command` 同样会记录这个 404，属正常现象。

其他可用接口：

| 接口 | 作用 |
|---|---|
| `GET /send?slot=N` | 发射第 N 个 433 码位（推荐用这个） |
| `GET /ir-send?slot=N` | 发射第 N 个红外码位 |
| `GET /logs` | 查看运行日志 |
| `GET /rf-status` | 查询 433 学习状态 |

## 第二步：HA 里配置 REST 开关

编辑 `configuration.yaml`：

```yaml
rest_command:
  # 吸顶灯：433 slot 1（slot 2/3 照抄，改 URL 里的数字）
  light_ceiling_on:
    url: "http://192.168.71.4/send?slot=1"
    method: "get"
    timeout: 10

  light_ceiling_off:
    url: "http://192.168.71.4/send?slot=1"
    method: "get"
    timeout: 10

switch:
  - platform: rest
    name: "吸顶灯"
    resource: "http://192.168.71.4/logs"
    # 用 rest_command 更直接，switch 适合需要状态回读的场景
    command_on: ""
    command_off: ""
```

**更推荐的做法**——用 `switch` 模板 + rest_command，让 HA 能读回状态：

```yaml
switch:
  - platform: command_line
    switches:
      living_room_light:
        friendly_name: "客厅吸顶灯"
        command_on: "curl -s 'http://192.168.71.4/send?slot=1'"
        command_off: "curl -s 'http://192.168.71.4/send?slot=1'"
        command_state: "/bin/false"
        value_template: "{{ value_json.state | default('on') }}"
```

**433 固定码的注意点**：吸顶灯用的是固定码，只有"触发"没有"状态"，
所以开和关发的是**同一个码**。如果你要区分开关状态，改用 GPIO 接继电器
控制市电，或给固件加一个状态变量（当前 `voiceOutletState[]` 只在语音路径更新）。

> ⚠️ 如果你希望 HA 能准确显示灯是开是关，固定码方案做不到——
> 它只能"发指令"，不能"读状态"。需要状态回读就得改成继电器方案。

## 第三步：接入 HassLife

1. 注册并登录 https://hass.blear.cn
2. HA 里 `配置 → 集成 → 添加集成 → 搜 HassLife`，填 HassLife 账号密码
3. HassLife 网页端确认插件状态显示**在线**
4. `设备管理 → 添加 Homeassistant 设备`，勾选刚建的吸顶灯 switch
5. 点`配置`，选**天猫精灵**平台，**设备类型选「灯」**，填语控名称
6. 天猫精灵 App → 精灵技能 → 搜 **HassLife** → 绑定 HassLife 账号
7. 回到精灵家，设备就出来了。说「**打开客厅吸顶灯**」

设备类型选「灯」很重要——HassLife 会按灯来渲染控制面板和语音指令。

## 第四步（可选）：小度 / 小爱同学

同一套 HA 设备，改绑平台即可：

- **小度**：小度 App → 添加设备 → 搜 HassLife
- **小爱同学**：米家 App → 其他平台设备 → 搜 HassLife

HassLife 支持灯/开关/窗帘/风扇/空调/传感器等类型，够用。

## 排障速查

| 现象 | 检查 |
|---|---|
| HA 里点开关没反应 | 先在浏览器直接访问 `http://192.168.71.4/send?slot=1` |
| 浏览器能开，HA 不行 | HA 容器网络不通主机 IP，确认同网段或用 host 网络 |
| HassLife 显示插件离线 | HA 重启，或检查外网/令牌 |
| 天猫能看到设备但语音不执行 | 设备类型是否为「灯」，语控名称是否含品类词 |
| 状态显示不准确 | 固定码无状态回读，属预期行为 |

## 备选：不装 HA 也能语音

如果不想折腾 HA，还有两个更轻的思路：

1. **Blinker App 直接点**（已实测可用）
   固件已修好 `btn-light1/2/3` 映射，手机点一下就能开关。
2. **IFTTT / 其它云中转**
   用 webhook 调 `http://192.168.71.4/send?slot=1`（需内网穿透）。

但要恢复**天猫精灵语音控制**，HassLife + HA 这条路是目前最可靠的。
