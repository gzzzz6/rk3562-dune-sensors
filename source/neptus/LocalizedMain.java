package pt.lsts.neptus.i18n;

import java.awt.AWTEvent;
import java.awt.Component;
import java.awt.Container;
import java.awt.Font;
import java.awt.Toolkit;
import java.awt.event.WindowEvent;
import java.io.File;
import java.io.IOException;
import javax.swing.AbstractButton;
import javax.swing.JLabel;
import javax.swing.UIManager;
import pt.lsts.neptus.console.ConsoleLayout;
import pt.lsts.neptus.loader.NeptusMain;
import pt.lsts.neptus.util.conf.ConfigFetch;

/** Loads the trial catalog for this process without changing shared Neptus preferences. */
public final class LocalizedMain {
    private LocalizedMain() {}

    public static void main(String[] args) throws IOException {
        String catalog = System.getProperty("dune.neptus.catalog");
        if (catalog == null) throw new IllegalArgumentException("Missing dune.neptus.catalog");
        ConfigFetch.initialize();
        PoFile translations = new PoFile();
        translations.load(new File(catalog));
        I18n.getInstance().po = translations;
        UIManager.put("OptionPane.yesButtonText", I18n.text("Yes"));
        UIManager.put("OptionPane.noButtonText", I18n.text("No"));
        UIManager.put("OptionPane.okButtonText", I18n.text("OK"));
        UIManager.put("OptionPane.cancelButtonText", I18n.text("Cancel"));
        Toolkit.getDefaultToolkit().addAWTEventListener(event -> {
            WindowEvent window = (WindowEvent) event;
            if (window.getID() == WindowEvent.WINDOW_OPENED && window.getWindow() instanceof ConsoleLayout)
                localizeStatusBarFonts(((ConsoleLayout) window.getWindow()).getStatusBar());
        }, AWTEvent.WINDOW_EVENT_MASK);
        NeptusMain.main(args);
    }

    private static void localizeStatusBarFonts(Component component) {
        if (component instanceof JLabel || component instanceof AbstractButton) {
            // StatusBar hardcodes Arial and reapplies it when the notification count changes.
            component.addPropertyChangeListener("font", e -> applyFallback(component));
            applyFallback(component);
        }
        if (component instanceof Container)
            for (Component child : ((Container) component).getComponents())
                localizeStatusBarFonts(child);
    }

    private static void applyFallback(Component component) {
        Font font = component.getFont();
        if (font != null && !font.canDisplay('\u901a'))
            component.setFont(new Font(Font.DIALOG, font.getStyle(), font.getSize()));
    }
}
