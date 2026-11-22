package dune.ins;

import com.google.common.eventbus.Subscribe;
import java.awt.BorderLayout;
import java.awt.Color;
import java.awt.Dimension;
import java.awt.Font;
import java.awt.GridBagConstraints;
import java.awt.GridBagLayout;
import java.awt.GridLayout;
import java.awt.Insets;
import java.time.LocalTime;
import java.time.format.DateTimeFormatter;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.Locale;
import java.util.Map;
import javax.swing.BorderFactory;
import javax.swing.Box;
import javax.swing.BoxLayout;
import javax.swing.JLabel;
import javax.swing.JPanel;
import javax.swing.JScrollPane;
import javax.swing.JToggleButton;
import javax.swing.JTextArea;
import javax.swing.Timer;
import pt.lsts.imc.DevDataText;
import pt.lsts.imc.Depth;
import pt.lsts.imc.IMCMessage;
import pt.lsts.neptus.comm.manager.imc.ImcMsgManager;
import pt.lsts.neptus.comm.manager.imc.MonitorIMCComms;
import pt.lsts.neptus.console.ConsoleLayout;
import pt.lsts.neptus.console.ConsolePanel;

/** Sensor simulation controls and readings alongside the standard IMC monitor tabs. */
public final class InsMonitorPanel extends ConsolePanel {
    private static final int TEST_SYSTEM = 0xb0d3;
    private static final String TITLE = "惯导与 DVL";
    private static final DateTimeFormatter CLOCK = DateTimeFormatter.ofPattern("HH:mm:ss.SSS");
    private volatile Sample latest;
    private Monitor monitor;
    private volatile SensorSimPanel simulation;
    private Timer refresh;
    private final Map<String, JLabel> values = new LinkedHashMap<>();
    private final JLabel connection = new JLabel();
    private final JLabel lastReceived = new JLabel("—");
    private final JTextArea raw = new JTextArea(4, 30);
    private Sample rendered;
    private JScrollPane rawPane;

    public InsMonitorPanel(ConsoleLayout console) {
        super(console);
    }

    @Override public void initSubPanel() {
        setLayout(new BorderLayout());
        simulation = new SensorSimPanel();
        monitor = new Monitor(getConsole().getImcMsgManager(), buildView(), simulation);
        add(monitor, BorderLayout.CENTER);
        refresh = new Timer(250, event -> render());
        refresh.start();
        render();
    }

    @Override public void cleanSubPanel() {
        if (refresh != null) refresh.stop();
        if (simulation != null) simulation.close();
        if (monitor != null) monitor.cleanup();
    }

    @Subscribe public void receive(IMCMessage message) {
        SensorSimPanel panel = simulation;
        if (panel != null && message.getSrc() == 0xb0d2 && message.getMgid() == Depth.ID_STATIC) {
            panel.acceptDepth(message.getDouble("value"));
            return;
        }
        if (message.getSrc() != TEST_SYSTEM || message.getMgid() != DevDataText.ID_STATIC) return;
        String value = message.getString("value");
        if (value != null && value.startsWith("MFOSN ")) {
            Sample sample = new Sample(value);
            latest = sample;
            if (panel != null) panel.acceptIns(sample);
        }
    }

    private static final class Monitor extends MonitorIMCComms {
        private final JPanel readingView;

        Monitor(ImcMsgManager manager, JPanel view, SensorSimPanel simulation) {
            super(manager);
            readingView = view;
            getMonitorTabHolder().insertTab(TITLE, null, view, "查看惯导记录与 DVL 测量", 0);
            getMonitorTabHolder().insertTab("传感器模拟", null, simulation, "启停板端传感器模拟", 0);
            getMonitorTabHolder().setSelectedComponent(simulation);
        }

        boolean isReadingViewSelected() {
            return getMonitorTabHolder().getSelectedComponent() == readingView;
        }
    }

    static final class Sample {
        final long received = System.nanoTime();
        final String time = LocalTime.now().format(CLOCK);
        final Map<String, String> fields = new HashMap<>();
        final String text;

        Sample(String text) {
            this.text = text;
            for (String token : text.split("\\s+")) {
                int equal = token.indexOf('=');
                if (equal > 0 && equal < token.length() - 1)
                    fields.put(token.substring(0, equal), token.substring(equal + 1));
            }
        }

        String number(String key, int precision, String unit) {
            try {
                double value = Double.parseDouble(fields.getOrDefault(key, ""));
                return Double.isFinite(value) ? String.format(Locale.ROOT, "%." + precision + "f%s", value, unit) : "—";
            } catch (NumberFormatException ignored) {
                return "—";
            }
        }

        String flag(String key, String on, String off) {
            String value = fields.get(key);
            return "1".equals(value) ? on : "0".equals(value) ? off : "—";
        }
    }

