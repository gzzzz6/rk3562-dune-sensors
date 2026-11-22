# RK3562 · DUNE 传感器台架联调

本仓库用于采集 mFOSN 惯导（含经惯导转发的 DVL）和 KELLER 33X 深度计数据，通过 IMC 在 Windows Neptus 查看，并在板端记录 LSF。首版交付用于实物联调：驱动已通过虚拟串口验证，真实传感器还要现场验收。此套件只运行采集、显示和日志任务。

## 拿到包后先做什么

1. 在 GitHub 用 **Code → Download ZIP** 下载，或用 Git 克隆。Windows 保留一份；把解压目录复制到板子。目录里已经有两套 ARM64 程序，不需要先编译。
2. 板端已知环境：RK3562、Tronlong-Desktop 20.04、`aarch64`、systemd、Python 3.8。检查至少有 1 GiB 空间。其他镜像先在其上跑完模拟自检。
3. 在板子上，切换到仓库目录后安装。安装不更改板端 IP、SSH、现有 DUNE 监控或开机启动项。

   ```sh
   uname -m
   sudo python3 board/kit.py install
   sudoedit /etc/rk3562-dune-sensors.json
   ```

   把 `pc_ipv4` 填为 Windows **实际用于和板子通信**的 IPv4 地址。Windows 的 Neptus IMC UDP 接收端口应与 `neptus_udp_port` 一致（默认 **16003**）；若防火墙拦截，给 Neptus 或该 UDP 端口添加入站规则，来源限制为板端 IP。固定网线直连可使用同一网段内两个不同的静态地址。
4. 电脑运行 [Neptus 查看器](#windows-查看)，板端启动模拟。模拟只使用虚拟串口，可在未接传感器时运行。

   ```sh
   sudo systemctl start rk3562-sensor@ins-sim rk3562-sensor@depth-sim
   systemctl status rk3562-sensor@ins-sim rk3562-sensor@depth-sim --no-pager
   ```

   Neptus 中应出现 `rk3562-ins-dvl-sim` 和 `rk3562-keller-sim`。惯导模拟前 25 秒包含阶段性的断流和失锁，之后恢复；深度计应出现约 **10.197 m** 和 **200000 Pa**。确认消息持续刷新再停掉两路：

   ```sh
   sudo systemctl stop rk3562-sensor@ins-sim rk3562-sensor@depth-sim
   ```

## 开始实物台架测试

| 设备 | 核对与填写 | 怎么判断通过 |
|---|---|---|
| 惯导和 DVL | 按现有设备方案，DVL A100 通过 RS232 / 9600 / PD6 接惯导 COMB；工控机通过 RS232 / 115200 / 8N1 接惯导 COMA。按本板和尾线图确认针脚和实际 Linux 串口，填 `ins.serial`。 | Neptus 查 `DevDataText`（位置和状态）及 `EulerAngles`（进入导航状态后，弧度）。DVL 先保持 `confirmed_bottom_track: false`，通过诊断文本比对；在水下核对底锁、失锁、轴向、测量时效和离底距离后才打开标准 `GroundVelocity` / `Distance`。 |
| KELLER 33X | 按该实物铭牌、标定单和尾线图核对供电及线序；用确认的 RS485 半双工端口，电源不要接入 A/B。核对 `depth.serial`、`baud`、`device_address`、`local_echo`。实测水面/空气 P1（bar），填入 `surface_pressure_bar`，按测试水体及传感器量程设置密度和最大深度。 | 先用已验证的 Python 工具或厂家工具在空气中读取基准并退出，避免抢串口。再用 DUNE 观察 `Pressure`（Pa）、`Depth`（m）、`EntityState` 和更新时间；用已知水深复核数值。 |

惯导位置校准入口在本套件中未启用；深度计执行协议初始化 F48 和 F73/P1 读取，不执行硬件校零。实物串口和深度基准都要求显式填写。启动一个设备时只运行对应服务：

```sh
sudo python3 /opt/rk3562-dune-sensors/kit.py check ins
sudo systemctl start rk3562-sensor@ins
sudo python3 /opt/rk3562-dune-sensors/kit.py check depth
sudo systemctl start rk3562-sensor@depth
```

实物来源显示为 `rk3562-ins-dvl` / `rk3562-keller`。能看到系统只说明 IMC 已通，**还要核对测量消息时间持续变化**。缺帧时不要把上次显示的数字当作新数据。出现问题用 `journalctl -u rk3562-sensor@depth -n 80 --no-pager`（或把 `depth` 换成 `ins`）排查。

## Windows 查看

安装 [LSTS Neptus](https://github.com/LSTS/neptus) 和 Java 11。项目原环境使用 Neptus 提交 [`9fe8db6`](https://github.com/LSTS/neptus/commit/9fe8db6d61ced2f9163023fbedbf689e8eaa55c9)。在 PowerShell 进入仓库目录，填 Neptus 安装路径（包含 `bin/neptus.jar`）。如 Java 11 不在 PATH，用 `-JavaExe` 指定其可执行文件。

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\neptus\Start-SensorBench.ps1 -NeptusHome 'C:\Apps\neptus'
```

如果已经配置好 Neptus，也可用该应用的 IMC 通信监视器接收，无需在电脑上建立 SSH 密钥。在「状态」选择目标系统，在「设备消息」打开上述 IMC 消息。避免两套 Neptus 同时监听同一个 UDP 端口。

## 停止、取日志和卸载

```sh
sudo systemctl stop rk3562-sensor@ins rk3562-sensor@depth
sudo python3 /opt/rk3562-dune-sensors/kit.py export depth /tmp/rk3562-depth-evidence.tar.gz
sudo python3 /opt/rk3562-dune-sensors/kit.py uninstall
```

导出包包含此路 LSF 及最近 300 行服务日志；可用 Neptus MRA 打开其中的 `Data.lsf.gz`。如果要反馈问题，附传感器型号、串口/波特率、复现步骤及对应时段。日志可能包含实际经纬度，发出前先核对内容。

日志在 `/var/log/rk3562-dune-sensors`，每路 200 MiB 或磁盘剩余不足 1 GiB 时停止该服务，阈值每 5 秒检查。卸载保留日志、配置和数据库。

## 源码和构建

板端程序以 [LSTS DUNE `09da1c45`](https://github.com/LSTS/dune/commit/09da1c45aa3ba4ce94cc86cdff555bc7691b869c) 为基线，新增的驱动修改见 `source/MFOSN` 与 `source/KellerDepth`；内置程序来自原 ARM64 GCC 9.4 构建，SHA-256 见 [board/SHA256SUMS](board/SHA256SUMS)。源码可按 [构建说明](source/BUILD.md) 还原。项目新增驱动于 2026-11 修改。

DUNE 及派生驱动沿用上游 [修改版 EUPL v1.1](LICENCE.md)，适用于非商业用途。公开下载不会改变许可条件；商业用途须另向上游权利人取得书面授权。

`source/neptus` 保存原试验控制台插件源码供对照；本联调套件使用 Neptus 内置 IMC 监视器，不加载旧插件。旧直连 DVL、LAUV 仿真和安保机模块不属于本套件。
