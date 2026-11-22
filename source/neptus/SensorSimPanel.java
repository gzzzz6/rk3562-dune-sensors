package dune.ins;

import com.google.gson.JsonObject;
import com.google.gson.JsonParser;
import java.awt.BorderLayout;
import java.awt.Color;
import java.awt.Dimension;
import java.awt.FlowLayout;
import java.awt.Font;
import java.awt.GridBagConstraints;
import java.awt.GridBagLayout;
import java.awt.GridLayout;
import java.awt.Insets;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.time.LocalTime;
import java.time.format.DateTimeFormatter;
import java.util.Locale;
import java.util.concurrent.FutureTask;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import javax.swing.BorderFactory;
import javax.swing.JButton;
import javax.swing.JLabel;
import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.JTextArea;
import javax.swing.SwingWorker;
import javax.swing.Timer;

final class SensorSimPanel extends JPanel {
    private static final String HOST = "169.254.66.50";
    private static final String CONTROL = "/usr/bin/python3 /opt/dune-ins-dvl/scripts/sensor_sim_ctl.py";
    private static final long STALE = TimeUnit.SECONDS.toNanos(2);
    private static final long POLL = TimeUnit.SECONDS.toNanos(5);
    private static final Color ACTIVE = new Color(23, 104, 59);
    private static final Color CAUTION = new Color(140, 74, 6);
    private static final DateTimeFormatter CLOCK = DateTimeFormatter.ofPattern("HH:mm:ss.SSS");
    private static final String EMPTY = "—";

    private final Sensor ins = new Sensor("ins", "惯导与 DVL", "rk3562-ins-dvl-test", "纬度", "经度", "DVL 离底距离", "DVL 测量");
    private final Sensor depth = new Sensor("depth", "深度计", "rk3562-depth-test", "水深");
    private final Sensor[] sensors = {ins, depth};
    private final JButton startAll = new JButton("全部启动");
    private final JTextArea outcome = new JTextArea(3, 36);
    private final Timer timer;
    private volatile InsMonitorPanel.Sample insSample;
    private volatile DepthSample depthSample;
    private long nextPoll = System.nanoTime();
    private boolean busy;
    private boolean polling;
    private boolean closed;
    private long actionVersion;

    SensorSimPanel() {
        super(new BorderLayout());
        JPanel surface = new JPanel(new BorderLayout(0, 12));
        surface.setBorder(BorderFactory.createEmptyBorder(14, 18, 14, 18));
        JPanel header = new JPanel(new BorderLayout());
        JLabel title = new JLabel("传感器模拟  /  RK3562");
        title.setFont(title.getFont().deriveFont(Font.BOLD, 17f));
        header.add(title, BorderLayout.WEST);
        header.add(startAll, BorderLayout.EAST);
        surface.add(header, BorderLayout.NORTH);

        JPanel devices = new JPanel(new GridLayout(1, 2, 12, 0));
        devices.add(ins.view);
        devices.add(depth.view);
        surface.add(devices, BorderLayout.CENTER);
        outcome.setEditable(false);
        outcome.setLineWrap(true);
        outcome.setWrapStyleWord(true);
        outcome.setFont(title.getFont().deriveFont(Font.PLAIN, 12f));
        outcome.setText("板端连接：正在查询");
        JScrollPane result = new JScrollPane(outcome);
        result.setBorder(BorderFactory.createTitledBorder("操作结果"));
        surface.add(result, BorderLayout.SOUTH);
        JPanel top = new JPanel(new BorderLayout());
        top.add(surface, BorderLayout.NORTH);
        JScrollPane scroller = new JScrollPane(top);
        scroller.setBorder(BorderFactory.createEmptyBorder());
        scroller.getVerticalScrollBar().setUnitIncrement(16);
        add(scroller, BorderLayout.CENTER);

        startAll.addActionListener(e -> command("start", "all"));
        timer = new Timer(250, e -> tick());
        timer.start();
        render();
    }

