# Home Assistant 通过 REST 控制 ESP8266

## ⚠️ HA 2025.x 重要变更（实测踩坑）

**`command_line` 平台已不支持在 YAML 里直接配置。**

源码 `homeassistant/components/command_line/switch.py`：
```python
async def async_setup_platform(hass, config, async_add_entities, discovery_info=None):
    if not discovery_info:
        return          # 没有 discovery_info 直接返回，什么都不做
```
且整个 command_line 组件**没有 `PLATFORM_SCHEMA`**。

所以写：
```yaml
switch:
  - platform: command_line     # ← 2025.11 下完全无效
    switches:
      xxx: ...
```
会看到「`Setting up command_line.switch` / `took 0.06s`」但**一个实体都不建**，
极易误判为配置写错或启动卡死。

---

## 正确写法（2025.11 实测通过）

`shell_command` + `template switch` + `rest sensor`：

```yaml
shell_command:
  ceiling_on: "curl -s 'http://192.168.71.4/send?slot=1&state=on' > /dev/null; echo 1"
  ceiling_off: "curl -s 'http://192.168.71.4/send?slot=1&state=off' > /dev/null; echo 0"

switch:
  - platform: template
    switches:
      living_room_ceiling:
        friendly_name: "客厅吸顶灯"
        value_template: "{{ states('sensor.esp8266_ceiling_state') == 'on' }}"
        turn_on:
          - service: shell_command.ceiling_on
        turn_off:
          - service: shell_command.ceiling_off

sensor:
  - platform: rest
    name: "ESP8266 吸顶灯状态"
    unique_id: esp8266_ceiling_state
    resource: "http://esp8266-433.local/rf-state"
    value_template: "{{ 'on' if value_json.slot1 else 'off' }}"
    scan_interval: 10
```

> ⚠️ `rest` 的 `resource` 在容器内要用 **IP**（容器解析不了 `.local`），
> 我这里写的域名适用于 HA 直接装在系统的情况。
>
> ⚠️ 实测 `scan_interval` 未自动生效（`last_updated` 停在创建时刻），
> 需调 `homeassistant.update_entity` 手动刷新。**不影响开关控制**，
> 只是 UI 状态显示可能滞后。

依据：`template/switch.py` 第 67/81 行
`CONF_TURN_ON: cv.SCRIPT_SCHEMA`，支持写 service 调用。

## 验证过的行为

| 操作 | 结果 |
|---|---|
| `switch.turn_on` | `RF TX source=HTTP slot=1 value=14513344 repeat=8` ✅ |
| `switch.turn_off` | `RF TX source=HTTP slot=1 value=14513344 repeat=8` ✅ |

实体：
- `switch.living_room_ceiling`（客厅吸顶灯）
- `sensor.esp8266_ceiling_state`（ESP8266 吸顶灯状态）

---

## 老版本 HA（≤2024）可用写法

如果你以后回退到旧版 HA，`command_line` 方案是这样：

```yaml
switch:
  - platform: command_line
    switches:
      living_room_ceiling:
        friendly_name: "客厅吸顶灯"
        command_on: "curl -s 'http://esp8266-433.local/send?slot=1&state=on' && echo 1"
        command_off: "curl -s 'http://esp8266-433.local/send?slot=1&state=off' && echo 0"
        command_state: "curl -s 'http://esp8266-433.local/rf-state' | grep -o '\"slot1\":true' && echo 1 || echo 0"
        value_template: "{{ value == '1' }}"
```

---

## 排查 HA 配置问题的有效手段

在配置里加 logger，能看到组件加载全过程：

```yaml
logger:
  default: info
  logs:
    homeassistant.core: debug
    homeassistant.config: debug
    homeassistant.setup: debug
    homeassistant.components.switch: debug
```

重启后日志里会有：
```
INFO  [homeassistant.setup] Setting up switch
INFO  [homeassistant.components.switch] Setting up command_line.switch
DEBUG [homeassistant.setup] Phase platform_setup ... took 0.06s
```

**关键判读**：
- `took 0.06s` 秒退 + 零实体 → **配置未被接受**（不是启动卡住）
- 反复出现 `Something is blocking Home Assistant from wrapping up the start up phase`
  → 有集成在等网络，看 `The system is waiting for tasks:` 后面的协程

