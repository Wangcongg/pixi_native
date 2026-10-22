package org.pixinative.runtime

import android.util.Log
import okhttp3.Authenticator
import okhttp3.Call
import okhttp3.Cookie
import okhttp3.CookieJar
import okhttp3.EventListener
import okhttp3.HttpUrl
import okhttp3.Headers
import okhttp3.java.net.cookiejar.JavaNetCookieJar
import okhttp3.OkHttpClient
import okhttp3.Protocol
import okhttp3.Request
import okhttp3.RequestBody.Companion.toRequestBody
import java.net.CookieHandler
import java.net.InetSocketAddress
import java.net.Proxy
import java.util.concurrent.TimeUnit

/** Shared transport for native resource/XHR, version tables, policy and telemetry requests. */
internal object NativeHttpTransport {
    // Keep the process CookieHandler used by URLConnection, including handlers installed later.
    private val cookies = object : CookieJar {
        override fun loadForRequest(url: HttpUrl): List<Cookie> =
            CookieHandler.getDefault()?.let { JavaNetCookieJar(it).loadForRequest(url) } ?: emptyList()

        override fun saveFromResponse(url: HttpUrl, cookies: List<Cookie>) {
            CookieHandler.getDefault()?.let { JavaNetCookieJar(it).saveFromResponse(url, cookies) }
        }
    }

    val resources: OkHttpClient = OkHttpClient.Builder()
        .protocols(listOf(Protocol.HTTP_2, Protocol.HTTP_1_1))
        .followSslRedirects(false) // Preserve the prohibition on HTTP <-> HTTPS redirects.
        .connectTimeout(15, TimeUnit.SECONDS)
        .readTimeout(30, TimeUnit.SECONDS)
        .writeTimeout(0, TimeUnit.SECONDS) // URLConnection writes had no separate timeout.
        .cache(null) // The native, hash-verified resource cache remains the cache authority.
        .cookieJar(cookies)
        .authenticator(Authenticator.JAVA_NET_AUTHENTICATOR)
        .proxyAuthenticator(Authenticator.JAVA_NET_AUTHENTICATOR)
        .addInterceptor { chain ->
            val original = chain.request()
            val agent = System.getProperty("http.agent")
            val request = if (original.header("User-Agent") == null && agent != null) {
                // Preserve Android's URLConnection User-Agent, including its ASCII conversion.
                val ascii = agent.codePoints().toArray().joinToString("") {
                    if (it < 0x20 || it >= 0x7f) "?" else it.toChar().toString()
                }
                original.newBuilder().header("User-Agent", ascii).build()
            } else original
            chain.proceed(request)
        }
        .eventListener(object : EventListener() {
            override fun connectEnd(call: Call, inetSocketAddress: InetSocketAddress, proxy: Proxy, protocol: Protocol?) {
                // One diagnostic per new connection; omit paths, query strings and headers.
                Log.i("PixiNative", "[native-http] connected protocol=$protocol host=${call.request().url.host}")
            }
        })
        .build()

    // newBuilder shares the resource client's connection pool and dispatcher.
    val shortRead: OkHttpClient = resources.newBuilder().readTimeout(15, TimeUnit.SECONDS).build()

    /** Resource GET: preserve the original HTTP status/exception before the JNI null result. */
    fun fetchResource(url: String, client: OkHttpClient = resources,
                      diagnostic: (Int, Long, Exception?) -> Unit): ByteArray? {
        var status = 0
        var failure: Exception? = null
        val bytes = try {
            val request = Request.Builder().url(url.replace(" ", "%20"))
                .header("Cache-Control", "no-cache, no-store, must-revalidate").build()
            client.newCall(request).execute().use { response ->
                status = response.code
                if (status == 200) response.body.bytes() else null
            }
        } catch (error: Exception) {
            failure = error
            null
        }
        // Observability must not turn a successful download into a resource failure.
        try { diagnostic(status, bytes?.size?.toLong() ?: 0L, failure) }
        catch (error: Exception) { Log.e("PixiNative", "[resource-diagnostic] reporting failed", error) }
        return bytes?.takeIf { it.isNotEmpty() }
    }

    /** Retain URLConnection's method/body and default form Content-Type rules for the XHR bridge. */
    fun xhrRequest(method: String, url: String, headers: Headers, body: ByteArray?): Request {
        val hasBody = body != null && body.isNotEmpty()
        val verb = method.ifEmpty { "GET" }.let { if (it == "GET" && hasBody) "POST" else it }
        val builder = Request.Builder().url(url).headers(headers)
        if (verb != "GET" && verb != "HEAD" && headers["Content-Type"] == null)
            builder.header("Content-Type", "application/x-www-form-urlencoded")
        val payload = if (hasBody || verb in listOf("POST", "PUT", "PATCH", "PROPPATCH", "REPORT"))
            (body ?: ByteArray(0)).toRequestBody() else null
        return builder.method(verb, payload).build()
    }
}