    void acceptIns(InsMonitorPanel.Sample sample) { insSample = sample; }

    void acceptDepth(double value) {
        if (Double.isFinite(value) && value >= 0) depthSample = new DepthSample(value);
    }

    void close() {
        closed = true;
        timer.stop();
    }

    private void tick() {
        if (!isShowing()) return;
        render();
        if (!busy && !polling && System.nanoTime() - nextPoll >= 0) refreshStates();
    }

    private void render() {
        InsMonitorPanel.Sample nav = insSample;
        DepthSample water = depthSample;
        ins.render(nav == null ? 0L : nav.received, nav == null ? EMPTY : nav.time, nav == null ? null :
                new String[]{nav.number("lat_deg", 8, "°"), nav.number("lon_deg", 8, "°"),
                    nav.number("bottom_m", 2, " m"), nav.flag("dvl_valid", "有效", "无效")});
        depth.render(water == null ? 0L : water.received, water == null ? EMPTY : water.time, water == null ? null :
                new String[]{String.format(Locale.ROOT, "%.3f m", water.value)});
        for (Sensor sensor : sensors) {
            sensor.start.setEnabled(!busy && canStart(sensor));
            sensor.stop.setEnabled(!busy && "active".equals(sensor.state));
        }
        startAll.setEnabled(!busy && (canStart(ins) || canStart(depth)));
    }

    private static boolean canStart(Sensor sensor) {
        return "inactive".equals(sensor.state) || "failed".equals(sensor.state);
    }

    private void useStates(JsonObject reply) {
        JsonObject states = reply == null ? null : reply.getAsJsonObject("states");
        for (Sensor sensor : sensors) {
            String state = states == null ? "unknown" : states.get(sensor.id).getAsString();
            if ("inactive".equals(state) || "failed".equals(state) || "missing".equals(state)) {
                sensor.sampleFloor = System.nanoTime();
            }
            sensor.state = state;
        }
    }

    private void refreshStates() {
        polling = true;
        long version = actionVersion;
        new SwingWorker<JsonObject, Void>() {
            @Override protected JsonObject doInBackground() throws Exception { return ssh("status", 9); }
            @Override protected void done() {
                if (closed) return;
                try {
                    JsonObject reply = get();
                    if (version != actionVersion) return;
                    useStates(reply);
                    if (outcome.getText().startsWith("板端连接：")) showOutcome("板端连接：已连接");
                } catch (Exception error) {
                    if (version != actionVersion) return;
                    useStates(null);
                    if (outcome.getText().startsWith("板端连接：")) {
                        Throwable cause = error.getCause() == null ? error : error.getCause();
                        showOutcome("板端连接：无法查询 SSH（" + cause.getMessage() + "）");
                    }
                }
                finally {
                    polling = false;
                    nextPoll = System.nanoTime() + POLL;
                    render();
                }
            }
        }.execute();
    }

