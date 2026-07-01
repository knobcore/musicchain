// Bopwire web player — runtime config. Plain global, no build step.
// Override the gateway at runtime for local testing: bopwire.com/?gateway=http://localhost:8090
window.BOPWIRE = {
  // The C++ gateway, fronted by Caddy with auto-TLS. The browser talks ONLY here.
  gateway: (new URLSearchParams(location.search).get('gateway')
            || 'https://api.bopwire.com').replace(/\/+$/, ''),

  // Auto-refresh the Discover feed on this cadence (ms), mirroring the app's 20s.
  refreshMs: 20000,

  // "Click here" → download the native app (where the listener DOES earn).
  // Release assets live on knobcore/bopwire (releases/latest).
  downloads: {
    linux:   'https://github.com/knobcore/bopwire/releases/latest/download/bopwire-linux-x86_64.AppImage',
    android: 'https://github.com/knobcore/bopwire/releases/latest/download/bopwire-android.apk',
    windows: 'https://github.com/knobcore/bopwire/releases/latest/download/bopwire-windows-x64-setup.exe',
  },
};