## 设备地址


本固件提供两个接口，满足 HA 实体控制 + 状态回读：

| 接口 | 方法 | 说明 |
| --- | --- | --- |
| `/send?slot=N&state=on\|off` | GET | 发射第 N 个 433 码位，并记录状态 |
| `/rf-state` | GET | 返回 JSON，`{"ok":true,"slot1":true,...}` |

设备地址：`http://esp8266-433.local`（mDNS 主机名，**推荐**）

> 固件已内置 mDNS，路由器重新分配 IP 也不会影响配置。
> 若你的环境解析不了 `.local`（部分 Linux/容器需装 `avahi-daemon`/`libnss-mdns`），
> 可退回用 IP：`http://esp8266-433.local`。
>
> 之前给出的 `192.168.71.4` 是 DHCP 动态分配的，**不要依赖它**——
> 上海电信光猫无法从内网访问管理页（ICMP 通但 TCP 80/443 全部超时），
> 也无法设置静态绑定，所以 mDNS 是这里唯一可靠的做法。

> `state` 参数可选。不带则按 **toggle** 处理（翻转当前请求状态）；
> 带 `state=on` / `state=off` 则显式指定。
>
> 433 固定码本身是 toggle 语义——开和关发的是同一个码，设备**无法回读灯具真实状态**。
> `/rf-state` 返回的是「最后一次请求的状态」，用于 UI 显示，不代表灯的实际亮灭。
> 需要真实状态回读，必须改用继电器控制市电。

实测验证：

```
初始:      {"slot1":false,"slot2":false,...}
发 on:     {"slot1":true,...}
发 off:    {"slot1":false,...}
slot2 on:  {"slot1":false,"slot2":true,...}
```

## 方案一：rest sensor + template switch

编辑 `configuration.yaml`：

```yaml
rest_command:
  rf_slot_1_on:
    url: "http://esp8266-433.local/send?slot=1&state=on"
    method: "get"
    timeout: 10
  rf_slot_1_off:
    url: "http://esp8266-433.local/send?slot=1&state=off"
    method: "get"
    timeout: 10

sensor:
  - platform: rest
    name: "ESP8266 吸顶灯状态"
    unique_id: esp8266_rf_slot1
    resource: "http://esp8266-433.local/rf-state"
    value_template: "{{ 'on' if value_json.slot1 else 'off' }}"
    json_attributes: []
    scan_interval: 30

switch:
  - platform: template
    switches:
      living_room_ceiling:
        friendly_name: "客厅吸顶灯"
        value_template: "{{ states('sensor.esp8266_rf_slot1') == 'on' }}"
        turn_on:
          - service: rest_command.rf_slot_1_on
        turn_off:
          - service: rest_command.rf_slot_1_off
```

状态变化有最长 30 秒延迟（受 `scan_interval` 限制），但控制是即时的。
想更快可把 `scan_interval` 调到 5 秒。

## 方案二：command_line switch（推荐）

不用 sensor 联动，直接用 shell 调 curl。`command_state` 命令返回一个
大于 0 的值就视为开，0 视为关：

```yaml
switch:
  - platform: command_line
    switches:
      living_room_ceiling:
        friendly_name: "客厅吸顶灯"
        command_on: "curl -s 'http://esp8266-433.local/send?slot=1&state=on' && echo 1"
        command_off: "curl -s 'http://esp8266-433.local/send?slot=1&state=off' && echo 0"
        # 固定码无状态回读，乐观假设：刚发出的指令已生效
        command_state: "echo 1"
        value_template: "{{ value == '1' }}"
```

> `command_state` 每 30 秒执行一次（`scan_interval`），`echo 1` 表示
> **永远报告为开**。想要更准的状态就把这里改成读 `/rf-state`：
> ```yaml
>         command_state: >
>           curl -s 'http://esp8266-433.local/rf-state' | grep -o '"slot1":true' && echo 1 || echo 0
>         value_template: "{{ value == '1' }}"
> ```
> 但注意：HA 重启后会重新查询，此时状态以 ESP 内存中记录的为准
> （ESP 断电重启后状态会重置为全 off）。

