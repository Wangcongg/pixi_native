package org.pixinative.runtime

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.media.MediaDataSource
import android.media.MediaMetadataRetriever
import android.media.MediaPlayer
import android.media.PlaybackParams
import android.os.Handler
import android.util.Log
import java.util.ArrayDeque
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors

/**
 * HTMLAudio 后端。JS Audio 句柄保存音频数据和播放意图，只有实际播放期间持有 MediaPlayer。
 * Fire OS 实测 prepare 就会分配 AudioTrack；将每个 source/alias 和播完的 clone 一直保持
 * Prepared 会耗尽 AudioFlinger 资源，导致后续背景音乐/音效 error(1,-19)。
 * load/clone 只读取/共享元数据；pause、ended、free 和后台释放物理播放器，保留续播位置。
 * 构建/设置数据源/prepare 在准备队列独占执行，完成后把播放器所有权交给主线程。
 * 准备期间主线程只改播放意图，取消/换源后的旧结果只释放，不能恢复播放。
 * 不限制并发播放，不回收仍在播放或等待前台/焦点恢复的句柄。释放仍放到后台队列，避免阻塞帧。
 * 所有句柄状态及 MediaPlayer 回调都在主线程；异步 release 前摘掉监听，旧回调还须校验实例。
 */
class AudioBridge(context: Context, private val main: Handler, private val onEnded: (Int) -> Unit) {
    private class ByteSource(val bytes: ByteArray) : MediaDataSource() {
        override fun readAt(position: Long, buffer: ByteArray, offset: Int, size: Int): Int {
            if (position < 0 || position >= bytes.size) return -1
            val n = minOf(size, bytes.size - position.toInt())
            System.arraycopy(bytes, position.toInt(), buffer, offset, n)
            return n
        }
        override fun getSize(): Long = bytes.size.toLong()
        override fun close() {}
    }

    private class Clip(val bytes: ByteArray, val hash: Int, val durationMs: Int)
    private class Preparation { @Volatile var cancelled = false }
    private class Voice(val clip: Clip) {
        var player: MediaPlayer? = null
        var preparation: Preparation? = null
        var positionMs = 0
        var completed = false
        var volume = 1f
        var looping = false
        var speed = 1f
    }
    private val voices = HashMap<Int, Voice>()
    private val srcIds = HashSet<Int>()
    // 最近释放的 source 只缓存字节和元数据，不缓存 Prepared 播放器/音轨。
    private val pooledSourceIds = ArrayDeque<Int>()
    private val busy = HashSet<Int>()
    // 不用 MediaPlayer.isPlaying 查询：它在错误状态可能阻塞并再次触发错误回调。
    private val started = HashSet<MediaPlayer>()
    private val appliedPlaybackRates = HashMap<MediaPlayer, Float>()
    private val prepareExecutor: ExecutorService = Executors.newSingleThreadExecutor { task ->
        Thread(task, "PixiAudioPrepare").apply { isDaemon = true }
    }
    private var pendingPreparations = 0
    private val releaseExecutor: ExecutorService = Executors.newSingleThreadExecutor { task ->
        Thread(task, "PixiAudioRelease").apply { isDaemon = true }
    }
    private var destroyed = false

