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
// event loop, so it MUST stay cheap. One callDBus per focus change, no
// allocations beyond the strings KWin already holds, no polling, no
// timers. Anything more would degrade compositor responsiveness.

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

// Plasma 6 renamed clientActivated → windowActivated (Window object instead
// of Client). Try the modern signal first, fall back to the legacy one so
// the same script package works on Plasma 5.27+ too.
try {
    workspace.windowActivated.connect(publish);
} catch (e) {
    workspace.clientActivated.connect(publish);
}

// Publish the currently-focused window once at script load. Without this,
// the daemon would sit at "global preset" until the user clicks a different
// window — surprising on first launch.
var initial = workspace.activeWindow || workspace.activeClient;
if (initial) {
    publish(initial);
}
