package dune.ins;

import java.awt.BorderLayout;
import java.awt.Component;
import java.awt.FlowLayout;
import java.awt.GridLayout;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.UUID;
import java.util.concurrent.TimeUnit;
import javax.swing.*;

public final class AlignmentPanel extends JPanel {
    private static final String HOST = "169.254.66.50";
    private final JTextField latitude = new JTextField(18), longitude = new JTextField(18);
    private final JLabel actual = new JLabel("等待板端回读");
    private final JLabel reference = new JLabel("未提交");
    private final JLabel outcome = new JLabel("正在检查板端");
    private final JButton submit = new JButton("核对并发送…");
    private final Timer timer = new Timer(1500, e -> pollStatus());
    private String target;
    private final java.util.function.BooleanSupplier fresh;
    private final JButton toggle = new JButton("校准坐标…");
    private boolean expanded, ready;
    private int generation;
    private JPanel content;
    private String board() { return InsMonitorPanel.targetName(target) + " · " + HOST; }
    void target(String value) {
        generation++; target=value; ready=false; session=null; request=null; working=false; awaiting=false;
        latitude.setText(""); longitude.setText(""); actual.setText("等待板端回读");
        reference.setText("未提交"); outcome.setText("等待核对目标"); submit.setEnabled(false);
        if (expanded) pollStatus();
    }
    void updateEnabled() { submit.setEnabled(expanded && ready && fresh.getAsBoolean() && !working && !awaiting); }
    void close() { timer.stop(); generation++; }
    void open() { if (!expanded) toggle.doClick(); }
    private boolean working;
    private String session, request;
    private boolean awaiting;
    private long until;

    AlignmentPanel(java.util.function.BooleanSupplier fresh, String initial) {
        this.fresh=fresh; this.target=initial;
        setLayout(new BorderLayout()); add(toggle, BorderLayout.NORTH);
        JPanel form = new JPanel(new GridLayout(0, 2, 12, 10));
            row(form, "参考纬度（°）", latitude);
            row(form, "参考经度（°）", longitude);
            row(form, "本次参考位置", reference);
            row(form, "惯导实际位置", actual);
            row(form, "结果", outcome);
            JPanel buttons = new JPanel(new FlowLayout(FlowLayout.RIGHT));


            submit.setEnabled(false);
            submit.addActionListener(e -> confirm());

            buttons.add(submit);
            content = new JPanel(new BorderLayout(0, 12));
            content.setBorder(BorderFactory.createEmptyBorder(16, 16, 12, 16));
            content.add(form, BorderLayout.CENTER);
            content.add(buttons, BorderLayout.SOUTH);
        add(content,BorderLayout.CENTER); content.setVisible(false);
        toggle.addActionListener(e -> {
            expanded=!expanded; content.setVisible(expanded);
            toggle.setText(expanded ? "收起坐标校准" : "校准坐标…");
            revalidate();
            if (expanded) { ready=false; updateEnabled(); timer.start(); pollStatus(); }
            else if (!awaiting) timer.stop();
        });
    }

    private static void row(JPanel form, String caption, Component field) {
        JLabel label = new JLabel(caption);
        label.setLabelFor(field);
        form.add(label);
        form.add(field);
    }

    private static double decimal(JTextField field, double max) {
        String s = field.getText().trim();
        if (s.length() > 24 || !s.matches("[+-]?(?:[0-9]+(?:\\.[0-9]*)?|\\.[0-9]+)"))
            throw new IllegalArgumentException();
        double value = Double.parseDouble(s);
        if (!Double.isFinite(value) || Math.abs(value) > max) throw new IllegalArgumentException();
        return value;
    }

    private static String coordinates(double lat, double lon) {
        return String.format(Locale.ROOT, "%.8f°, %.8f°", lat, lon);
    }

    private void confirm() {
        if (working || awaiting || !submit.isEnabled() || session == null) return;
        final double lat, lon;
        try {
            lat = decimal(latitude, 90);
            lon = decimal(longitude, 180);
        } catch (IllegalArgumentException e) {
            JOptionPane.showMessageDialog(this, "请输入十进制度：纬度 −90 至 90，经度 −180 至 180。",
                    "坐标无效", JOptionPane.WARNING_MESSAGE);
            return;
        }
        // Suppress refresh while the modal confirmation is open.
        String verifiedSession = session;
        if (!fresh.getAsBoolean()) return;
        timer.stop();
        Object[] choices = {"取消", target.equals("ins") ? "已确认实物静止，发送一次" : "确认发送一次"};
        int answer = JOptionPane.showOptionDialog(this,
                "目标板：" + board() + "\n参考位置：" + coordinates(lat, lon) +
                "\n请确认处于台架或停航状态。",
                "核对校准请求", JOptionPane.DEFAULT_OPTION, JOptionPane.WARNING_MESSAGE,
                null, choices, choices[0]);
        timer.start();
        if (answer != 1 || !fresh.getAsBoolean()) return;
        request = verifiedSession + ":" + UUID.randomUUID().toString().replace("-", "");
        reference.setText(coordinates(lat, lon));
        outcome.setText("正在提交请求");
        outcome.setToolTipText(request);
        awaiting = true;
        until = System.nanoTime() + TimeUnit.MINUTES.toNanos(6) + TimeUnit.SECONDS.toNanos(10);
        exchange("send " + request + " " + lat + " " + lon, true);
    }