    private val attributes = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_GAME)
        .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build()
    private val audioManager = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
    private var focusRequest: AudioFocusRequest? = null
    private var focusHeld = false
    private var backgrounded = false
    private var interrupted = false
    private val focusListener = AudioManager.OnAudioFocusChangeListener { change ->
        when (change) {
            AudioManager.AUDIOFOCUS_LOSS, AudioManager.AUDIOFOCUS_LOSS_TRANSIENT -> {
                focusHeld = false
                interrupted = true
                pauseAll(if (change == AudioManager.AUDIOFOCUS_LOSS) "focus-loss" else "focus-loss-transient")
            }
            AudioManager.AUDIOFOCUS_GAIN -> {
                focusHeld = true
                interrupted = false
                resumeBusy("focus-gain")
            }
            else -> Unit // 与既有逻辑一致：LOSS_TRANSIENT_CAN_DUCK 不压低游戏音。
        }
    }

    private fun readClip(aid: Int, bytes: ByteArray): Clip? {
        if (bytes.isEmpty()) return null
        val hash = bytes.contentHashCode()
        // JNI 每次传入都会创建新 ByteArray；hash 筛选后比较内容，防止碰撞误复用。
        voices.values.firstOrNull {
            it.clip.bytes === bytes || (it.clip.hash == hash && it.clip.bytes.contentEquals(bytes))
        }?.let { return it.clip }
        val metadata = MediaMetadataRetriever()
        try {
            metadata.setDataSource(ByteSource(bytes))
            val duration = metadata.extractMetadata(MediaMetadataRetriever.METADATA_KEY_DURATION)?.toIntOrNull()
            if (duration == null || duration < 0) {
                Log.e(TAG, "[audio] #$aid duration unavailable (${bytes.size} B)")
                return null
            }
            return Clip(bytes, hash, duration)
        } catch (e: Exception) {
            Log.e(TAG, "[audio] #$aid metadata failed (${bytes.size} B): ${e.message}")
            return null
        } finally {
            metadata.release()
        }
    }

    private fun makeVoice(aid: Int, clip: Clip, isSource: Boolean): Double {
        // pixi-sound 反复 clone 而不 free；复用同内容且已空闲的非 source 句柄，避免句柄/字节累积。
        val reuseKey = voices.entries.firstOrNull { (key, voice) ->
            key != aid && key !in srcIds && key !in busy && voice.player == null && voice.clip === clip
        }?.key
        if (reuseKey != null) detach(reuseKey)
        detach(aid)
        voices[aid] = Voice(clip)
        if (isSource) srcIds.add(aid)
        return clip.durationMs / 1000.0
    }

    /** aid<0 表示 JS 持有的 source/URL Audio，op6 之前不可被 clone 池复用。 */
    fun load(aidRaw: Int, bytes: ByteArray): Double {
        val isSource = aidRaw < 0
        val aid = if (isSource) -aidRaw - 1 else aidRaw
        val clip = readClip(aid, bytes) ?: return -1.0
        return makeVoice(aid, clip, isSource)
    }

    fun clone(nid: Int, sid: Int): Double {
        val clip = voices[sid]?.clip ?: return -1.0
        return makeVoice(nid, clip, false)
    }

    private fun startVoice(aid: Int, voice: Voice) {
        if (destroyed) return
        val p = voice.player
        if (p != null) {
            playPrepared(voice, p)
            return
        }
        if (voice.preparation != null) return
        val preparation = Preparation()
        voice.preparation = preparation
        pendingPreparations++
        prepareExecutor.execute {
            var created: MediaPlayer? = null
            var failure: Exception? = null
            try {
                if (!preparation.cancelled) {
                    // No Looper on this executor: MediaPlayer events are delivered on the main
                    // Looper. The player is unpublished and exclusively owned here until post().
                    val player = MediaPlayer()
                    created = player
                    player.setAudioAttributes(attributes)
                    player.setDataSource(ByteSource(voice.clip.bytes))
                    player.prepare()
                }
            } catch (e: Exception) {
                failure = e
            }
            val player = created
            val error = failure
            main.post {
                val current = voices[aid] === voice && voice.preparation === preparation
                if (current) voice.preparation = null
                if (error != null) {
                    Log.e(TAG, "[audio] #$aid prepare failed (${voice.clip.bytes.size} B): ${error.message}")
                    if (current) busy.remove(aid)
                }
                if (player != null && error == null && current && !preparation.cancelled &&
                    aid in busy && !backgrounded && !interrupted && !destroyed) {
                    voice.player = player
                    appliedPlaybackRates[player] = 1f
                    try {
                        attachListeners(aid, voice, player)
                        player.setVolume(voice.volume, voice.volume)
                        if (voice.positionMs > 0) player.seekTo(voice.positionMs)
                        playPrepared(voice, player)
                    } catch (e: Exception) {
                        Log.e(TAG, "[audio] #$aid prepared start failed: ${e.message}")
                        busy.remove(aid)
                        retirePlayer(voice, keepPosition = false, pause = false)
                    }
                } else if (player != null) releaseAsync(player)
                pendingPreparations--
                // A result arriving after destroy still owns a physical player; drain its
                // release before closing the queue, even though its JS voice no longer exists.
                if (destroyed && pendingPreparations == 0) releaseExecutor.shutdown()
            }
        }
    }

    private fun attachListeners(aid: Int, voice: Voice, player: MediaPlayer) {
        player.setOnCompletionListener { mp ->
            if (voices[aid] !== voice || voice.player !== mp) return@setOnCompletionListener
            busy.remove(aid)
            voice.positionMs = voice.clip.durationMs
            voice.completed = true
            retirePlayer(voice, keepPosition = false, pause = false)
            onEnded(aid)
        }
        player.setOnErrorListener { mp, what, extra ->
            if (voices[aid] === voice && voice.player === mp) {
                Log.e(TAG, "[audio] #$aid error what=$what extra=$extra (${voice.clip.bytes.size} B)")
                busy.remove(aid)
                // 保留 JS 句柄/源数据，清除错误播放器；不自动重试，不吞掉故障日志。
                retirePlayer(voice, keepPosition = false, pause = false)
            }
            true
        }
    }

    private fun playPrepared(voice: Voice, p: MediaPlayer) {
        p.isLooping = voice.looping
        p.start()
        started.add(p)
        applyPlaybackRate(p, voice.speed)
    }

    /** 暂停立即停声，保存位置后摘除物理播放器；JS 句柄和音频字节仍有效。 */
    private fun retirePlayer(voice: Voice, keepPosition: Boolean, pause: Boolean = true) {
        voice.preparation?.cancelled = true
        voice.preparation = null
        val p = voice.player ?: return
        voice.player = null
        val wasStarted = started.remove(p)
        try {
            if (wasStarted && pause) p.pause()
            if (keepPosition) voice.positionMs = p.currentPosition
        } catch (e: Exception) {
            Log.w(TAG, "[audio] retire failed: ${e.message}")
        } finally {
            appliedPlaybackRates.remove(p)
            releaseAsync(p)
        }
    }

    private fun detach(aid: Int) {
        val voice = voices.remove(aid) ?: return
        busy.remove(aid)
        srcIds.remove(aid)
        pooledSourceIds.remove(aid)
        retirePlayer(voice, keepPosition = false)
    }

    // PlaybackParams 在暂停态可能隐式 start；只在 Started 后且速率改变时下发。
    private fun applyPlaybackRate(p: MediaPlayer, speed: Float) {
        if (appliedPlaybackRates[p] == speed) return
        p.playbackParams = PlaybackParams().setSpeed(speed)
        appliedPlaybackRates[p] = speed
    }

    private fun releaseAsync(p: MediaPlayer) {
        p.setOnCompletionListener(null)
        p.setOnErrorListener(null)
        releaseExecutor.execute {
            try { p.release() } catch (e: Exception) {
                Log.w(TAG, "[audio] async release failed: ${e.message}")
            }
        }
    }

    /** op 0=play(v1=loop,v2=fromSec) 1=pause 2=stop 3=volume 4=rate 5=seek 6=free 7=currentTime */
    fun ctl(aid: Int, op: Int, v1: Double, v2: Double): Double {
        val voice = voices[aid] ?: return 0.0
        try {
            when (op) {
                0 -> {
                    if (voice.completed) voice.positionMs = 0
                    voice.completed = false
                    voice.looping = v1 != 0.0
                    if (v2 > 0) {
                        voice.positionMs = (v2 * 1000).toInt()
                        voice.player?.seekTo(voice.positionMs)
                    }
                    busy.add(aid)
                    if (backgrounded) return 0.0
                    if (interrupted) { resumeBusy("play"); return 0.0 }
                    ensureFocus()
                    startVoice(aid, voice)
                }
                1 -> {
                    busy.remove(aid)
                    retirePlayer(voice, keepPosition = true)
                }
                2 -> {
                    busy.remove(aid)
                    retirePlayer(voice, keepPosition = false)
                    voice.positionMs = 0
                    voice.completed = false
                }
                3 -> {
                    voice.volume = v1.toFloat().coerceIn(0f, 1f)
                    voice.player?.setVolume(voice.volume, voice.volume)
                }
                4 -> if (v1 in 0.5..2.0) {
                    voice.speed = v1.toFloat()
                    voice.player?.let { p -> if (p in started) applyPlaybackRate(p, voice.speed) }
                }
                5 -> {
                    voice.positionMs = (maxOf(0.0, v1) * 1000).toInt()
                    voice.completed = false
                    voice.player?.seekTo(voice.positionMs)
                }
                6 -> {
                    busy.remove(aid)
                    retirePlayer(voice, keepPosition = false)
                    if (srcIds.remove(aid)) {
                        pooledSourceIds.remove(aid)
                        pooledSourceIds.addLast(aid)
                        while (pooledSourceIds.size > MAX_POOLED_SOURCES) detach(pooledSourceIds.first)
                    } else detach(aid)
                }
                7 -> return (voice.player?.currentPosition ?: voice.positionMs) / 1000.0
            }
        } catch (e: Exception) {
            Log.e(TAG, "[audio] #$aid op=$op failed: ${e.message}")
            if (op == 0) {
                busy.remove(aid)
                retirePlayer(voice, keepPosition = false, pause = false)
            }
        }
        return 0.0
    }

    // ---- 生命周期 ----
    fun setBackgrounded(value: Boolean) {
        if (backgrounded == value) return
        backgrounded = value
        if (value) {
            pauseAll("background")
            abandonFocus()
        } else resumeBusy("foreground")
    }

    fun shutdown() {
        val count = voices.size
        for (voice in voices.values) retirePlayer(voice, keepPosition = false)
        voices.clear(); srcIds.clear(); pooledSourceIds.clear(); busy.clear()
        started.clear(); appliedPlaybackRates.clear()
        abandonFocus()
        if (count > 0) Log.i(TAG, "[audio] shutdown: cleared $count voices")
    }

    fun destroy() {
        shutdown()
        destroyed = true
        prepareExecutor.shutdown()
        if (pendingPreparations == 0) releaseExecutor.shutdown()
    }

    private fun pauseAll(reason: String) {
        val count = started.size
        for (voice in voices.values) retirePlayer(voice, keepPosition = true)
        Log.i(TAG, "[audio] $reason: paused $count, busy=${busy.size}")
    }

    private fun resumeBusy(reason: String) {
        if (backgrounded || busy.isEmpty()) return
        if (!ensureFocus()) {
            Log.i(TAG, "[audio] $reason: focus unavailable, ${busy.size} deferred")
            return
        }
        interrupted = false
        var resumed = 0
        for (aid in busy.toList()) {
            val voice = voices[aid] ?: continue
            if (voice.player in started) continue
            try {
                startVoice(aid, voice)
                resumed++
            } catch (e: Exception) {
                Log.e(TAG, "[audio] #$aid $reason: resume failed: ${e.message}")
                busy.remove(aid)
                retirePlayer(voice, keepPosition = false, pause = false)
            }
        }
        Log.i(TAG, "[audio] $reason: resumed $resumed/${busy.size}")
    }

    private fun ensureFocus(): Boolean {
        if (focusHeld) return true
        val request = focusRequest ?: AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(attributes)
            .setWillPauseWhenDucked(false)
            .setOnAudioFocusChangeListener(focusListener, main)
            .build().also { focusRequest = it }
        val result = try { audioManager.requestAudioFocus(request) } catch (e: Exception) {
            Log.w(TAG, "[audio] focus request failed: ${e.message}"); AudioManager.AUDIOFOCUS_REQUEST_FAILED
        }
        focusHeld = result == AudioManager.AUDIOFOCUS_REQUEST_GRANTED
        if (!focusHeld) Log.i(TAG, "[audio] focus request result=$result")
        return focusHeld
    }

    private fun abandonFocus() {
        if (!focusHeld) return
        focusHeld = false
        focusRequest?.let { try { audioManager.abandonAudioFocusRequest(it) } catch (e: Exception) {
            Log.w(TAG, "[audio] abandon focus failed: ${e.message}")
        } }
    }

    private companion object {
        const val TAG = "PixiNative"
        const val MAX_POOLED_SOURCES = 16
    }
}
