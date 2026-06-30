package com.example.bopwire_player

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import androidx.core.app.NotificationCompat

/**
 * Foreground service whose only job is to keep the Flutter process
 * alive when the user backgrounds the app or turns off the screen.
 * The Dart isolate holds the librats client + PlayerServer; if the OS
 * decides the app is "cached" and reclaims it, both shut down and we
 * disappear from the swarm. Pinning a foreground service with an
 * ongoing notification stops Android from putting us in the cached
 * bucket — the connection survives, audio keeps streaming, and other
 * peers can keep fetching from us.
 *
 * We also hold a partial wake lock so the CPU keeps ticking when the
 * screen is off (network IO + audio decode would otherwise stall).
 */
class BopwireService : Service() {
    private var wakeLock: PowerManager.WakeLock? = null

    override fun onCreate() {
        super.onCreate()
        ensureChannel()
        val notif = NotificationCompat.Builder(this, CHANNEL_ID)
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentTitle("Bopwire")
            .setContentText("Connected to the swarm")
            .setOngoing(true)
            .setSilent(true)
            .setPriority(NotificationCompat.PRIORITY_LOW)
            .build()
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.UPSIDE_DOWN_CAKE) {
            startForeground(
                NOTIF_ID, notif,
                ServiceInfo.FOREGROUND_SERVICE_TYPE_DATA_SYNC,
            )
        } else {
            startForeground(NOTIF_ID, notif)
        }
        val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
        wakeLock = pm.newWakeLock(
            PowerManager.PARTIAL_WAKE_LOCK,
            "Bopwire:swarm-link"
        )
        // 24-hour cap is a safety net — Android logs a warning if a
        // wake lock is held indefinitely, and a process death frees it
        // anyway.
        //
        // Wake lock alone is NOT sufficient on cellular: Doze will still
        // throttle the Dart isolate ~30 s after screen-off, killing
        // librats RX. MainActivity now prompts the user once for the
        // REQUEST_IGNORE_BATTERY_OPTIMIZATIONS exemption — without that
        // whitelist this wake lock keeps the CPU ticking but the network
        // stack is still suspended by Doze.
        wakeLock?.acquire(24L * 60L * 60L * 1000L)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // START_STICKY: if the OS kills us under memory pressure, get
        // recreated when memory frees so the rats client comes back.
        return START_STICKY
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onDestroy() {
        wakeLock?.let { if (it.isHeld) it.release() }
        wakeLock = null
        super.onDestroy()
    }

    private fun ensureChannel() {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return
        val nm = getSystemService(NotificationManager::class.java)
        if (nm.getNotificationChannel(CHANNEL_ID) != null) return
        val ch = NotificationChannel(
            CHANNEL_ID,
            "Bopwire background",
            NotificationManager.IMPORTANCE_LOW,
        ).apply {
            description = "Keeps the swarm link alive when the app is in the background"
            setShowBadge(false)
        }
        nm.createNotificationChannel(ch)
    }

    companion object {
        private const val CHANNEL_ID = "bopwire_swarm_link"
        private const val NOTIF_ID   = 1001

        /** Idempotent — calling start when already started is a no-op. */
        fun start(ctx: Context) {
            val intent = Intent(ctx, BopwireService::class.java)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.O) {
                ctx.startForegroundService(intent)
            } else {
                ctx.startService(intent)
            }
        }
    }
}