    private void command(String action, String target) {
        if (busy || closed) return;
        busy = true;
        actionVersion++;
        for (Sensor sensor : sensors) {
            if ((target.equals(sensor.id) || target.equals("all")) &&
                    !(action.equals("start") && sensor.state.equals("active"))) {
                if (action.equals("start")) sensor.sampleFloor = System.nanoTime();
                sensor.pending = action.equals("start") ? "正在启动" : "正在停止";
            }
        }
        showOutcome(action.equals("start") ? "正在启动所选模拟服务…" : "正在停止所选模拟服务…");
        render();
        new SwingWorker<CommandResult, Void>() {
            @Override protected CommandResult doInBackground() {
                try {
                    JsonObject reply = ssh(action + " " + target, target.equals("all") ? 75 : 40);
                    boolean timeouts = reply.getAsJsonObject("results").entrySet().stream().anyMatch(
                            e -> "timeout".equals(e.getValue().getAsJsonObject().get("code").getAsString()));
                    return new CommandResult(timeouts ? recheck() : reply, reply, null);
                } catch (Exception error) {
                    return new CommandResult(recheck(), null, error instanceof TimeoutException ?
                            "操作超时，已重新查询；未重发命令" : "未收到操作结果，已重新查询；未重发命令");
                }
            }
            @Override protected void done() {
                if (closed) return;
                try {
                    CommandResult response = get();
                    useStates(response.states);
                    if (response.error != null) {
                        showOutcome(response.error + (response.states == null ? "，SSH 状态未知。" : "。"));
                    } else {
                        JsonObject results = response.results.getAsJsonObject("results");
                        StringBuilder lines = new StringBuilder();
                        for (Sensor sensor : sensors) {
                            if (!results.has(sensor.id)) continue;
                            JsonObject row = results.getAsJsonObject(sensor.id);
                            String code = row.get("code").getAsString();
                            if (lines.length() > 0) lines.append('\n');
                            lines.append(sensor.name).append("：").append(resultText(action, code));
                            if (row.has("detail") && !row.get("detail").getAsString().isBlank())
                                lines.append("（").append(row.get("detail").getAsString().replace('\n', ' ')).append("）");
                        }
                        showOutcome(lines.toString());
                    }
                } catch (Exception error) {
                    useStates(null);
                    showOutcome("操作结果不可用，请查看板端服务状态。");
                } finally {
                    for (Sensor sensor : sensors) sensor.pending = null;
                    busy = false;
                    nextPoll = System.nanoTime() + POLL;
                    render();
                }
            }
        }.execute();
    }

    private void showOutcome(String text) {
        outcome.setText(text);
        outcome.setCaretPosition(0);
    }

    private static String resultText(String action, String code) {
        boolean starting = "start".equals(action);
        switch (code) {
            case "ok": return starting ? "已启动" : "已停止";
            case "already": return starting ? "已在运行" : "已处于停止状态";
            case "busy": return "板端正在处理另一项操作";
            case "timeout": return "操作超时，已重新查询；未重发命令";
            default: return starting ? "启动失败" : "停止失败";
        }
    }

    private static JsonObject recheck() {
        try { return ssh("status", 9); }
        catch (Exception error) { return null; }
    }

    private static JsonObject ssh(String operation, int seconds) throws Exception {
        String ssh = Path.of(System.getenv("SystemRoot"), "System32", "OpenSSH", "ssh.exe").toString();
        Process process = new ProcessBuilder(ssh, "-T", "-o", "BatchMode=yes",
                "-o", "ConnectTimeout=4", "-o", "StrictHostKeyChecking=yes",
                "root@" + HOST, CONTROL + " " + operation).redirectErrorStream(true).start();
        FutureTask<byte[]> output = new FutureTask<>(() -> {
            try (InputStream stream = process.getInputStream()) { return stream.readNBytes(16384); }
        });
        Thread reader = new Thread(output, "sensor-sim-ssh-output");
        reader.setDaemon(true);
        reader.start();
        try {
            process.getOutputStream().close();
            if (!process.waitFor(seconds, TimeUnit.SECONDS)) throw new TimeoutException();
            String reply = new String(output.get(2, TimeUnit.SECONDS), StandardCharsets.UTF_8).strip();
            if (process.exitValue() != 0) throw new IOException(reply.isEmpty() ? "SSH exit " + process.exitValue() : reply);
            return new JsonParser().parse(reply).getAsJsonObject();
        } finally {
            if (process.isAlive()) process.destroyForcibly();
        }
    }

    private static final class CommandResult {
        final JsonObject states, results;
        final String error;
        CommandResult(JsonObject states, JsonObject results, String error) {
            this.states = states;
            this.results = results;
            this.error = error;
        }
    }

    private static final class DepthSample {
        final double value;
        final long received = System.nanoTime();
        final String time = LocalTime.now().format(CLOCK);
        DepthSample(double value) { this.value = value; }
    }

