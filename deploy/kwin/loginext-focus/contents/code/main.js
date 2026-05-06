// LogiNext focus bridge — runs inside KWin's QtScript engine and forwards
// every workspace.windowActivated signal to the loginext daemon over the
// user session bus.
//
// Why this exists: starting with Plasma 6, KWin no longer exposes the
// `org_kde_plasma_window_management` wayland protocol to regular clients.
// Without a workaround, loginext can only see XWayland windows on a Plasma
// session, and native Wayland apps (Konsole, Dolphin, Kate, modern Firefox,
// VS Code, …) silently bypass per-app rules. callDBus is one of the few
// outbound channels available to a KWin script — so we use it to push the
// resourceClass / resourceName of every newly-activated window into the
// daemon's `org.loginext.WindowFocus.Activated(ss)` D-Bus method.
//
// Hot-path discipline (yes, even here): this script runs inside KWin's
// event loop, so it MUST stay cheap. One callDBus per focus change, plus
// a low-rate (≈ every 2 s) heartbeat that re-publishes the current active
// window so a freshly-restarted loginext daemon picks up the focused app
// immediately rather than waiting for the user to switch windows. The
// daemon's `publish_and_log` dedupes on the FNV-1a hash, so the heartbeat
// produces zero log lines while the focus is unchanged.

function publish(window) {
    if (!window) {
        return;
    }
    // Plasma 6 exposes resourceClass / resourceName as QStrings on Window.
    // Plasma 5's Client object uses the same property names. Coerce to JS
    // strings explicitly because callDBus's signature matcher is strict
    // about string-vs-undefined and KWin sometimes returns a QVariant.
    var resourceClass = window.resourceClass ? "" + window.resourceClass : "";
    var resourceName  = window.resourceName  ? "" + window.resourceName  : "";

    callDBus(
        "org.loginext.WindowFocus",       // bus name claimed by the daemon
        "/org/loginext/WindowFocus",      // object path
        "org.loginext.WindowFocus",       // interface
        "Activated",                      // method
        resourceClass,
        resourceName
    );
}

function publishCurrent() {
    // `activeWindow` is the Plasma 6 property, `activeClient` the Plasma 5
    // fallback. Either short-circuits cleanly to undefined on a desktop
    // with no focused window (lock screen, all-minimised, etc.), and the
    // null-guard inside `publish` then no-ops.
    publish(workspace.activeWindow || workspace.activeClient);
}

// Plasma 6 renamed clientActivated → windowActivated (Window object instead
// of Client). Try the modern signal first, fall back to the legacy one so
// the same script package works on Plasma 5.27+ too.
try {
    workspace.windowActivated.connect(publish);
} catch (e) {
    workspace.clientActivated.connect(publish);
}

// Publish at script load so the daemon knows the focused window the
// instant it bound the bus name. Without this, the daemon would sit at
// "no app" until the user changed focus — bad UX on first launch.
publishCurrent();

// Heartbeat. The daemon's `publish_and_log` returns early when the FNV-1a
// hash hasn't changed, so this fires zero log lines / zero work on the
// hot path while the user is on a single window. The point is to recover
// quickly when:
//   • the daemon restarts (UI toggle, systemctl restart, crash + respawn)
//   • the bus name changes ownership for any reason
// 2 s is an order of magnitude faster than a user notices, while keeping
// per-second IPC cost essentially zero on a quiet bus.
//
// We try Plasma 6's QML-engine setInterval first; if the script is
// running on an older / stricter QtScript engine, we fall back to a
// QTimer and finally to publishing on additional workspace events to
// widen the natural catch surface. Three tiers means the script keeps
// "just works" semantics across the supported Plasma versions.
(function startHeartbeat() {
    if (typeof setInterval === "function") {
        setInterval(publishCurrent, 2000);
        return;
    }
    try {
        var timer = new QTimer();
        timer.interval = 2000;
        timer.timeout.connect(publishCurrent);
        timer.start();
        return;
    } catch (eTimer) {
        // No timer API — widen the event coverage so the daemon catches
        // the active window from related signals.
        try { workspace.windowAdded.connect(publish);   } catch (e1) {}
        try { workspace.clientAdded.connect(publish);   } catch (e2) {}
        try { workspace.currentDesktopChanged.connect(publishCurrent); } catch (e3) {}
    }
})();