    private JPanel buildView() {
        JPanel view = new JPanel(new BorderLayout());
        JPanel content = new JPanel();
        content.setLayout(new BoxLayout(content, BoxLayout.Y_AXIS));
        content.setBorder(BorderFactory.createEmptyBorder(15, 18, 15, 18));
        JPanel header = new JPanel(new BorderLayout(12, 4));
        JLabel name = new JLabel("惯导与 DVL  /  模拟数据");
        name.setFont(name.getFont().deriveFont(Font.BOLD, 17f));
        header.add(name, BorderLayout.NORTH);
        header.add(new JLabel("rk3562-ins-dvl-test  ·  IMC B0:D3"), BorderLayout.CENTER);
        header.add(connection, BorderLayout.EAST);
        content.add(header);
        content.add(Box.createVerticalStrut(12));
        JPanel rows = new JPanel(new GridLayout(2, 2, 12, 10));
        rows.add(group("惯导位置",
            "纬度", "lat_deg", "经度", "lon_deg", "高度", "elevation_m"));
        rows.add(group("接收与有效性",
            "电脑最近收到", "last_received", "惯导状态", "nav",
            "DVL 参与导航", "dvl_fused", "DVL 测量", "dvl_valid", "DVL 数据年龄", "dvl_age_ms"));
        rows.add(group("惯导速度 · 地理坐标",
            "向北", "n_mps", "向东", "e_mps", "向下", "d_mps"));
        rows.add(group("DVL 测量 · 设备坐标",
            "向前（正）", "dvl_forward_mps", "向右（正）", "dvl_right_mps",
            "向上（正）", "dvl_up_mps", "离底距离", "bottom_m"));
        content.add(rows);
        content.add(Box.createVerticalStrut(10));
        content.add(group("帧诊断", "惯导计数", "ticks", "帧内时间", "utc_ms",
            "串口校验失败累计", "bad_checksums"));
        values.get("bad_checksums").setToolTipText("本测试的模拟器会注入校验失败的帧。");
        values.get("utc_ms").setToolTipText("惯导帧中的时间字段；模拟时不代表电脑时钟。");
        JPanel controls = new JPanel(new BorderLayout());
        controls.setBorder(BorderFactory.createEmptyBorder(8, 0, 4, 0));
        JToggleButton rawToggle = new JToggleButton("原始 IMC 文本");
        controls.add(rawToggle, BorderLayout.WEST);
        content.add(controls);
        raw.setEditable(false);
        raw.setLineWrap(true);
        raw.setWrapStyleWord(true);
        rawPane = new JScrollPane(raw);
        rawPane.setVisible(false);
        content.add(rawPane);
        rawToggle.addActionListener(event -> {
            rawPane.setVisible(rawToggle.isSelected());
            render();
            content.revalidate();
        });
        JPanel surface = new JPanel(new BorderLayout());
        surface.add(content, BorderLayout.NORTH);
        JScrollPane scroller = new JScrollPane(surface);
        scroller.setBorder(BorderFactory.createEmptyBorder());
        scroller.getVerticalScrollBar().setUnitIncrement(16);
        view.add(scroller, BorderLayout.CENTER);
        return view;
    }

    private JPanel group(String title, String... labels) {
        JPanel panel = new JPanel(new GridBagLayout());
        panel.setBorder(BorderFactory.createTitledBorder(title));
        GridBagConstraints c = new GridBagConstraints();
        c.insets = new Insets(3, 10, 3, 10);
        c.anchor = GridBagConstraints.WEST;
        for (int i = 0; i < labels.length; i += 2) {
            String key = labels[i + 1];
            c.gridy = i / 2;
            c.gridx = 0;
            c.weightx = 1;
            panel.add(new JLabel(labels[i]), c);
            c.gridx = 1;
            c.weightx = 0;
            JLabel value = "last_received".equals(key) ? lastReceived : new JLabel("—");
            value.setFont(value.getFont().deriveFont(Font.BOLD));
            value.setPreferredSize(new Dimension(148, value.getPreferredSize().height));
            value.setToolTipText("last_received".equals(key) ? "电脑接收时间" : key);
            values.put(key, value);
            panel.add(value, c);
        }
        c.gridy++;
        c.gridx = 0;
        c.weighty = 1;
        panel.add(Box.createVerticalGlue(), c);
        return panel;
    }

    private void render() {
        if (monitor == null || !isShowing() || !monitor.isReadingViewSelected()) return;
        Sample sample = latest;
        boolean fresh = sample != null && System.nanoTime() - sample.received < 2_000_000_000L;
        connection.setText(sample == null ? "等待惯导数据" : fresh ? "数据更新中" : "数据未更新（超过 2 秒）");
        connection.setForeground(fresh ? new Color(28, 105, 61) : new Color(132, 69, 6));
        if (sample != null && sample != rendered) {
            rendered = sample;
            lastReceived.setText(sample.time);
            for (String key : new String[]{"lat_deg", "lon_deg"}) set(key, sample.number(key, 8, "°"));
            for (String key : new String[]{"elevation_m", "bottom_m"}) set(key, sample.number(key, 2, " m"));
            for (String key : new String[]{"n_mps", "e_mps", "d_mps", "dvl_forward_mps",
                    "dvl_right_mps", "dvl_up_mps"}) set(key, sample.number(key, 3, " m/s"));
            set("dvl_age_ms", sample.number("dvl_age_ms", 0, " ms"));
            set("utc_ms", sample.number("utc_ms", 0, " ms"));
            set("ticks", sample.number("ticks", 0, ""));
            set("bad_checksums", sample.number("bad_checksums", 0, ""));
            set("nav", sample.flag("nav", "导航中", "未进入导航"));
            set("dvl_fused", sample.flag("dvl_fused", "正在融合", "未融合"));
            set("dvl_valid", sample.flag("dvl_valid", "有效", "无效"));
        }
        if (sample != null && rawPane.isVisible() && !raw.getText().equals(sample.text)) {
            raw.setText(sample.text);
            raw.setCaretPosition(0);
        }
        connection.setToolTipText(fresh ? null : "已有读数时，页面保留最后一次接收的值。");
        if (sample != null && !fresh) {
            set("dvl_valid", "已过期");
        } else if (sample != null) {
            set("dvl_valid", sample.flag("dvl_valid", "有效", "无效"));
        }
    }

    private void set(String key, String text) {
        values.get(key).setText(text);
    }
}