    private void pollStatus() {
        if (!working && (expanded || awaiting)) {
            exchange(awaiting ? "status " + request : "status", false);
        } else if (!expanded && !awaiting) {
            timer.stop();
        }
    }

    private void exchange(String command, boolean sent) {
        final int token = generation;
        final String instance=target;
        working = true;
        if (sent) submit.setEnabled(false);
        new SwingWorker<String[], Void>() {
            @Override protected String[] doInBackground() throws Exception {
                Process process = new ProcessBuilder("ssh.exe", "-T", "-o", "BatchMode=yes",
                        "-o", "ConnectTimeout=4", "-o", "StrictHostKeyChecking=yes", "root@" + HOST,
                        (instance.equals("local") ? "python3 /opt/dune-ins-dvl/scripts/target_alignment.py " :
                        "python3 /opt/rk3562-dune-sensors/target_alignment.py ") + instance)
                        .redirectError(ProcessBuilder.Redirect.DISCARD).start();
                try {
                    try (var stdin = process.getOutputStream()) {
                        stdin.write((command + "\n").getBytes(StandardCharsets.US_ASCII));
                    }
                    if (!process.waitFor(7, TimeUnit.SECONDS) || process.exitValue() != 0)
                        throw new IOException("Board communication failed");
                    String[] fields = new String(process.getInputStream().readNBytes(257),
                            StandardCharsets.US_ASCII).trim().split(" ");
                    if (fields.length == 1 && fields[0].matches("disabled|identity_mismatch|offline")) return fields;
                    if (fields.length != 6 || !fields[1].matches("[a-f0-9-]{36}"))
                        throw new IOException("Invalid board response");
                    return fields;
                } finally {
                    if (process.isAlive()) process.destroyForcibly();
                }
            }

            @Override protected void done() {
                if (token != generation) return;
                working = false;
                try {
                    String[] r = get();
                    if (r.length == 1) {
                        ready=false; session=null; submit.setEnabled(false);
                        actual.setText("无近期导航位置");
                        if (awaiting && System.nanoTime() >= until) {
                            awaiting=false; outcome.setText("未确认生效：板端回读超时");
                        } else {
                            outcome.setText(awaiting ? "未确认生效：" + describe(r[0]) : describe(r[0]));
                        }
                        return;
                    }
                    session = r[1];
                    actual.setText(r[2].equals("1")
                            ? coordinates(Double.parseDouble(r[3]), Double.parseDouble(r[4]))
                            : "无近期导航位置");
                    if (awaiting) {
                        String message = describe(r[0]);
                        awaiting = r[0].equals("waiting") || r[0].equals("writing");
                        outcome.setText(message + (r[5].equals("1") && !r[0].equals("already_near")
                                ? "（发送前已接近目标）" : ""));
                        if (!awaiting) submit.setEnabled(false);
                    } else {
                        ready = r[0].equals("ready");
                        updateEnabled();
                        if (request == null || !ready) outcome.setText(ready ? (fresh.getAsBoolean() ? "可发送" : "等待此目标的新 IMC 数据") : describe(r[0]));
                    }
                } catch (Exception failure) {
                    session = null; ready=false;
                    actual.setText("无法读取板端位置");
                    submit.setEnabled(false);
                    if (awaiting && System.nanoTime() < until) {
                        outcome.setText("等待恢复板端查询，发送状态待核对");
                    } else if (awaiting) {
                        awaiting = false;
                        outcome.setText("未确认生效：板端查询超时");
                    } else if (request == null) {
                        outcome.setText("板端服务不可用");
                    }
                }
            }
        }.execute();
    }

    private static String describe(String state) {
        switch (state) {
            case "disabled": return "实物写入未启用（只读采集）";
            case "identity_mismatch": return "设备身份或串口不匹配";
            case "offline": return "目标服务未运行";
            case "writing": return "正在写入串口";
            case "waiting": return "已提交串口，等待惯导回读";
            case "position_matches": return "惯导回读与目标一致";
            case "already_near": return "发送前已接近目标；惯导回读与目标一致";
            case "position_unchanged": return "未确认生效：惯导位置未变化";
            case "readback_timeout": return "未确认生效：惯导回读超时";
            case "not_confirmed": return "未确认生效：回读位置未持续接近目标";
            case "unknown_request": return "未确认生效：板端无此请求记录";
            case "write_timeout": return "串口写入超时";
            case "write_failed": return "串口写入失败";
            case "serial_disconnected": return "写入失败：串口断开";
            case "serial_unavailable": return "串口不可用或近期无合法惯导帧";
            case "busy": return "板端正在处理另一请求";
            case "session_limit": return "板端已达本次会话请求上限";
            default: return "板端拒绝请求";
        }
    }
}