## 方案三：只用 rest_command（最省事，够用）

如果只是要「语音能开关」，不追求 UI 状态准确：

```yaml
rest_command:
  ceiling_on:
    url: "http://esp8266-433.local/send?slot=1&state=on"
    method: "get"
  ceiling_off:
    url: "http://esp8266-433.local/send?slot=1&state=off"
    method: "get"
```

配合自动化或 HassLife 用。

## 多个灯（slot 2/3）

照抄即可，slot 换数字、名字换掉：

```yaml
rest_command:
  bedroom_ceiling_on:
    url: "http://esp8266-433.local/send?slot=2&state=on"
    method: "get"
  bedroom_ceiling_off:
    url: "http://esp8266-433.local/send?slot=2&state=off"
    method: "get"
```

## 关键：重启与验证

改完 `configuration.yaml` 后必须：

1. **检查配置语法**（HA 配置文件在 `config/configuration.yaml`）
2. **重启 HA**（或用 `HA → 开发者工具 → YAML → 检查配置` 验证）
3. **在开发者工具里测**：
   - 动作：`rest_command.ceiling_on` → 应看到灯亮
   - 服务：`switch.turn_on` 选 `living_room_ceiling` → 应看到灯亮

## 容器网络注意事项

如果 HA 跑在 Docker 里，**容器内的 `127.0.0.1` 指向容器自己**，不是宿主机。

- **推荐**：用 `network_mode: host`（Linux）
- 或用 `macvlan` 给容器分配局域网 IP
- 或直接用宿主机局域网 IP（`192.168.71.4` 这种），多数情况可直接访问

验证：进容器 `ping esp8266-433.local` 或 `curl http://esp8266-433.local/rf-state`，通了才能用。

> **Docker 容器里解析 `.local` 常见失败**，因为容器没有 avahi/mDNS 支持。
> 解决办法（任选）：
> - `docker-compose.yml` 加 `extra_hosts` 或直接用宿主 IP 兜底
> - 容器内装 `libnss-mdns` 并在启动脚本里跑 `nss-mdns`
> - 宿主机 `/etc/hosts` 加一行 `192.168.71.4 esp8266-433.local`，并给容器
>   配 `--add-host esp8266-433.local:<宿主机IP>`
>
> 实在解析不了就退回用 IP，只是失去「IP 变了不用改配置」的好处。

## 接入 HassLife

HA 实体建好后：

1. 注册登录 https://hass.blear.cn
2. HA → 配置 → 集成 → 添加集成 → 搜 **HassLife** → 填账号密码
3. HassLife 网页端确认插件状态**在线**
4. 设备管理 → 添加 Homeassistant 设备 → 勾选 `客厅吸顶灯`
5. 点**配置**，平台选**天猫精灵**，设备类型选**灯**，填语控名称
6. 天猫精灵 App → 精灵技能 → 搜 **HassLife** → 绑定账号
7. 精灵家里就能看到，说「打开客厅吸顶灯」

**设备类型必须选「灯」**，否则 HassLife 会按错误类型渲染控制面板。

## 排障

| 现象 | 原因与处理 |
| --- | --- |
| 灯不亮，HA 无报错 | 浏览器直接开 `http://esp8266-433.local/send?slot=1` 测，能亮就是 HA 网络问题 |
| HA 报 404 | **正常现象**，`/send` 执行后 302 重定向到 `/#rf`，以灯是否亮为准 |
| 容器内访问不了 | 容器网络隔离，改 `network_mode: host` 或用 macvlan |
| YAML 报错 | 用开发者工具的「检查配置」定位行号 |
| 实体状态乱跳 | 固定码无真实状态回读，属预期；需要准确状态请改继电器方案 |
| 设备 IP 变了 | ESP 用 DHCP，换 IP 后要同步改 HA 配置；建议路由器上做 IP 绑定 |

## 建议：固定设备 IP

ESP8266 每次重启可能拿到不同 IP，建议在路由器里给它做 **DHCP 静态绑定**，
否则 HA 配置要跟着改 IP。