    private final class Sensor {
        final String id, name;
        final JPanel view = new JPanel(new BorderLayout(0, 10));
        final JLabel service = new JLabel("状态未知");
        final JLabel data = new JLabel("等待数据");
        final JLabel last = new JLabel(EMPTY);
        final JButton start = new JButton("启动");
        final JButton stop = new JButton("停止");
        final JLabel[] values;
        String state = "unknown";
        String pending;
        long sampleFloor = System.nanoTime();

        Sensor(String id, String name, String system, String... labels) {
            this.id = id;
            this.name = name;
            view.setBorder(BorderFactory.createTitledBorder(name));
            JPanel top = new JPanel(new BorderLayout());
            top.setBorder(BorderFactory.createEmptyBorder(3, 10, 0, 10));
            top.add(new JLabel(system), BorderLayout.WEST);
            JPanel buttons = new JPanel(new FlowLayout(FlowLayout.RIGHT, 6, 0));
            buttons.add(start);
            buttons.add(stop);
            top.add(buttons, BorderLayout.EAST);
            view.add(top, BorderLayout.NORTH);

            JPanel readings = new JPanel(new GridBagLayout());
            row(readings, 0, "板端服务", service);
            row(readings, 1, "数据接收", data);
            row(readings, 2, "最近更新", last);
            values = new JLabel[labels.length];
            for (int i = 0; i < labels.length; i++) {
                values[i] = new JLabel(EMPTY);
                row(readings, i + 3, labels[i], values[i]);
            }
            JPanel rows = new JPanel(new BorderLayout());
            rows.add(readings, BorderLayout.NORTH);
            view.add(rows, BorderLayout.CENTER);
            start.setToolTipText("启动" + name + "模拟");
            stop.setToolTipText("停止" + name + "模拟");
            start.getAccessibleContext().setAccessibleName("启动" + name + "模拟");
            stop.getAccessibleContext().setAccessibleName("停止" + name + "模拟");
            start.addActionListener(e -> command("start", id));
            stop.addActionListener(e -> command("stop", id));
        }

        void render(long received, String time, String[] readings) {
            boolean off = "inactive".equals(state) || "failed".equals(state) || "missing".equals(state);
            boolean have = !off && readings != null && received - sampleFloor >= 0;
            boolean fresh = have && System.nanoTime() - received < STALE;
            service.setText(pending != null ? pending : serviceLabel(state));
            service.setForeground("active".equals(state) && pending == null ? ACTIVE : CAUTION);
            data.setText("正在启动".equals(pending) ? "等待数据" :
                    off ? "未接收" : !have ? "等待数据" : fresh ? "数据更新中" : "读数已过期");
            data.setForeground(fresh ? ACTIVE : CAUTION);
            last.setText(have ? time : EMPTY);
            for (int i = 0; i < values.length; i++)
                values[i].setText(have && !(id.equals("ins") && !fresh && i == 3) ? readings[i] :
                        have ? "已过期" : EMPTY);
        }
    }

    private static String serviceLabel(String state) {
        switch (state) {
            case "active": return "运行中";
            case "inactive": return "已停止";
            case "activating": return "正在启动";
            case "deactivating": return "正在停止";
            case "failed": return "服务故障";
            case "missing": return "服务未安装";
            default: return "状态未知";
        }
    }

    private static void row(JPanel panel, int index, String label, JLabel value) {
        GridBagConstraints c = new GridBagConstraints();
        c.gridy = index;
        c.insets = new Insets(4, 10, 4, 10);
        c.anchor = GridBagConstraints.WEST;
        c.weightx = 1;
        c.fill = GridBagConstraints.HORIZONTAL;
        panel.add(new JLabel(label), c);
        c.gridx = 1;
        c.weightx = 0;
        value.setFont(value.getFont().deriveFont(Font.BOLD));
        value.setPreferredSize(new Dimension(150, value.getPreferredSize().height));
        panel.add(value, c);
    }
}
